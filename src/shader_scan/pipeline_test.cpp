// SPDX-FileCopyrightText: 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// pipeline-test: build a real Vulkan graphics pipeline, headless, for the programs of a
// shader package on this machine's GPU. It goes through the pieces the Vulkan backend uses
// at run time (Vulkan::Device, its shader profile, Maxwell -> IR -> SPIR-V with one binding
// counter across the stages, DescriptorLayoutBuilder), so a pipeline that fails or is slow
// here fails or stutters in game too.
//
// usage: pipeline-test [--validate] <package> <index> <out dir> [pairs]
//   package/index: as for shader-scan (mtf_shaders.py output)
//   pairs: optional, one "<vs n> <fs n>" per line (n = unique program number, as in
//          shader-scan's info.tsv), e.g. the pairs a game run recorded. Without it every pixel
//          program is paired with the vertex program whose outputs cover its inputs most
//          tightly, and every vertex program left over with a compatible pixel program, so
//          each program is compiled at least once.
//   --validate: enable the Vulkan validation layer (messages go to the log in <out dir>)
// Output: <out dir>/pipelines.tsv (vs, fs, interface match, create ms, result) and a summary.
//
// Fixed-function state is a neutral default (triangle list, no culling, depth test, no
// blending, RGBA8 targets): the shaders decide whether a pipeline compiles; state only
// selects variants of the same compile.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_scan/package_env.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

/// --alpha: fragment stages alpha test against a push constant reference.
bool g_alpha_test = false;

using namespace ShaderScan;
using namespace Vulkan;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// The profile and host limits the Vulkan pipeline cache derives from the device.
Shader::Profile MakeProfile(const Device& device) {
    const auto& fc{device.FloatControlProperties()};
    const VkDriverIdKHR driver_id{device.GetDriverID()};
    const VkShaderStageFlags supported{device.GetSubgroupSupportedStages()};
    u32 subgroup_stages = 0;
    if (supported & VK_SHADER_STAGE_VERTEX_BIT) {
        subgroup_stages |= 1u << static_cast<u32>(Shader::Stage::VertexB);
    }
    if (supported & VK_SHADER_STAGE_FRAGMENT_BIT) {
        subgroup_stages |= 1u << static_cast<u32>(Shader::Stage::Fragment);
    }
    return Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_int8 = device.IsInt8Supported(),
        .support_uniform_and_storage_buffer_8bit =
            device.IsUniformAndStorageBuffer8BitAccessSupported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_uniform_and_storage_buffer_16bit =
            device.IsUniformAndStorageBuffer16BitAccessSupported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            fc.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            fc.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = fc.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = fc.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = fc.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = fc.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .supported_subgroup_stages = subgroup_stages,
        .support_viewport_index_layer_non_geometry = device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_demote_to_helper_invocation = device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),
        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                       driver_id == VK_DRIVER_ID_MESA_RADV ||
                                       driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                       driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA,
        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances(),
    };
}

Shader::HostTranslateInfo MakeHostInfo(const Device& device) {
    Shader::HostTranslateInfo info{
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_per_stage_descriptor_sampled_images = device.GetMaxPerStageDescriptorSampledImages(),
        .max_per_stage_resources = device.GetMaxPerStageResources(),
        .max_descriptor_set_samplers = device.GetMaxDescriptorSetSamplers(),
        .max_descriptor_set_uniform_buffers = device.GetMaxDescriptorSetUniformBuffers(),
        .max_descriptor_set_uniform_buffers_dynamic = device.GetMaxDescriptorSetUniformBuffersDynamic(),
        .max_descriptor_set_storage_buffers = device.GetMaxDescriptorSetStorageBuffers(),
        .max_descriptor_set_storage_buffers_dynamic = device.GetMaxDescriptorSetStorageBuffersDynamic(),
        .max_descriptor_set_sampled_images = device.GetMaxDescriptorSetSampledImages(),
        .max_descriptor_set_storage_images = device.GetMaxDescriptorSetStorageImages(),
        .max_descriptor_set_input_attachements = device.GetMaxDescriptorSetInputAttachments(),
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
    };
    info.ApplyDescriptorLimitPolicy();
    return info;
}

