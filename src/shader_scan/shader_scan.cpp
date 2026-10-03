// SPDX-FileCopyrightText: 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// shader-scan: run every Maxwell program from a shader package through the
// shader recompiler (Maxwell -> IR -> SPIR-V) offline, the same calls the Vulkan
// pipeline cache makes at run time, and report which ones fail.
//
// usage: shader-scan <package> <index> <out dir>
//   package: raw bytes holding the programs (e.g. an extracted MT Framework SPK)
//   index:   one program per line, "<hex offset of the program header>\t<sph shader_type>\t<bytes>"
//   out dir: receives <n>_<vs|fs>.spv for each unique program and results.txt
//
// Anything a real draw would supply (texture types, constant buffer contents,
// the previous stage's outputs) gets a neutral default, so this checks that the
// translator handles every instruction and control-flow shape in the package,
// not that a particular draw renders correctly.

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_scan/package_env.h"

namespace {

using namespace ShaderScan;

Shader::Profile MakeProfile() {
    // A capable desktop Vulkan 1.3 device; the conservative fallbacks are what
    // the real pipeline cache would pick on weaker hardware.
    return Shader::Profile{
        .supported_spirv = 0x00010600,
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = true,
        .support_int8 = true,
        .support_uniform_and_storage_buffer_8bit = true,
        .support_int16 = true,
        .support_uniform_and_storage_buffer_16bit = true,
        .support_int64 = true,
        .support_vertex_instance_id = false,
        .support_float_controls = true,
        .support_separate_denorm_behavior = true,
        .support_separate_rounding_mode = true,
        .support_fp16_denorm_preserve = true,
        .support_fp32_denorm_preserve = true,
        .support_fp16_denorm_flush = true,
        .support_fp32_denorm_flush = true,
        .support_fp16_signed_zero_nan_preserve = true,
        .support_fp32_signed_zero_nan_preserve = true,
        .support_fp64_signed_zero_nan_preserve = true,
        .support_explicit_workgroup_layout = true,
        .support_vote = true,
        .support_viewport_index_layer_non_geometry = true,
        .support_viewport_mask = false,
        .support_typeless_image_loads = true,
        .support_demote_to_helper_invocation = true,
        .support_int64_atomics = true,
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = false,
        .support_native_ndc = true,
        .support_scaled_attributes = true,
        .support_multi_viewport = true,
        .support_geometry_streams = true,
        .support_sampled_image_array_nonuniform_indexing = true,
        .warp_size_potentially_larger_than_guest = false,
        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
    };
}

Shader::HostTranslateInfo MakeHostInfo() {
    Shader::HostTranslateInfo info{};
    info.min_ssbo_alignment = 16;
    info.support_float64 = true;
    info.support_float16 = true;
    info.support_int64 = true;
    info.support_snorm_render_buffer = true;
    info.support_viewport_index_layer = true;
    info.support_conditional_barrier = true;
    info.ApplyDescriptorLimitPolicy();
    return info;
}

// One tab-separated line of what the program needs from the host: constant buffers
// (index:bytes), textures (type@cbuf:offset[xcount]), images, storage/texel buffers,
// generic inputs/outputs (bit masks), colour outputs and a few feature flags.
std::string Describe(const Shader::Info& info) {
    std::ostringstream s;
    for (u32 i = 0; i < Shader::Info::MAX_CBUFS; ++i) {
        if (info.constant_buffer_mask & (1u << i)) {
            s << i << ':' << info.constant_buffer_used_sizes[i] << ' ';
        }
    }
    s << '\t';
    for (const auto& t : info.texture_descriptors) {
        s << static_cast<u32>(t.type) << (t.is_depth ? "d" : "") << '@' << t.cbuf_index << ':'
          << std::hex << t.cbuf_offset << std::dec;
        if (t.count != 1) {
            s << 'x' << t.count;
        }
        s << ' ';
    }
    s << '\t' << info.image_descriptors.size() << '\t' << info.storage_buffers_descriptors.size()
      << '\t' << info.texture_buffer_descriptors.size() << '\t';
    u32 in_mask = 0, out_mask = 0, colors = 0;
    for (u32 i = 0; i < 32; ++i) {
        in_mask |= info.loads.Generic(i) ? 1u << i : 0;
        out_mask |= info.stores.Generic(i) ? 1u << i : 0;
    }
    for (u32 i = 0; i < 8; ++i) {
        colors |= info.stores_frag_color[i] ? 1u << i : 0;
    }
    s << std::hex << in_mask << '\t' << out_mask << '\t' << colors << std::dec << '\t';
    const std::pair<bool, const char*> flags[] = {
        {info.loads.Legacy(), "legacy-in"},     {info.stores.Legacy(), "legacy-out"},
        {info.stores_frag_depth, "frag-depth"}, {info.uses_demote_to_helper_invocation, "discard"},
        {info.uses_derivatives, "derivatives"}, {info.uses_local_memory, "local-mem"},
        {info.uses_global_memory, "global-mem"}, {info.uses_fp16, "fp16"},
        {info.uses_fp64, "fp64"},               {info.loads_indexed_attributes, "indexed-in"},
        {info.uses_shadow_lod, "shadow-lod"},   {info.uses_sparse_residency, "sparse"},
    };
    for (const auto& [on, name] : flags) {
        if (on) {
            s << name << ' ';
        }
    }
    return s.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: shader-scan <package> <index> <out dir>\n");
        return 2;
    }
    const std::filesystem::path out_dir{argv[3]};
    std::filesystem::create_directories(out_dir);
    // The logger writes to the emulator's log folder unless told otherwise.
    Common::FS::SetSuyuPath(Common::FS::SuyuPath::LogDir, out_dir);
    Common::Log::Initialize();
    Common::Log::Filter filter;
    filter.ParseFilterString("*:Error");
    Common::Log::SetGlobalFilter(filter);