// A translated program; the IR lives in the pools, so they travel with it.
struct Translated {
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    std::optional<PackageEnvironment> env;
    Shader::IR::Program program;
};

std::unique_ptr<Translated> Translate(const PackageProgram& p, const Shader::HostTranslateInfo& host) {
    auto t = std::make_unique<Translated>();
    t->env.emplace(p.code, p.is_fragment ? Shader::Stage::Fragment : Shader::Stage::VertexB);
    Shader::Maxwell::Flow::CFG cfg(*t->env, t->flow_block, kProgramBase + kSphSize, !p.is_fragment);
    t->program = Shader::Maxwell::TranslateProgram(t->inst_pool, t->block_pool, *t->env, cfg, host);
    return t;
}

struct Masks {
    u32 in{};
    u32 out{};
    u32 colors{};
};

Masks GetMasks(const Shader::Info& info) {
    Masks m;
    for (u32 i = 0; i < 32; ++i) {
        m.in |= info.loads.Generic(i) ? 1u << i : 0;
        m.out |= info.stores.Generic(i) ? 1u << i : 0;
    }
    for (u32 i = 0; i < 8; ++i) {
        m.colors |= info.stores_frag_color[i] ? 1u << i : 0;
    }
    return m;
}

vk::RenderPass MakeRenderPass(const Device& device, u32 color_count) {
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_refs;
    for (u32 i = 0; i < color_count; ++i) {
        attachments.push_back({0, kColorFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                               VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                               VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_GENERAL});
        color_refs.push_back({i, VK_IMAGE_LAYOUT_GENERAL});
    }
    attachments.push_back({0, kDepthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                           VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD,
                           VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_GENERAL});
    const VkAttachmentReference depth_ref{color_count, VK_IMAGE_LAYOUT_GENERAL};
    const VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = color_count,
        .pColorAttachments = color_refs.data(),
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = &depth_ref,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    return device.GetLogical().CreateRenderPass({
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = static_cast<u32>(attachments.size()),
        .pAttachments = attachments.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    });
}

struct BuildResult {
    double emit_ms{};
    double create_ms{};
    std::string status{"ok"};
};

BuildResult BuildPipeline(const Device& device, const Shader::Profile& profile,
                     const Shader::HostTranslateInfo& host, const PackageProgram& vs_src,
                     const PackageProgram& fs_src, std::map<u32, vk::RenderPass>& render_passes) {
    BuildResult r;
    const auto t0 = std::chrono::steady_clock::now();
    auto vs = Translate(vs_src, host);
    auto fs = Translate(fs_src, host);

    // Runtime info as MakeRuntimeInfo builds it for a VertexB + Fragment pipeline.
    Shader::RuntimeInfo vs_rt{};
    vs_rt.previous_stage_stores.mask.set();
    for (auto& type : vs_rt.generic_input_types) {
        type = Shader::AttributeType::Float;
    }
    vs_rt.input_topology = Shader::InputTopology::Triangles;
    Shader::RuntimeInfo fs_rt{};
    fs_rt.previous_stage_stores = vs->program.info.stores;
    fs_rt.previous_stage_legacy_stores_mapping = vs->program.info.legacy_stores_mapping;
    fs_rt.input_topology = Shader::InputTopology::Triangles;
    if (g_alpha_test) {
        // Alpha test with the reference as a push constant, as the Vulkan backend does.
        fs_rt.alpha_test_func = Shader::CompareFunction::Greater;
        fs_rt.alpha_test_dynamic = true;
    }

    Shader::Backend::Bindings binding;
    Shader::Maxwell::ConvertLegacyToGeneric(vs->program, vs_rt);
    const std::vector<u32> vs_code =
        Shader::Backend::SPIRV::EmitSPIRV(profile, vs_rt, vs->program, binding);
    Shader::Maxwell::ConvertLegacyToGeneric(fs->program, fs_rt);
    const std::vector<u32> fs_code =
        Shader::Backend::SPIRV::EmitSPIRV(profile, fs_rt, fs->program, binding);

    const auto& dev = device.GetLogical();
    auto module = [&](const std::vector<u32>& code) {
        return dev.CreateShaderModule({
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = code.size() * sizeof(u32),
            .pCode = code.data(),
        });
    };
    const vk::ShaderModule vs_module = module(vs_code);
    const vk::ShaderModule fs_module = module(fs_code);

    DescriptorLayoutBuilder builder{device};
    builder.Add(vs->program.info, VK_SHADER_STAGE_VERTEX_BIT);
    builder.Add(fs->program.info, VK_SHADER_STAGE_FRAGMENT_BIT);
    const bool push = builder.CanUsePushDescriptor();
    const vk::DescriptorSetLayout set_layout = builder.CreateDescriptorSetLayout(push);
    const vk::PipelineLayout layout = builder.CreatePipelineLayout(*set_layout);

    const Masks vm = GetMasks(vs->program.info);
    const Masks fm = GetMasks(fs->program.info);
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (u32 i = 0; i < 32; ++i) {
        if (vm.in & (1u << i)) {
            attributes.push_back({i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
        }
    }
    const VkVertexInputBindingDescription vbinding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = attributes.empty() ? 0u : 1u,
        .pVertexBindingDescriptions = &vbinding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    const VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    const u32 color_count = std::max(1u, 32u - static_cast<u32>(std::countl_zero(fm.colors)));
    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(color_count);
    for (auto& a : blend_attachments) {
        a = {};
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = color_count;
    blend.pAttachments = blend_attachments.data();
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dynamic_states};
    auto& render_pass = render_passes[color_count];
    if (!render_pass) {
        render_pass = MakeRenderPass(device, color_count);
    }
    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         *vs_module, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, *fs_module, "main", nullptr},
    };
    const VkGraphicsPipelineCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = nullptr,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = *layout,
        .renderPass = *render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    const auto t1 = std::chrono::steady_clock::now();
    const vk::Pipeline pipeline = dev.CreateGraphicsPipeline(ci);
    const auto t2 = std::chrono::steady_clock::now();
    r.emit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.create_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    return r;
}

// Default pairing: each pixel program with the vertex program whose generic outputs cover
// its inputs most tightly (or overlap most, if none covers them), then each vertex program
// not used yet with the compatible pixel program reading the most of its outputs.
std::vector<std::pair<size_t, size_t>> DefaultPairs(const std::vector<PackageProgram>& programs,
                                                    const std::vector<Masks>& masks) {
    std::vector<size_t> vss, fss;
    for (const auto& p : programs) {
        (p.is_fragment ? fss : vss).push_back(p.n);
    }
    std::vector<std::pair<size_t, size_t>> pairs;
    std::vector<bool> vs_used(programs.size());
    for (const size_t f : fss) {
        const u32 need = masks[f].in;
        size_t best = vss.front();
        int best_score = -1000;
        for (const size_t v : vss) {
            const u32 have = masks[v].out;
            const int score = (have & need) == need ? 100 - std::popcount(have & ~need)
                                                    : std::popcount(have & need) - 64;
            if (score > best_score) {
                best_score = score;
                best = v;
            }
        }
        pairs.emplace_back(best, f);
        vs_used[best] = true;
    }
    for (const size_t v : vss) {
        if (vs_used[v]) {
            continue;
        }
        const u32 have = masks[v].out;
        size_t best = fss.front();
        int best_score = -1000;
        for (const size_t f : fss) {
            const u32 need = masks[f].in;
            const int score = (have & need) == need ? std::popcount(need) : -64;
            if (score > best_score) {
                best_score = score;
                best = f;
            }
        }
        pairs.emplace_back(v, best);
    }
    return pairs;
}

} // namespace