    const auto profile = MakeProfile();
    const auto host_info = MakeHostInfo();
    std::ofstream results(out_dir / "results.txt");
    std::ofstream info_out(out_dir / "info.tsv");
    info_out << "# n\tstage\toffset\tcbufs\ttextures\timages\tssbos\ttexbufs\tin_mask\tout_mask\t"
                "colors\tflags\n";
    size_t total = 0, ok = 0, failed = 0;
    std::vector<std::string> bad;
    auto programs = LoadUniquePrograms(argv[1], argv[2], total, bad);
    const size_t unique = programs.size();
    std::map<std::string, size_t> failure_kinds;
    for (const auto& off_hex : bad) {
        results << off_hex << "\tbad-index\n";
    }

    for (auto& p : programs) {
        const size_t n = p.n;
        const std::string& off_hex = p.offset;
        const u32 bytes = p.bytes;
        const bool is_fragment = p.is_fragment;
        const auto stage = is_fragment ? Shader::Stage::Fragment : Shader::Stage::VertexB;
        PackageEnvironment env{std::move(p.code), stage};

        std::string status = "ok";
        try {
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
            Shader::ObjectPool<Shader::IR::Inst> inst_pool;
            Shader::ObjectPool<Shader::IR::Block> block_pool;
            Shader::Maxwell::Flow::CFG cfg(env, flow_block, kProgramBase + kSphSize, !is_fragment);
            auto program =
                Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host_info);
            info_out << n << '\t' << (is_fragment ? "fs" : "vs") << '\t' << off_hex << '\t'
                     << Describe(program.info) << '\n';
            Shader::RuntimeInfo runtime_info{};
            for (auto& type : runtime_info.generic_input_types) {
                type = Shader::AttributeType::Float;
            }
            Shader::Maxwell::ConvertLegacyToGeneric(program, runtime_info);
            Shader::Backend::Bindings binding;
            const std::vector<u32> spirv =
                Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
            char name[64];
            std::snprintf(name, sizeof(name), "%05zu_%s.spv", n, is_fragment ? "fs" : "vs");
            std::ofstream(out_dir / name, std::ios::binary)
                .write(reinterpret_cast<const char*>(spirv.data()),
                       static_cast<std::streamsize>(spirv.size() * sizeof(u32)));
        } catch (const std::exception& e) {
            status = std::string("FAIL ") + e.what();
        } catch (...) {
            status = "FAIL unknown exception";
        }
        if (status == "ok") {
            ++ok;
        } else {
            ++failed;
            ++failure_kinds[status.substr(0, 120)];
        }
        results << n << '\t' << off_hex << '\t' << (is_fragment ? "fs" : "vs") << '\t' << bytes
                << '\t' << status << '\n';
        results.flush();
    }
    results << "# programs " << total << ", unique " << unique << ", translated " << ok
            << ", failed " << failed << '\n';
    for (const auto& [kind, count] : failure_kinds) {
        results << "# " << count << "x " << kind << '\n';
    }
    std::printf("programs %zu, unique %zu, translated %zu, failed %zu\n", total, unique, ok,
                failed);
    for (const auto& [kind, count] : failure_kinds) {
        std::printf("  %zux %s\n", count, kind.c_str());
    }
    return failed == 0 ? 0 : 1;
}