int main(int argc, char** argv) {
    int arg = 1;
    bool validate = false;
    if (arg < argc && std::string(argv[arg]) == "--alpha") {
        g_alpha_test = true;
        ++arg;
    }
    if (arg < argc && std::string(argv[arg]) == "--validate") {
        validate = true;
        ++arg;
    }
    if (argc - arg < 3) {
        std::fprintf(stderr, "usage: pipeline-test [--validate] <package> <index> <out dir> [pairs]\n");
        return 2;
    }
    const std::filesystem::path out_dir{argv[arg + 2]};
    std::filesystem::create_directories(out_dir);
    Common::FS::SetSuyuPath(Common::FS::SuyuPath::LogDir, out_dir);
    Common::Log::Initialize();
    Common::Log::Filter filter;
    filter.ParseFilterString("*:Error"); // validation messages are logged as errors
    Common::Log::SetGlobalFilter(filter);

    size_t total = 0;
    std::vector<std::string> bad;
    const auto programs = LoadUniquePrograms(argv[arg], argv[arg + 1], total, bad);
    std::printf("%zu programs, %zu unique\n", total, programs.size());

    try {
        vk::InstanceDispatch dld;
        const auto library = OpenLibrary();
        const vk::Instance instance = CreateInstance(
            *library, dld, VK_API_VERSION_1_1, Core::Frontend::WindowSystemType::Headless, validate);
        const vk::DebugUtilsMessenger messenger =
            validate ? CreateDebugUtilsCallback(instance) : vk::DebugUtilsMessenger{};
        const auto physical_devices = instance.EnumeratePhysicalDevices();
        if (physical_devices.empty()) {
            std::fprintf(stderr, "no Vulkan device\n");
            return 1;
        }
        const Device device(*instance, vk::PhysicalDevice(physical_devices[0], dld), nullptr, dld);
        std::printf("device: %s, driver %s\n", std::string(device.GetModelName()).c_str(),
                    device.GetDriverName().c_str());
        const auto profile = MakeProfile(device);
        const auto host = MakeHostInfo(device);

        std::vector<Masks> masks(programs.size());
        for (const auto& p : programs) {
            try {
                masks[p.n] = GetMasks(Translate(p, host)->program.info);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "program %zu does not translate: %s\n", p.n, e.what());
            }
        }

        std::vector<std::pair<size_t, size_t>> pairs;
        if (argc - arg > 3) {
            std::ifstream pf(argv[arg + 3]);
            size_t v, f;
            while (pf >> v >> f) {
                if (v < programs.size() && f < programs.size() && !programs[v].is_fragment &&
                    programs[f].is_fragment) {
                    pairs.emplace_back(v, f);
                }
            }
        } else {
            pairs = DefaultPairs(programs, masks);
        }

        std::ofstream out(out_dir / "pipelines.tsv");
        out << "# vs\tfs\tinterface\temit_ms\tcreate_ms\tresult\n";
        std::map<u32, vk::RenderPass> render_passes;
        std::vector<double> times;
        std::map<std::string, size_t> failures;
        size_t ok = 0, mismatched = 0;
        double total_ms = 0;
        for (size_t i = 0; i < pairs.size(); ++i) {
            const auto [v, f] = pairs[i];
            const bool match = (masks[v].out & masks[f].in) == masks[f].in;
            mismatched += !match;
            BuildResult r;
            try {
                r = BuildPipeline(device, profile, host, programs[v], programs[f], render_passes);
            } catch (const std::exception& e) {
                r.status = std::string("FAIL ") + e.what();
            }
            if (r.status == "ok") {
                ++ok;
                times.push_back(r.create_ms);
                total_ms += r.create_ms;
            } else {
                ++failures[r.status.substr(0, 120)];
            }
            char line[256];
            std::snprintf(line, sizeof(line), "%zu\t%zu\t%s\t%.2f\t%.2f\t", v, f,
                          match ? "match" : "partial", r.emit_ms, r.create_ms);
            out << line << r.status << '\n';
            if ((i + 1) % 250 == 0) {
                std::printf("  %zu/%zu pipelines\n", i + 1, pairs.size());
                std::fflush(stdout);
            }
        }
        std::sort(times.begin(), times.end());
        auto pct = [&](double q) {
            return times.empty() ? 0.0 : times[std::min(times.size() - 1, size_t(q * times.size()))];
        };
        char summary[512];
        std::snprintf(summary, sizeof(summary),
                      "# pipelines %zu, created %zu, failed %zu, interface partial %zu\n"
                      "# driver compile ms: total %.0f, median %.2f, p95 %.2f, max %.2f\n",
                      pairs.size(), ok, pairs.size() - ok, mismatched, total_ms, pct(0.5),
                      pct(0.95), times.empty() ? 0.0 : times.back());
        out << summary;
        std::printf("%s", summary);
        for (const auto& [kind, count] : failures) {
            out << "# " << count << "x " << kind << '\n';
            std::printf("  %zux %s\n", count, kind.c_str());
        }
        return pairs.size() == ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Vulkan setup failed: %s\n", e.what());
        return 1;
    }
}
