// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// G3c, shadow stage: executes the NVN call lists the recorder (core/arm/recomp/nvn_rec) hands over at
// every queue submit, on Vulkan, into images of our own. Everything NVN-side comes from the recorded
// arguments and the objects they point at; guest memory is read with the pool map the game gave NVN.
// Texture descriptors are still read from the pools the guest driver fills (the TIC/TSC decoders are
// suyu's), until the native pool registration exists. Nothing here is shown; frames are dumped.

#include "common/host_key.h"
#include "common/thread.h"
#include <algorithm>
#include <atomic>
#include <set>
#include <array>
#include <cmath>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <thread>
#include <unordered_set>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include "shader_recompiler/frontend/ir/program.h"
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/log_redact.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#endif
#include "core/arm/recomp/game_settings.h"
#include "core/arm/recomp/nvn_rec.h"
#include "core/hle/kernel/stall_ring.h"
#include "core/memory.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/nvn_vk.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/host_shaders/vulkan_nvn_motion_vectors_comp_spv.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/memory_manager.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/format_lookup_table.h"
#include "video_core/texture_cache/formatter.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/texture.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef SUYU_HAS_FSR_VULKAN // [Debug] nvn_fsr (externals/fsr-vulkan); after suyu's Vulkan header (VK_NO_PROTOTYPES)
#include "video_core/host_shaders/vulkan_nvn_fsr_animsurf_comp_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_composite_comp_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_objmv_apply_comp_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_objmv_search_comp_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_objmv_classify_comp_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_objmv_search_list_comp_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_objvel_frag_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_objvel_merge_comp_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_objvel_vert_spv.h"
#include "video_core/host_shaders/vulkan_nvn_fsr_prep_comp_spv.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h> // AMD's jitter helpers, used to cross-check the jitter sequence
#include "ffx_vk_loader_shim.h"
#include "ffx_vk_portable.h"
#ifdef SUYU_HAS_FSR4_BRIDGE
#include "video_core/renderer_vulkan/nvn_fsr4.h" // [Debug] nvn_fsr_backend = 4
#endif
#endif

#ifdef _MSC_VER
#define NVN_VK_NOINLINE __declspec(noinline)
#else
#define NVN_VK_NOINLINE __attribute__((noinline))
#endif

namespace Vulkan {

namespace {

#ifdef _WIN32
// Runs fn(arg); an access violation inside it is swallowed (the renderer then switches itself off).
bool SehRun(void (*fn)(void*), void* arg) {
    __try {
        fn(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#else
bool SehRun(void (*fn)(void*), void* arg) {
    fn(arg);
    return true;
}
#endif

// Where the renderer is, for the crash note (see CrashHandler).
const char* volatile g_phase = "idle";
volatile u64 g_detail = 0;
volatile int g_line = 0;
#define PHASE(name, detail) (g_phase = name, g_detail = (u64)(detail), g_line = __LINE__)

namespace NR = Core::NvnRec;

/// Window-title text of the automatic benchmark ([Debug] nvn_bench); empty when no run is active or just finished.
std::mutex g_bench_status_mutex;
std::string g_bench_status;
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

constexpr u32 kSlots = 3;                          // frames in flight
constexpr size_t kArenaBytes = 160ull << 20;       // per-frame upload arena
constexpr u32 kMaxDumpFrames = 80;

std::string OptionsText() {
    std::ifstream f(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_vk_opts.txt");
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

// ---------------------------------------------------------------------------------------------
// Guest-memory environment for the shader recompiler
// ---------------------------------------------------------------------------------------------
struct TexBinding {
    u32 handle{};
};

static_assert(sizeof(Shader::ProgramHeader) == 0x50);

/// Everything one stage's translation asked of its environment, and what it got. Translation is
/// deterministic in these answers, so replaying them rebuilds the same program without guest memory.
struct StageTranscript {
    u32 stage{};
    u32 start_address{};
    std::array<u8, 0x50> sph{};
    std::map<u32, u64> instr_map;             // while capturing: relative instruction address -> word
    std::vector<std::pair<u32, u64>> instr;   // finished: the same, sorted by address
    std::map<std::pair<u32, u32>, u32> cbuf;  // (cbuf index, offset) -> value
    std::map<u32, u32> tex_type, tex_format; // raw handle -> answer
    u64 hash{};                               // content: stage, start, SPH and every instruction word read
    u64 quick{};                              // lookup index: stage, start, SPH and the first instruction word
};

inline void Fnv(u64& h, const void* data, size_t size) {
    const auto* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
}
template <typename T>
inline void FnvV(u64& h, const T& v) {
    Fnv(h, &v, sizeof(v));
}
inline u64 StageQuick(u32 stage, u32 start, const u8* sph, u64 first_word) {
    u64 h = 1469598103934665603ull;
    FnvV(h, stage);
    FnvV(h, start);
    Fnv(h, sph, 0x50);
    FnvV(h, first_word);
    return h;
}
/// Where translation reads the first instruction of a stage: the CFG starts at start + 0x50 and Location
/// skips the scheduling word at 32-byte boundaries (see Maxwell::Location::Align).
inline u32 FirstCodeAddress(u32 start_address) {
    const u32 a = start_address + 0x50;
    return a + (a % 32 == 0 ? 8 : 0);
}
inline const u64* FindInstr(const StageTranscript& t, u32 address) {
    const auto it = std::lower_bound(t.instr.begin(), t.instr.end(), address, [](const auto& e, u32 a) { return e.first < a; });
    return it != t.instr.end() && it->first == address ? &it->second : nullptr;
}
inline void FinishTranscript(StageTranscript& t) {
    if (!t.instr_map.empty()) {
        t.instr.assign(t.instr_map.begin(), t.instr_map.end());
        t.instr_map.clear();
    }
    u64 h = 1469598103934665603ull;
    FnvV(h, t.stage);
    FnvV(h, t.start_address);
    Fnv(h, t.sph.data(), t.sph.size());
    for (const auto& [a, v] : t.instr) {
        FnvV(h, a);
        FnvV(h, v);
    }
    t.hash = h;
    const u64* first = FindInstr(t, FirstCodeAddress(t.start_address));
    if (!first) throw std::runtime_error(fmt::format("shader cache: stage transcript has no instruction at its first code address {:x}", FirstCodeAddress(t.start_address)));
    t.quick = StageQuick(t.stage, t.start_address, t.sph.data(), *first);
}

/// Texture slots a stage's translation actually read a handle for (cbuf 2, 8 bytes per slot from 0x20).
/// Everything else bound to the other slots is stale leftover and must not be part of a cache key.
inline u32 UsedSlotMask(const StageTranscript& t) {
    u32 m = 0;
    for (const auto& [k, v] : t.cbuf) {
        if (k.first == 2 && k.second >= 0x20 && k.second < 0x20 + 8 * 8 && ((k.second - 0x20) & 7) == 0) m |= 1u << ((k.second - 0x20) / 8);
    }
    return m;
}

Tegra::Texture::TICEntry TicFor(Core::Memory::Memory& mem, u32 tex_pool, u32 handle) {
    Tegra::Texture::TICEntry tic{};
    u32 cpu = 0;
    if (NR::TextureDescriptor(tex_pool, handle & 0xfffff, cpu)) {
        mem.ReadBlock(cpu, &tic, sizeof(tic));
    }
    return tic;
}
Shader::TextureType TicTextureType(const Tegra::Texture::TICEntry& tic) {
    switch (tic.texture_type) {
    case Tegra::Texture::TextureType::Texture1D:
        return Shader::TextureType::Color1D;
    case Tegra::Texture::TextureType::Texture2D:
    case Tegra::Texture::TextureType::Texture2DNoMipmap:
        return tic.normalized_coords ? Shader::TextureType::Color2D : Shader::TextureType::Color2DRect;
    case Tegra::Texture::TextureType::Texture3D:
        return Shader::TextureType::Color3D;
    case Tegra::Texture::TextureType::TextureCubemap:
        return Shader::TextureType::ColorCube;
    case Tegra::Texture::TextureType::Texture1DArray:
        return Shader::TextureType::ColorArray1D;
    case Tegra::Texture::TextureType::Texture2DArray:
        return Shader::TextureType::ColorArray2D;
    case Tegra::Texture::TextureType::Texture1DBuffer:
        return Shader::TextureType::Buffer;
    case Tegra::Texture::TextureType::TextureCubeArray:
        return Shader::TextureType::ColorArrayCube;
    default:
        return Shader::TextureType::Color2D;
    }
}
Shader::TexturePixelFormat TicPixelFormat(const Tegra::Texture::TICEntry& tic) {
    return static_cast<Shader::TexturePixelFormat>(VideoCommon::PixelFormatFromTextureInfo(
        tic.format, tic.r_type, tic.g_type, tic.b_type, tic.a_type, tic.srgb_conversion));
}
/// Reads guest GPU memory the way the environment does: all or nothing (zeros).
void ReadGpuEnv(Core::Memory::Memory& mem, u64 va, void* dst, u32 size) {
    u32 cpu = 0, avail = 0;
    if (NR::GpuToCpu(mem, va, cpu, avail) && avail >= size) {
        mem.ReadBlock(cpu, dst, size);
    } else {
        std::memset(dst, 0, size);
    }
}

class NvnEnv final : public Shader::Environment {
public:
    NvnEnv(Core::Memory::Memory& mem_, u64 sph_va, Shader::Stage stage_, const std::array<u32, 8>& handles_,
           u32 tex_pool_, StageTranscript* rec_)
        : mem{&mem_}, handles{handles_}, tex_pool{tex_pool_}, rec{rec_} {
        start_address = static_cast<u32>(sph_va & 0xff);
        base = sph_va - start_address;
        stage = stage_;
        ReadGpuEnv(*mem, sph_va, &sph, sizeof(sph));
        local_memory_size = static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        if (rec) {
            rec->stage = static_cast<u32>(stage);
            rec->start_address = start_address;
            std::memcpy(rec->sph.data(), &sph, sizeof(sph));
        }
    }

    u64 ReadInstruction(u32 address) override {
        u64 v{};
        ReadGpuEnv(*mem, base + address, &v, sizeof(v));
        if (rec) rec->instr_map[address] = v;
        return v;
    }
    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override {
        u32 v = 0;
        // NVN keeps the bound texture handles in constant buffer 2, 8 bytes per slot from 0x20.
        if (cbuf_index == 2 && cbuf_offset >= 0x20 && cbuf_offset < 0x20 + 8 * 8 && ((cbuf_offset - 0x20) & 7) == 0) {
            v = handles[(cbuf_offset - 0x20) / 8];
        }
        if (rec) rec->cbuf[{cbuf_index, cbuf_offset}] = v;
        return v;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        const auto t = TicTextureType(TicFor(*mem, tex_pool, handle));
        if (rec) rec->tex_type[handle] = static_cast<u32>(t);
        return t;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        const auto f = TicPixelFormat(TicFor(*mem, tex_pool, handle));
        if (rec) rec->tex_format[handle] = static_cast<u32>(f);
        return f;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return VideoCore::Surface::IsPixelFormatInteger(
            static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 2;
    }
    u32 LocalMemorySize() const override {
        return local_memory_size;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    Core::Memory::Memory* mem;
    u64 base{};
    std::array<u32, 8> handles;
    u32 tex_pool{};
    u32 local_memory_size{};
    StageTranscript* rec{};
};

/// Replays a recorded transcript. Anything the translation asks that was not recorded is a hard error:
/// translation is deterministic, so it means the record does not belong to this code.
class TranscriptEnv final : public Shader::Environment {
public:
    explicit TranscriptEnv(const StageTranscript& t_) : t{t_} {
        stage = static_cast<Shader::Stage>(t.stage);
        start_address = t.start_address;
        std::memcpy(&sph, t.sph.data(), sizeof(sph));
        local_memory_size = static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
    }
    u64 ReadInstruction(u32 address) override {
        const u64* v = FindInstr(t, address);
        if (!v) throw std::runtime_error(fmt::format("shader cache: instruction {:x} not recorded", address));
        return *v;
    }
    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override {
        const auto it = t.cbuf.find({cbuf_index, cbuf_offset});
        if (it == t.cbuf.end()) throw std::runtime_error(fmt::format("shader cache: cbuf {} {:x} not recorded", cbuf_index, cbuf_offset));
        return it->second;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        const auto it = t.tex_type.find(handle);
        if (it == t.tex_type.end()) throw std::runtime_error(fmt::format("shader cache: texture type {:x} not recorded", handle));
        return static_cast<Shader::TextureType>(it->second);
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        const auto it = t.tex_format.find(handle);
        if (it == t.tex_format.end()) throw std::runtime_error(fmt::format("shader cache: texture format {:x} not recorded", handle));
        return static_cast<Shader::TexturePixelFormat>(it->second);
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return VideoCore::Surface::IsPixelFormatInteger(
            static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 2;
    }
    u32 LocalMemorySize() const override {
        return local_memory_size;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    const StageTranscript& t;
    u32 local_memory_size{};
};

/// [Debug] nvn_fsr_objvel: a copy of a translated vertex shader whose position output is also captured by transform
/// feedback (buffer 0, stride 16, offset 0: one vec4 clip position per vertex of every assembled primitive). Done on the
/// emitted SPIR-V words so the translation runs once: OpCapability TransformFeedback, OpExecutionMode <main> Xfb and
/// XfbBuffer / XfbStride / Offset on the variable decorated BuiltIn Position. Returns false (code untouched) when the
/// shape is not the expected one (no standalone Position variable, position in a block, xfb already present).
bool PatchXfbPosition(std::vector<u32>& code) {
    constexpr u32 kOpCapability = 17, kOpEntryPoint = 15, kOpExecutionMode = 16, kOpDecorate = 71, kOpMemberDecorate = 72;
    constexpr u32 kCapTransformFeedback = 53, kModeXfb = 11, kDecBuiltIn = 11, kBuiltInPosition = 0;
    constexpr u32 kDecOffset = 35, kDecXfbBuffer = 36, kDecXfbStride = 37;
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    size_t first_cap = 0, after_entry = 0, after_pos = 0;
    u32 main_id = 0, pos_id = 0;
    for (size_t i = 5; i < code.size();) {
        const u32 n = code[i] >> 16, op = code[i] & 0xffffu;
        if (n == 0 || i + n > code.size()) return false;
        if (op == kOpCapability) {
            if (first_cap == 0) first_cap = i;
            if (n >= 2 && code[i + 1] == kCapTransformFeedback) return false;
        } else if (op == kOpEntryPoint) {
            if (main_id != 0 || n < 3) return false; // one entry point expected
            main_id = code[i + 2];
            after_entry = i + n;
        } else if (op == kOpExecutionMode) {
            if (n >= 3 && code[i + 2] == kModeXfb) return false;
            if (after_entry != 0) after_entry = i + n; // execution modes follow the entry points
        } else if (op == kOpDecorate) {
            if (n >= 4 && code[i + 2] == kDecBuiltIn && code[i + 3] == kBuiltInPosition) {
                if (pos_id != 0) return false;
                pos_id = code[i + 1];
                after_pos = i + n;
            }
            if (n >= 3 && (code[i + 2] == kDecXfbBuffer || code[i + 2] == kDecXfbStride)) return false;
        } else if (op == kOpMemberDecorate) {
            if (n >= 5 && code[i + 3] == kDecBuiltIn && code[i + 4] == kBuiltInPosition) return false; // gl_PerVertex block
        }
        i += n;
    }
    if (first_cap == 0 || main_id == 0 || pos_id == 0 || after_entry == 0 || after_pos == 0 || after_pos < after_entry) return false;
    // insert back to front so the earlier positions stay valid
    // OpDecorate <id> <decoration> <literal>: 4 words each
    const std::array<u32, 12> d{(4u << 16) | kOpDecorate, pos_id, kDecXfbBuffer, 0u,
                                (4u << 16) | kOpDecorate, pos_id, kDecXfbStride, 16u,
                                (4u << 16) | kOpDecorate, pos_id, kDecOffset,    0u};
    code.insert(code.begin() + static_cast<std::ptrdiff_t>(after_pos), d.begin(), d.end());
    const std::array<u32, 3> mode{(3u << 16) | kOpExecutionMode, main_id, kModeXfb};
    code.insert(code.begin() + static_cast<std::ptrdiff_t>(after_entry), mode.begin(), mode.end());
    const std::array<u32, 2> cap{(2u << 16) | kOpCapability, kCapTransformFeedback};
    code.insert(code.begin() + static_cast<std::ptrdiff_t>(first_cap), cap.begin(), cap.end());
    return true;
}

// A translated program; the IR lives in the pools, so they travel with it.
struct Translated {
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block;
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    std::unique_ptr<Shader::Environment> env;
    Shader::IR::Program program;
};

// ---------------------------------------------------------------------------------------------
// State the recorded calls build up
// ---------------------------------------------------------------------------------------------
struct BlendTarget {
    u8 src_c{2}, dst_c{1}, src_a{2}, dst_a{1};
};
struct VertexBinding {
    u64 addr{};
    u32 size{};
};
struct RtRef {
    u32 tex{}; // NVN texture object
};
struct State {
    u64 prog[2]{};
    u64 ub_addr[2][16]{};
    u32 ub_size[2][16]{};
    u64 cb1[2]{}; // shader constant buffer 1 per stage, from the program object's command block (static review hypothesis)
    u32 tex_handle[2][8]{};
    VertexBinding vb[4]{};
    u32 attrib[16]{};
    u32 stride[4]{};
    BlendTarget blend[8]{};
    u32 color_state{0x08000000};
    u32 chan_mask{0xffffffff};
    u32 ds_flags{0};
    u32 poly_flags{0x10};
    bool depth_clamp{};
    f32 viewport[4]{};
    bool viewport_set{};
    s32 scissor[4]{};
    bool scissor_set{};
    f32 blend_color[4]{};
    f32 alpha_ref{};
    f32 poly_offset[3]{};
    bool prim_restart{};
    u32 rt_count{};
    u32 rt_tex[4]{};
    u32 depth_tex{};
    u32 tex_pool{};
    u32 sampler_pool{};
};

struct PipelineKey {
    u64 prog[2]{};
    u32 tex_sig[16]{};
    u32 attrib[16]{};
    u32 stride[4]{};
    u32 topology{};
    u32 poly{};
    u32 ds{};
    u32 alpha_func{};
    u32 chan_mask{};
    u32 blend_enable{};
    u8 blend[8][4]{};
    u32 rt_count{};
    u32 rt_format[4]{};
    u32 depth_format{};
    bool restart{};
    bool operator==(const PipelineKey&) const = default;
};
struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const noexcept {
        // Up to the last member only: the tail padding is not guaranteed to survive a copy.
        const auto* p = reinterpret_cast<const unsigned char*>(&k);
        size_t h = 1469598103934665603ull;
        for (size_t i = 0; i < offsetof(PipelineKey, restart) + sizeof(k.restart); ++i) h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }
};

struct ShaderKey {
    u64 prog[2]{};
    u32 tex_sig[16]{};
    u32 attrib_types[16]{};
    u32 alpha_func{};
    bool operator==(const ShaderKey&) const = default;
};
struct ShaderKeyHash {
    size_t operator()(const ShaderKey& k) const noexcept {
        // Up to the last member only: the tail padding is not guaranteed to survive a copy.
        const auto* p = reinterpret_cast<const unsigned char*>(&k);
        size_t h = 1469598103934665603ull;
        for (size_t i = 0; i < offsetof(ShaderKey, alpha_func) + sizeof(k.alpha_func); ++i) h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }
};

struct Program {
    vk::ShaderModule vs, fs;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::DescriptorUpdateTemplate update_template;
    bool push{};
    std::unique_ptr<Translated> t[2]; // dropped once the SPIR-V is emitted (they hold large pools)
    Shader::Info info[2];
    u32 vs_in_mask{};
    u32 fs_color_mask{};
    bool failed{};
    u64 content_key{}; // shader cache key (address free); 0 = not recordable
    // [Debug] nvn_fsr_objvel: the vertex shader with its position also written to transform feedback buffer 0
    // (PatchXfbPosition); empty when the feature is off or the patch did not apply
    vk::ShaderModule vs_xfb;
    // the vertex shader indexes a constant buffer dynamically (bone palettes / instance arrays: skinned or animated)
    bool vs_dyn_cbuf{};
    // the vertex shader samples textures (vertex texture fetch: possible bone / displacement textures)
    bool vs_tex{};
};

struct Pipeline {
    vk::Pipeline pipeline;
    Program* program{};
    bool failed{};
};

// ---------------------------------------------------------------------------------------------
// Persistent shader cache (user/nvn_vk_shaders.bin): the inputs of every program and pipeline the
// game used, keyed by content, never by guest address. Programs are stored as their translation
// transcripts, so a rebuild runs the same translator on the same answers.
// ---------------------------------------------------------------------------------------------
struct ProgramRecord {
    u64 key{};                // program content key
    u32 attrib_types[16]{};   // ShaderKey::attrib_types
    u32 alpha_func{};         // ShaderKey::alpha_func
    u8 attrib_enabled[16]{};
    u32 dbg{};                // nvn_vk_debug at build time (affects the build)
    u32 slots[2][8][3]{};     // per stage and texture slot: type, pixel format, first slot with the same handle
    StageTranscript stage[2];
};
struct PipelineRecord {
    u64 key{};                // pipeline content key
    u64 program{};            // ProgramRecord::key
    PipelineKey pk{};         // prog and tex_sig zeroed (address dependent; the program covers them)
    std::vector<VkVertexInputAttributeDescription> attributes;
    u8 flip_front{};
    u32 dbg{};
};
constexpr char kShaderCacheMagic[8] = {'N', 'V', 'N', 'V', 'K', 'S', 'H', 'C'};
constexpr u32 kShaderCacheVersion = 3; // 3: Eden recompiler sync; 2: program keys cover only the texture slots a stage samples

/// Largest 2D image side the GPU allows (VkPhysicalDeviceLimits::maxImageDimension2D); set when the renderer starts.
std::atomic<u32> g_max_image_dim{16384};

/// [Upscaler] nvn_fsr_split_scale: set once by RenderScaleFactor(). True = the scene targets use the (lower) render
/// scale and the present images / HUD layer are kept at 1X (the output); see FsrSplitScaleActive().
bool g_fsr_split{false};

/// suyu's Graphics > Resolution scale, applied to every render target of the replacement renderer
/// (the game's picture stays 1920x1080-based; targets, viewports and scissors are scaled).
float RenderScaleFactor() {
    static const float factor = [] {
        // The shader recompiler's rescaling pass bakes in the same Settings::values.resolution_info.
        Settings::UpdateRescalingInfo();
        auto& info = Settings::values.resolution_info;
        // FSR Quality modes with the split scale ([Upscaler] nvn_fsr_split_scale, default off): the render scale is the
        // mode's own fraction (1/256 steps) and only the scene targets use it; the output stays at 1X. Off (or not
        // applicable): the single-factor code below, unchanged.
        {
            Core::GameSettings::FsrSplit sp{};
            const bool split = Core::GameSettings::FsrSplitScale(&sp);
            const u32 limit = g_max_image_dim.load();
            const u32 base = std::max(Core::GameSettings::RenderWidth(), 1920u);
            if (split && base <= limit) {
                info.up_scale = sp.up_scale;
                info.down_shift = sp.down_shift;
                info.up_factor = static_cast<f32>(sp.render_scale);
                info.down_factor = 1.0f / info.up_factor;
                info.active = true;
                info.downscale = true;
                g_fsr_split = true;
                u32 gw = 0, gh = 0;
                Core::GameSettings::MvRenderSize(&gw, &gh);
                LOG_INFO(Render_Vulkan, "nvn_vk: FSR render {}x{} -> output {}x{} (split scale: {})",
                         std::max<long>(1, std::lround(gw * sp.render_scale)), std::max<long>(1, std::lround(gh * sp.render_scale)), gw,
                         gh, sp.why);
                LOG_INFO(Render_Vulkan, "nvn_vk: render scale x{} (GPU image limit {} px)", info.up_factor, limit);
                return info.up_factor;
            }
            if (Core::GameSettings::UpscalerChoiceNow() != Core::GameSettings::UpscalerChoice::Off) {
                LOG_INFO(Render_Vulkan, "nvn_vk: FSR split scale not used: {}", split ? "output wider than the GPU image limit" : sp.why);
            }
        }
        // An upscaler on the Direct renderer: the Quality mode picks the render scale (nearest available step) unless
        // [Upscaler] render_scale_override is on. Only the in-memory info changes; the saved resolution_setup is left alone.
        {
            const int saved = static_cast<int>(Settings::values.resolution_setup.GetValue());
            bool by_mode = false;
            std::string why;
            const int step = Core::GameSettings::EffectiveResolutionStep(saved, &by_mode, &why);
            if (by_mode) {
                Settings::TranslateResolutionInfo(static_cast<Settings::ResolutionSetup>(step), info);
                LOG_INFO(Render_Vulkan, "nvn_vk: upscaler render scale: {}", why);
            }
        }
        // The widest/tallest screen-sized target decides how far the scale can go on this GPU: a target
        // past the limit can't be allocated, and clamping one axis would squash the picture.
        const u32 limit = g_max_image_dim.load();
        const u32 base = std::max(Core::GameSettings::RenderWidth(), 1920u);
        const float cap = std::max(1.0f, std::floor(static_cast<float>(limit) / static_cast<float>(base)));
        if (info.up_factor > cap) {
            LOG_WARNING(Render_Vulkan, "nvn_vk: render scale x{} is too large for this GPU (images up to {} px, targets up to {} px wide); using x{}",
                        info.up_factor, limit, base, cap);
            // Only the in-memory scale changes (the shaders read it); the saved setting is left alone.
            info.up_scale = static_cast<u32>(cap);
            info.down_shift = 0;
            info.up_factor = cap;
            info.down_factor = 1.0f / cap;
            info.active = info.up_scale != 1;
            info.downscale = false;
        }
        LOG_INFO(Render_Vulkan, "nvn_vk: render scale x{} (GPU image limit {} px)", info.up_factor, limit);
        return info.up_factor;
    }();
    return factor;
}

/// Scaled size of a render target. If it would still pass the GPU limit (a target bigger than the screen),
/// both sides shrink by the same amount so the shape is kept.
void ScaledSize(u32 w, u32 h, u32& pw, u32& ph) {
    double f = RenderScaleFactor();
    const double limit = g_max_image_dim.load();
    f = std::min(f, limit / std::max(1u, std::max(w, h)));
    pw = std::clamp<u32>(static_cast<u32>(std::lround(w * f)), 1u, static_cast<u32>(limit));
    ph = std::clamp<u32>(static_cast<u32>(std::lround(h * f)), 1u, static_cast<u32>(limit));
}

/// The split render / output scale is in use (decided once, with the render scale).
bool FsrSplitScaleActive() {
    RenderScaleFactor();
    return g_fsr_split;
}

struct Image {
    vk::Image image;
    vk::ImageView view;
    vk::ImageView sampled_view; // depth images: depth aspect only, for sampling
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
    u32 width{}, height{};
    u32 pw{}, ph{}; // render targets: the allocated (scaled) size; 0 = same as width/height
    u32 cpu{}, bytes{};
    u64 last_use{};
    bool render_target{};
    bool out_scaled{}; // nvn_fsr_split_scale: a present image kept at 1X (pw = width); never marked as a scaled texture
    u32 layers{1}, levels{1};
    u64 draws{}, clears{};
    u64 spot{};        // sampled hash of the guest bytes when the image was built
    u64 spot_serial{}; // submit in which the guest bytes were last compared with `spot`
};

struct Frame {
    VkCommandBuffer cmd{};
    vk::Fence fence;
    vk::Buffer arena;
    size_t used{};
    bool pending{};
};

struct Stats {
    u64 submits{}, lists{}, recs{}, draws{}, draws_done{}, clears{};
    u64 shader_ok{}, shader_fail{}, pipeline_ok{}, pipeline_fail{};
    u64 images{}, image_fail{}, samplers{}, arena_full{}, presents{};
    double translate_ms{}, pipeline_ms{}, texture_ms{};
    std::map<std::string, u64> notes;
};

// ---------------------------------------------------------------------------------------------
// Always-on frame profiling (QueryPerformanceCounter). Frame boundary = Present. Writes
// user/nvn_prof.txt: one line per slow frame (wall > 25 ms or any single event > 10 ms) and a
// summary line once per second. Nothing is written per draw.
// ---------------------------------------------------------------------------------------------
enum ProfBucket : u32 {
    PB_Lock,        // waiting for the renderer lock (Submit / Present entry)
    PB_Replay,      // whole Submit body (prescan, pool builds, recording + everything below that runs in it)
    PB_Prescan,     // the dry walk over the lists that collects uncached programs and pipelines
    PB_Translate,   // new programs: wait for the pool's translate batch, plus any translated inline in the replay
    PB_Pipeline,    // new pipelines: wait for the pool's pipeline batch, plus inline ones, render passes, framebuffers
    PB_Texture,     // new sampled texture (read, unswizzle, image + view, staging) and new render targets
    PB_Buffer,      // guest buffer reads into the frame arena (Upload)
    PB_Submit,      // vkQueueSubmit of the frame command buffer
    PB_WaitFrame,   // BeginFrame: fence of the reused frame slot
    PB_WaitCapture, // capture only: occlusion query / pick readback fence waits
    PB_WaitPresent, // PresentCopy: waits for all pending frame fences
    PB_WaitUtil,    // PresentCopy: util fence after the blit submit
    PB_WaitDump,    // capture only: DumpImage / DumpDepth / HashTexture fence waits
    PB_Present,     // whole Present body (includes PresentCopy and its waits)
    PB_CacheSave,   // writing the Vulkan pipeline cache file (about once a minute when it grew)
    PB_Prewarm,     // guest thread waiting for a shader cache entry that the prewarm had not finished yet
    PB_GapPre,      // outside the renderer: end of the last Present to the first Submit of this frame (guest CPU work)
    PB_GapPost,     // outside the renderer: end of the last Submit to this frame's Present
    PB_Draw,        // whole DoDraw (nested: buffer/texture/pipeline buckets also count inside it); replay - draw = state ops + loop
    PB_DKey,        // DoDraw: MakeKey + GetPipeline lookup
    PB_DTex,        // DoDraw: TIC reads, SampledImage, Sampler lookups (new images also in texture)
    PB_DDesc,       // DoDraw: vertex/index/cbuf uploads + descriptor list build (copies also in buffer)
    PB_DRec,        // DoDraw: BeginPassFor + Vulkan command recording
    PB_Count
};
constexpr const char* kProfNames[PB_Count] = {"lock", "replay", "prescan", "translate", "pipeline", "texture", "buffer", "submit",
                                              "wait_frame", "wait_capture", "wait_present", "wait_util", "wait_dump", "present", "cache_save",
                                              "prewarm", "gap_pre", "gap_post", "draw", "d_key", "d_tex", "d_desc", "d_rec"};
// programs / pipelines: built (translated / created) this run; late_*: of those, the ones the prescan missed
// (built inline in the replay); warm_hits: programs + pipelines taken from the shader cache prewarm;
// cold_builds: programs + pipelines built because the cache did not have them.
enum ProfCount : u32 {
    PC_Programs, PC_Pipelines, PC_LatePrograms, PC_LatePipelines, PC_WarmHits, PC_ColdBuilds, PC_Textures, PC_TexRestream, PC_Bytes, PC_Draws, PC_Submits,
    PC_ArenaFull, PC_BytesVB, PC_BytesIB, PC_BytesCB1, PC_BytesUB, PC_DupVB, PC_DupIB, PC_DupCB1, PC_DupUB, PC_RebuiltFile, PC_RebuiltRun, PC_WarmRunHits, PC_ProgMiss, PC_Count
};
constexpr const char* kProfCountNames[PC_Count] = {"programs", "pipelines", "late_programs", "late_pipelines", "warm_hits", "cold_builds",
                                                   "textures", "tex_restream", "bytes", "draws", "submits", "arena_full",
                                                   "b_vb", "b_ib", "b_cb1", "b_ub", "dup_vb", "dup_ib", "dup_cb1", "dup_ub", "rebuilt_file", "rebuilt_run", "warm_run_hits", "prog_miss"};

inline u64 ProfNow() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Prof {
    double ms_per_tick{};
    u64 slow_frame_ticks{}, slow_event_ticks{}, second_ticks{};
    std::ofstream out;
    u64 frame{};
    u64 last_present{}, sec_start{};
    bool slow_event{};
    u64 f_tick[PB_Count]{}, f_max[PB_Count]{}, f_cnt[PC_Count]{};
    u64 s_tick[PB_Count]{}, s_max[PB_Count]{}, s_cnt[PC_Count]{};
    u64 s_frames{}, s_wall_sum{}, s_wall_max{}, s_slow{};
    bool on{}; // [Debug] gpu_diagnostics: off = no timing, no counting, nothing written

    // ---- user/nvn_perf.txt (only with gpu_diagnostics): per-second frame time percentiles, CPU stage split, GPU per-pass times,
    // and one X line per spike frame. Machine-readable (key=value); summarised by tools\perf-summary.ps1.
    std::ofstream perf;
    struct PerfFrame {
        float wall, gap, rcpu, wait, stall;
        u32 submits, draws;
    };
    std::vector<PerfFrame> perf_frames; // this second
    float perf_prev_p50{};
    u32 perf_spikes{};
    u32 perf_marks{};
    bool perf_key_down{};
    u64 perf_start{};
    /// Set by the renderer: GPU part of the second line (empty when GPU timestamps are off). Arg: presents this second.
    std::function<std::string(u64)> perf_gpu_summary;

    void OpenPerf(const std::filesystem::path& path) {
        if (!on) return;
        perf.open(path, std::ios::trunc);
        if (!perf) return;
        perf_start = ProfNow();
        perf << "# nvn_perf: S = one second (wall = present-to-present ms; gap = guest/game time outside the renderer, includes the game's own frame pacing sleep;\n"
                "# rcpu = renderer CPU ms per frame without waits; wait = renderer waits (frame fence, present drain, util, lock); stall = translate+pipeline+texture+prewarm+cache_save\n"
                "# (included in rcpu); gpu_* = sum of GPU timestamp spans of the frame's submits). g.<label>=avg ms per frame/max ms of one interval/avg draws per frame.\n"
                "# X = spike frame (all non-zero CPU buckets in ms). T = pass timeline of the slowest GPU submit of the last 5 s.\n";
        perf.flush();
    }
    static float Pct(std::vector<float>& v, double p) { // v sorted ascending
        if (v.empty()) return 0.0f;
        const size_t i = std::min<size_t>(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
        return v[i];
    }
    void PerfLine(const std::string& s) {
        if (!perf.is_open()) return;
        perf << s << '\n';
        perf.flush();
    }
    /// Called from EndFrame before the per-frame arrays are cleared.
    void PerfFrameEnd(u64 wall_ticks) {
        if (!perf.is_open()) return;
        { // Numpad . (decimal) = scene marker: "M n=<k>" line; perf-summary.ps1 maps k to the protocol's scene list
            const bool down = Common::HostKeyDown(VK_DECIMAL);
            if (down && !perf_key_down) {
                ++perf_marks;
                PerfLine(fmt::format("M n={} frame={}", perf_marks, frame));
            }
            perf_key_down = down;
        }
        const auto ms = [&](ProfBucket b) { return static_cast<float>(f_tick[b] * ms_per_tick); };
        PerfFrame pf{};
        pf.wall = static_cast<float>(wall_ticks * ms_per_tick);
        pf.gap = ms(PB_GapPre) + ms(PB_GapPost);
        pf.wait = ms(PB_WaitFrame) + ms(PB_WaitCapture) + ms(PB_WaitPresent) + ms(PB_WaitUtil) + ms(PB_WaitDump) + ms(PB_Lock);
        pf.rcpu = std::max(0.0f, ms(PB_Replay) + ms(PB_Present) - (pf.wait - ms(PB_Lock)));
        pf.stall = ms(PB_Translate) + ms(PB_Pipeline) + ms(PB_Texture) + ms(PB_Prewarm) + ms(PB_CacheSave);
        pf.submits = static_cast<u32>(f_cnt[PC_Submits]);
        pf.draws = static_cast<u32>(f_cnt[PC_Draws]);
        perf_frames.push_back(pf);
        if (perf_prev_p50 > 0.0f && perf_spikes < 400 && pf.wall > std::max(2.0f * perf_prev_p50, perf_prev_p50 + 6.0f)) {
            ++perf_spikes;
            std::string s = fmt::format("X frame={} wall={:.2f} submits={} draws={} programs={} pipelines={} textures={}", frame, pf.wall, pf.submits,
                                        pf.draws, f_cnt[PC_Programs], f_cnt[PC_Pipelines], f_cnt[PC_Textures]);
            for (u32 i = 0; i < PB_Count; ++i) {
                if (f_tick[i] * ms_per_tick >= 0.05) s += fmt::format(" {}={:.2f}", kProfNames[i], f_tick[i] * ms_per_tick);
            }
            PerfLine(s);
        }
    }
    /// Called at the one-second boundary.
    void PerfSecond(u64 frame_no, double span_ms) {
        if (!perf.is_open() || perf_frames.empty()) return;
        std::vector<float> w;
        double gap = 0, rc = 0, wt = 0, st = 0, sb = 0, dr = 0;
        for (const auto& f : perf_frames) {
            w.push_back(f.wall);
            gap += f.gap;
            rc += f.rcpu;
            wt += f.wait;
            st += f.stall;
            sb += f.submits;
            dr += f.draws;
        }
        std::sort(w.begin(), w.end());
        const double n = static_cast<double>(perf_frames.size());
        perf_prev_p50 = Pct(w, 0.5);
        std::string s = fmt::format("S t={:.1f} frame={} n={} fps={:.1f} wall_p50={:.2f} wall_p95={:.2f} wall_p99={:.2f} wall_max={:.2f} gap={:.2f} rcpu={:.2f} "
                                    "wait={:.2f} stall={:.2f} submits={:.1f} draws={:.0f}",
                                    (ProfNow() - perf_start) / 1e9, frame_no, perf_frames.size(), n * 1000.0 / span_ms, Pct(w, 0.5), Pct(w, 0.95),
                                    Pct(w, 0.99), w.back(), gap / n, rc / n, wt / n, st / n, sb / n, dr / n);
        if (perf_gpu_summary) s += perf_gpu_summary(perf_frames.size());
        PerfLine(s);
        perf_frames.clear();
    }

    /// Returns an empty string on success, else what failed (the caller stops loudly).
    std::string Open(const std::filesystem::path& path) {
        constexpr u64 kTicksPerSecond = 1'000'000'000; // ProfNow() counts nanoseconds
        ms_per_tick = 1000.0 / static_cast<double>(kTicksPerSecond);
        slow_frame_ticks = kTicksPerSecond * 25 / 1000;
        slow_event_ticks = kTicksPerSecond * 10 / 1000;
        second_ticks = kTicksPerSecond;
        out.open(path, std::ios::trunc);
        if (!out) return "cannot open " + path.string();
        out << "# nvn_prof: F = slow frame (wall > 25 ms or one event > 10 ms), S = one-second summary. Bucket = total ms/max single ms.\n";
        out << "# present includes wait_present and wait_util; replay includes prescan, translate, pipeline, texture, buffer, submit, wait_frame.\n";
        out << "# translate / pipeline = wall time the guest thread waited for the worker pool, plus inline (late) builds.\n";
        out.flush();
        last_present = sec_start = ProfNow();
        on = Core::GameSettings::GpuDiagnosticsEnabled();
        return {};
    }
    void Add(ProfBucket b, u64 ticks) {
        if (!on) return;
        f_tick[b] += ticks;
        if (ticks > f_max[b]) f_max[b] = ticks;
        if (ticks > slow_event_ticks) slow_event = true;
    }
    void AddQuiet(ProfBucket b, u64 ticks) { // no slow-event trigger: these are frame-sized every frame
        if (!on) return;
        f_tick[b] += ticks;
        if (ticks > f_max[b]) f_max[b] = ticks;
    }
    void Count(ProfCount c, u64 n = 1) {
        if (!on) return;
        f_cnt[c] += n;
    }
    void Line(const std::string& s) {
        if (!on) return;
        out << s << '\n';
        out.flush();
    }
    std::string Buckets(const u64 (&tick)[PB_Count], const u64 (&mx)[PB_Count], const u64 (&cnt)[PC_Count]) const {
        std::string s;
        for (u32 i = 0; i < PB_Count; ++i) s += fmt::format(" {} {:.2f}/{:.2f}", kProfNames[i], tick[i] * ms_per_tick, mx[i] * ms_per_tick);
        s += " |";
        for (u32 i = 0; i < PC_Count; ++i) s += fmt::format(" {} {}", kProfCountNames[i], cnt[i]);
        return s;
    }
    /// Wiggle-hitch probe: dump every guest thread event of the last present-to-present gap.
    struct HookSnap {
        u64 key, calls, ns;
    };
    std::vector<HookSnap> prev_hooks;
    u64 prev_fb[4]{};
    u64 prev_lock_calls[Kernel::StallRing::kLockCount]{}, prev_lock_wait[Kernel::StallRing::kLockCount]{},
        prev_lock_hold[Kernel::StallRing::kLockCount]{};

    void TakeHookSnapshot() {
        prev_hooks.clear();
        for (const auto& h : Kernel::StallRing::g_hooks) {
            const u64 key = h.key.load(std::memory_order_acquire);
            if (key != 0) {
                prev_hooks.push_back({key, h.calls.load(), h.ns.load()});
            }
        }
        if (Kernel::StallRing::g_fallback_counters) {
            Kernel::StallRing::g_fallback_counters(prev_fb);
        }
        for (u32 i = 0; i < Kernel::StallRing::kLockCount; ++i) {
            prev_lock_calls[i] = Kernel::StallRing::g_locks[i].calls.load();
            prev_lock_wait[i] = Kernel::StallRing::g_locks[i].wait_ns.load();
            prev_lock_hold[i] = Kernel::StallRing::g_locks[i].hold_ns.load();
        }
    }

    /// Hook time and counter deltas over the last frame (the gap), plus the guest profile.
    std::string GapSummary(double gap_ms, double to_ms) {
        namespace SR = Kernel::StallRing;
        std::unordered_map<u64, HookSnap> before;
        for (const auto& h : prev_hooks) {
            before[h.key] = h;
        }
        struct Row {
            u32 gtid;
            uintptr_t fn;
            u64 calls;
            double ms;
        };
        std::vector<Row> rows;
        for (const auto& h : SR::g_hooks) {
            const u64 key = h.key.load(std::memory_order_acquire);
            if (key == 0) {
                continue;
            }
            const auto it = before.find(key);
            const u64 c = h.calls.load() - (it == before.end() ? 0 : it->second.calls);
            const u64 n = h.ns.load() - (it == before.end() ? 0 : it->second.ns);
            if (c != 0) {
                rows.push_back({static_cast<u32>(key >> 48), static_cast<uintptr_t>(key & 0xffffffffffffull), c, n / 1e6});
            }
        }
        const auto name = [](uintptr_t fn) { return SR::g_name_of ? SR::g_name_of(fn) : std::string("?"); };
        std::string sum = "-- hook time per guest thread over the gap (native NVN hooks, includes our renderer work)\n";
        std::map<u32, std::pair<double, u64>> per_thread;
        for (const auto& r : rows) {
            per_thread[r.gtid].first += r.ms;
            per_thread[r.gtid].second += r.calls;
        }
        for (const auto& [g, v] : per_thread) {
            if (v.first >= 0.5 || g == 77) {
                sum += fmt::format("  gtid {}: hooks {:.2f} ms in {} calls, outside hooks {:.2f} ms of {:.1f} ms gap\n", g,
                                   v.first, v.second, gap_ms - v.first, gap_ms);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
        sum += "  top hooks:";
        for (size_t i = 0; i < std::min<size_t>(rows.size(), 8); ++i) {
            sum += fmt::format(" [gtid {} {} x{} {:.2f} ms]", rows[i].gtid, name(rows[i].fn), rows[i].calls, rows[i].ms);
        }
        sum += "\n  pool/texture/program calls:";
        std::map<std::string, u64> by_name;
        for (const auto& r : rows) {
            const std::string n = name(r.fn);
            if (n.find("MemoryPool") != std::string::npos || n.find("Texture") != std::string::npos ||
                n.find("Program") != std::string::npos || n.find("PoolInit") != std::string::npos) {
                by_name[n] += r.calls;
            }
        }
        for (const auto& [n, c] : by_name) {
            sum += fmt::format(" {}={}", n, c);
        }
        sum += "\n-- mutex waits over the gap (all threads, cumulative delta):";
        for (u32 i = 0; i < SR::kLockCount; ++i) {
            sum += fmt::format(" [{} calls {} wait {:.2f} ms hold {:.2f} ms]", SR::kLockNames[i],
                               SR::g_locks[i].calls.load() - prev_lock_calls[i],
                               (SR::g_locks[i].wait_ns.load() - prev_lock_wait[i]) / 1e6,
                               (SR::g_locks[i].hold_ns.load() - prev_lock_hold[i]) / 1e6);
        }
        sum += "\n-- profiler buckets this frame (ms, largest first):";
        {
            std::vector<std::pair<double, u32>> bk;
            for (u32 i = 0; i < PB_Count; ++i) {
                bk.emplace_back(f_tick[i] * ms_per_tick, i);
            }
            std::sort(bk.rbegin(), bk.rend());
            for (size_t i = 0; i < 5; ++i) {
                sum += fmt::format(" {} {:.2f}", kProfNames[bk[i].second], bk[i].first);
            }
            sum += fmt::format("\n-- GAP FELL IN BUCKET: {}\n", kProfNames[bk[0].second]);
        }
        sum += "-- renderer counters this frame:";
        for (u32 i = 0; i < PC_Count; ++i) {
            sum += fmt::format(" {} {}", kProfCountNames[i], f_cnt[i]);
        }
        u64 fb[4]{};
        if (SR::g_fallback_counters) {
            SR::g_fallback_counters(fb);
        }
        sum += fmt::format("\n-- JIT fallback deltas: missing-block entries {} steps {} returns {} full runs {}\n",
                           fb[0] - prev_fb[0], fb[1] - prev_fb[1], fb[2] - prev_fb[2], fb[3] - prev_fb[3]);
        if (SR::g_guest_probe) {
            sum += SR::g_guest_probe(to_ms - gap_ms, to_ms);
        }
        return sum;
    }

    void DumpStall(u64 frame_no, double gap_ms) {
        static u32 dumps = 0;
        if (++dumps > 60) {
            return;
        }
        const double to = Kernel::StallRing::NowMs();
        std::string text = StallEventText(frame_no, gap_ms, to);
        text += GapSummary(gap_ms, to);
        NvnVkDumpLine("nvn_stall.txt", text);
    }

    /// The guest thread event list of [to - gap - 10 ms, to] (no renderer state needed).
    static std::string StallEventText(u64 frame_no, double gap_ms, double to) {
        const double from = to - gap_ms - 10.0;
        const u64 end = Kernel::StallRing::g_index.load();
        const u64 begin = end > Kernel::StallRing::kSize ? end - Kernel::StallRing::kSize : 0;
        std::vector<Kernel::StallRing::Event> ev;
        for (u64 i = begin; i < end; ++i) {
            const auto& e = Kernel::StallRing::g_events[i & (Kernel::StallRing::kSize - 1)];
            if (e.t1 >= from && e.t0 <= to) {
                ev.push_back(e);
            }
        }
        std::sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.t0 < b.t0; });
        std::string text = fmt::format("=== GAP frame {} {:.1f} ms window {:.1f}..{:.1f} events {} (oldest ring entry {:.1f})\n",
                                       frame_no, gap_ms, from, to, ev.size(),
                                       begin == 0 ? 0.0 : Kernel::StallRing::g_events[begin & (Kernel::StallRing::kSize - 1)].t0);
        const char* names[] = {"?", "RUN", "SVC", "SWITCH", "HEARTBEAT", "HOOK", "LOCKWAIT", "SYNC", "CTRLWAIT", "REPORT", "PRESENT"};
        u32 lines = 0;
        for (const auto& e : ev) {
            const double dur = e.t1 - e.t0;
            if (e.kind != Kernel::StallRing::Switch && dur < 0.3 && lines > 4000) {
                continue;
            }
            text += fmt::format("{:.2f} +{:.2f} {} core {} gtid {} prio {} a {:#x} b {:#x}\n", e.t0, dur,
                                names[e.kind <= 10 ? e.kind : 0], e.core, e.gtid, e.prio, e.a, e.b);
            if (e.kind == Kernel::StallRing::Run && dur > 1.0) {
                text.pop_back();
                text += fmt::format(" cpu {:.2f} cycles/ns{}\n", e.b * 1000.0 / (dur * 1e6),
                                    e.b * 1000.0 / (dur * 1e6) < 1.0 ? "  <<< LOW CPU: descheduled or faulting" : "");
            }
            if (e.kind == Kernel::StallRing::Hook && Kernel::StallRing::g_name_of) {
                text.pop_back();
                text += " " + Kernel::StallRing::g_name_of(static_cast<uintptr_t>(e.a) | (static_cast<uintptr_t>(e.b) << 32)) +
                        (dur > 5.0 ? "  <<< HOOK > 5 ms\n" : "\n");
            } else if (e.kind == Kernel::StallRing::LockWait && e.a < Kernel::StallRing::kLockCount) {
                text.pop_back();
                text += std::string(" ") + Kernel::StallRing::kLockNames[e.a] + " waited\n";
            }
            if (++lines > 8000) {
                text += "... truncated\n";
                break;
            }
        }
        return text;
    }
    /// Called at the end of Present.
    void EndFrame(u64 now) {
        ++frame;
        if (Kernel::StallRing::On()) NvnVkDumpLine("nvn_bq.txt", fmt::format("PRESENT frame {} t {:.3f}", frame, NvnVkNowMs()));
        const u64 wall = now - last_present;
        last_present = now;
        if (Kernel::StallRing::On() && wall * ms_per_tick > 60.0) {
            DumpStall(frame, wall * ms_per_tick);
        }
        PerfFrameEnd(wall);
        const bool slow = wall > slow_frame_ticks || slow_event;
        if (slow) {
            out << fmt::format("F {} wall {:.2f} |{}\n", frame, wall * ms_per_tick, Buckets(f_tick, f_max, f_cnt));
            out.flush();
            ++s_slow;
        }
        for (u32 i = 0; i < PB_Count; ++i) {
            s_tick[i] += f_tick[i];
            if (f_max[i] > s_max[i]) s_max[i] = f_max[i];
            f_tick[i] = f_max[i] = 0;
        }
        for (u32 i = 0; i < PC_Count; ++i) {
            s_cnt[i] += f_cnt[i];
            f_cnt[i] = 0;
        }
        slow_event = false;
        if (Kernel::StallRing::On()) TakeHookSnapshot();
        ++s_frames;
        s_wall_sum += wall;
        if (wall > s_wall_max) s_wall_max = wall;
        if (now - sec_start >= second_ticks) {
            PerfSecond(frame, (now - sec_start) * ms_per_tick);
            out << fmt::format("S frame {} span {:.0f} frames {} slow {} avg {:.2f} max {:.2f} |{}\n", frame, (now - sec_start) * ms_per_tick, s_frames,
                               s_slow, s_wall_sum * ms_per_tick / s_frames, s_wall_max * ms_per_tick, Buckets(s_tick, s_max, s_cnt));
            out.flush();
            for (u32 i = 0; i < PB_Count; ++i) s_tick[i] = s_max[i] = 0;
            for (u32 i = 0; i < PC_Count; ++i) s_cnt[i] = 0;
            s_frames = s_wall_sum = s_wall_max = s_slow = 0;
            sec_start = now;
        }
    }
};

struct ProfScope {
    Prof& p;
    ProfBucket b;
    u64 t0;
    ProfScope(Prof& p_, ProfBucket b_) : p{p_}, b{b_}, t0{p_.on ? ProfNow() : 0} {}
    ~ProfScope() {
        if (p.on) p.Add(b, ProfNow() - t0);
    }
    ProfScope(const ProfScope&) = delete;
    ProfScope& operator=(const ProfScope&) = delete;
};

// ---------------------------------------------------------------------------------------------
// Worker threads for shader translation and pipeline creation. The guest thread hands over a
// batch and only waits (its fiber stack is small: the work runs on these threads' own stacks).
// Jobs must not throw; they record their own failure.
// ---------------------------------------------------------------------------------------------
/// Crash records: which of our threads this is ("NvnRender", "NvnWorker"); nullptr = any other thread (g_phase is ours).
thread_local const char* t_nvn_role = nullptr;

class WorkerPool {
public:
    explicit WorkerPool(u32 count, bool unsafe_reads = false) {
        for (u32 i = 0; i < count; ++i) {
            threads.emplace_back([this, unsafe_reads] {
                Core::Memory::SetThreadSkipsRasterizerDownload(unsafe_reads); // [Debug] nvn_unsafe_reads
                t_nvn_role = "NvnWorker";
                Loop();
            });
        }
    }
    ~WorkerPool() {
        {
            std::scoped_lock lk{m};
            stop = true;
        }
        cv.notify_all();
        for (auto& t : threads) t.join();
    }
    u32 Size() const {
        return static_cast<u32>(threads.size());
    }
    /// Runs every job on the pool, ahead of background work, and returns once all of them finished.
    void Run(std::vector<std::function<void()>>& jobs) {
        if (jobs.empty()) return;
        size_t left = jobs.size();
        {
            std::scoped_lock lk{m};
            for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
                queue.push_front([this, &left, job = std::move(*it)] {
                    job();
                    std::scoped_lock lk2{m};
                    if (--left == 0) done_cv.notify_all();
                });
            }
        }
        jobs.clear();
        cv.notify_all();
        std::unique_lock lk{m};
        done_cv.wait(lk, [&left] { return left == 0; });
    }
    /// Background work (shader cache prewarm): queued behind everything, nobody waits for the queue.
    void Post(std::function<void()> job) {
        {
            std::scoped_lock lk{m};
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }

private:
    void Loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lk{m};
                cv.wait(lk, [this] { return stop || !queue.empty(); });
                if (stop) return; // shutting down: queued background work is dropped
                job = std::move(queue.front());
                queue.pop_front();
            }
            job();
        }
    }
    std::mutex m;
    std::condition_variable cv, done_cv;
    std::deque<std::function<void()>> queue;
    bool stop{};
    std::vector<std::thread> threads;
};

// ---------------------------------------------------------------------------------------------
// The renderer
// ---------------------------------------------------------------------------------------------

// Present-path timeline (logging only): user\nvn_present.txt, flushed about once a second.
void PpLine(const std::string& text) {
    static std::mutex m;
    static std::string buf;
    static double last_flush = 0;
    static bool truncated = false;
    std::scoped_lock lk{m};
    buf += text;
    buf += '\n';
    const double now = NvnVkNowMs();
    if (now - last_flush < 1000.0 && buf.size() < (1u << 20)) return;
    last_flush = now;
    std::ofstream f(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_present.txt",
                    truncated ? std::ios::app : std::ios::trunc);
    truncated = true;
    f << buf;
    buf.clear();
}

class Renderer {
    friend void ::Vulkan::NvnVkSavePng(const char*, unsigned, unsigned, const unsigned char*);
    friend bool ::Vulkan::NvnVkPresentImage(VkImage&, unsigned&, unsigned&);
    friend void ::Vulkan::NvnVkPresentImageUsed(MasterSemaphore&, u64);
    friend void ::Vulkan::NvnVkCompositeProbe(int);

public:
    Renderer(const Device& device_, MemoryAllocator& allocator_) : device{device_}, allocator{allocator_}, dev{device_.GetLogical()} {
        g_max_image_dim = device_.GetPhysical().GetProperties().limits.maxImageDimension2D;
        const std::string opts = OptionsText();
        flip_front =opts.find("flipfront") != std::string::npos;
        no_flip_y = opts.find("noflipy") != std::string::npos;
        if (const char* e = std::getenv("NVNVK_DUMP")) dump_all = std::atoi(e) != 0;
        // without a second queue (RADV) the NVN renderer shares the graphics queue; Queue::Submit/Present lock for it
        if (!device.IsKhrPushDescriptorSupported()) {
            Note(fmt::format("disabled: push descriptors {} (separate queue {})", device.IsKhrPushDescriptorSupported(),
                             device.HasSeparateNvnQueue()));
            enabled = false;
            return;
        }
        queue = device.GetNvnQueue();
        pool = dev.CreateCommandPool({
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = device.GetGraphicsFamily(),
        });
        cmd_buffers = pool.Allocate(kSlots + 1);
        for (u32 i = 0; i < kSlots; ++i) {
            frames[i].cmd = cmd_buffers[i];
            frames[i].fence = dev.CreateFence({
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            });
            frames[i].arena = allocator.CreateBuffer(
                {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = kArenaBytes,
                    .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                },
                MemoryUsage::Upload);
        }
        util_cmd = cmd_buffers[kSlots];
        util_fence = dev.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
        MakeProfile();
        MakeHostInfo();
        zero_buffer = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = 65536,
                .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Upload);
        std::memset(zero_buffer.Mapped().data(), 0, zero_buffer.Mapped().size());
        zero_buffer.Flush();
        default_sampler = dev.CreateSampler({
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0f,
            .maxLod = 16.0f,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        });
        start_time = std::chrono::steady_clock::now();
        if (const std::string err = prof.Open(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_prof.txt"); !err.empty()) {
            Fatal("profiler: " + err);
        }
        prof.OpenPerf(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_perf.txt");
        LogPresentMode();
        gp_ini = DebugOption("nvn_perf_gpu", 1) != 0;
        if (prof.perf.is_open()) prof.perf_gpu_summary = [this](u64 presents) { return GpSecond(presents); };
        LoadPipelineCache();
        // [Debug] A/B switches (defaults on): nvn_prescan, nvn_prewarm, nvn_workers (count; 0 = build inline in the replay).
        opt_prescan = DebugOption("nvn_prescan", 1) != 0;
        opt_prewarm = DebugOption("nvn_prewarm", 1) != 0;
        // [Debug] nvn_unsafe_reads (default off): the render thread and the workers read guest memory without asking suyu's
        // GPU to flush its cached copies first (ReadBlockUnsafe behaviour). Off = the old path, which on a rasterizer-cached
        // page calls into suyu's GPU thread from our threads.
        opt_unsafe_reads = DebugOption("nvn_unsafe_reads", 0) != 0;
        // [Debug] nvn_spot_every N (1..256, default 1): a cached texture's guest bytes are compared with its build-time sample
        // hash at most every N frames instead of every frame (streamed-in textures are then noticed up to N-1 frames later).
        opt_spot_every = static_cast<u64>(std::max(1, DebugOption("nvn_spot_every", 1)));
        // [Debug] nvn_prescan_lazy (default off): skip the per-submit prescan walk until the replay meets the first draw whose
        // pipeline is missing, then prescan from that draw on. Same pipelines either way; only when they get built differs.
        opt_prescan_lazy = DebugOption("nvn_prescan_lazy", 0) != 0;
        // [Debug] nvn_fsr_texel_stats (absent = on with gpu_diagnostics, off without): the once-a-second full-size readbacks
        // of the camera mv image (16 MB) and the FSR reactive / silhouette / object images (~12 MB) and the CPU loops over
        // them, for the texel percentages in nvn_mv_pass.txt / nvn_fsr.txt. Log lines only; the picture does not depend on it.
        opt_texel_stats = DebugOption("nvn_fsr_texel_stats", Core::GameSettings::GpuDiagnosticsEnabled() ? 1 : 0) != 0;
        {
            const int w = DebugOption("nvn_workers", -1);
            const u32 hc = std::thread::hardware_concurrency();
            const u32 count = w >= 0 ? static_cast<u32>(w) : std::max<u32>(2, hc > 1 ? hc - 1 : 2);
            if (count > 0) workers = std::make_unique<WorkerPool>(count, opt_unsafe_reads);
        }
        cache_check_tick = ProfNow();
        const std::string opts_line = fmt::format("options: prescan {} prewarm {} workers {}{}", opt_prescan, opt_prewarm, workers ? workers->Size() : 0,
                                                  workers ? "" : " (prescan and prewarm need workers: both off, everything builds inline)");
        Note(opts_line);
        prof.Line("P " + opts_line);
        Note(fmt::format("nvn_unsafe_reads {} nvn_spot_every {} nvn_prescan_lazy {}", opt_unsafe_reads, opt_spot_every, opt_prescan_lazy));
        LoadShaderCache();
        if (opt_prewarm && workers) {
            StartPrewarm();
        } else {
            prewarm_logged = true;
            prof.Line("P prewarm off");
        }
        Note(fmt::format("started; arena {} MB x {}, flipfront={} noflipy={}", kArenaBytes >> 20, kSlots, flip_front, no_flip_y));
        mv_on = Core::GameSettings::NvnMvPass();
        mv_keys = !Core::GameSettings::NvnFsr() || Core::GameSettings::NvnFsrDebugHooks();
        // nvn_mv_pass.txt gets its once-a-second lines only for the debug uses (nvn_mv_pass alone, nvn_fsr_debug / nvn_bench, gpu_diagnostics)
        mv_chatty = mv_keys || Core::GameSettings::GpuDiagnosticsEnabled();
        up_probe = Core::GameSettings::NvnUpscalerProbe();
        if (mv_on) MvLog(fmt::format("nvn_mv_pass on: lag {}, F2 dumps two frames to user/upscaler_dump (mv_<frame>_<w>x<h>.*)",
                                     Core::GameSettings::NvnMvLag()));
        // [Upscaler] nvn_fsr_split_scale: decided with the render scale (RenderScaleFactor, once per run)
        split_on = FsrSplitScaleActive();
        if (split_on) Note(fmt::format("split scale on: scene targets x{}, present images and HUD layer 1X", RenderScaleFactor()));
#ifdef SUYU_HAS_FSR_VULKAN
        if (Core::GameSettings::NvnFsr()) FsrInit();
#endif
    }

    ~Renderer() {
        StopRenderThread();
        if (enabled) {
            workers.reset(); // background prewarm jobs use the members below
#ifdef SUYU_HAS_FSR4_BRIDGE
            // a Vulkan submit may wait on a D3D12 signal: let D3D12 finish (or release the waits) before WaitIdle
            if (fsr4 && fsr4->Initialized() && !fsr4->WaitIdle(1000)) fsr4->ReleaseWaiters();
#endif
            device.GetLogical().WaitIdle();
            SavePipelineCache();
#ifdef SUYU_HAS_FSR_VULKAN
            FsrDestroyContext(false);
#endif
#ifdef SUYU_HAS_FSR4_BRIDGE
            if (fsr4) fsr4->Shutdown();
            fsr4.reset();
#endif
        }
    }

    bool enabled{true};

    /// game_settings.ini [Debug] switch: on/true/off/false or a count; absent = def; anything else stops loudly.
    int DebugOption(const char* key, int def) {
        const std::string v = Core::GameSettings::Value("Debug", key);
        if (v.empty()) return def;
        if (v == "on" || v == "true") return 1;
        if (v == "off" || v == "false") return 0;
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != 0 || n < 0 || n > 256) Fatal(fmt::format("game_settings.ini [Debug] {} = '{}' is not on/off or a count", key, v));
        return static_cast<int>(n);
    }

    /// vkQueueSubmit / vkWaitForFences result. These were ignored: after a driver reset (VK_ERROR_DEVICE_LOST) every wait
    /// returned at once, the slot looked free and the renderer kept recording into reused command buffers. Any error now
    /// stops loudly with the reason (device lost gets its own message).
    void CheckVk(VkResult r, const char* what) {
        if (r == VK_SUCCESS) [[likely]] return;
        if (r == VK_ERROR_DEVICE_LOST) {
            Fatal(fmt::format("GPU device lost (driver reset / TDR) in {}, phase '{}'", what, g_phase));
        }
        Fatal(fmt::format("{} failed: VkResult {}, phase '{}'", what, static_cast<int>(r), g_phase));
    }

    /// Missing support is a loud stop on purpose: write the report, tell the player, end the process.
    [[noreturn]] void Fatal(const std::string& what) {
        Note("FATAL: " + what);
        WriteStats();
        const std::string text = "NVN Vulkan renderer: " + Common::Log::RedactText(what) + "\nSee nvn_vk.txt / nvn_vk_crash.txt in the user folder.";
#ifdef _WIN32
        MessageBoxA(nullptr, text.c_str(), "HEY - something is missing", MB_OK | MB_ICONERROR | MB_TOPMOST);
        TerminateProcess(GetCurrentProcess(), 3);
#else
        std::fprintf(stderr, "%s\n", text.c_str());
        std::_Exit(3);
#endif
        for (;;) {
        }
    }

    // ---- render thread ---------------------------------------------------------------------------
    // The game's submits and presents are replayed on our own thread. The game cannot observe the work finishing
    // except through the guest driver's fences, which suyu's GPU thread signals after NvnVkWaitRenderIdle() returned
    // (gpu_thread.cpp), so guest memory the replay reads stays untouched until it has been read.
    struct Job {
        Core::Memory::Memory* mem{};
        std::vector<std::unique_ptr<NR::List>> lists;
        std::vector<std::pair<u32, u32>> dirty;
        u32 texture{};
        bool present{};
    };
    std::mutex job_lock;
    std::condition_variable job_cv;
    std::deque<Job> jobs;
    u64 jobs_enqueued{}, jobs_done{};
    bool job_stop{};
    bool job_busy_flag{};
    std::thread job_thread;
    bool threaded{};
    std::atomic<u64> enq_wait_us{}, gate_wait_us{}, idle_us{};
    double last_t_line{};
    static constexpr size_t kJobDepth = 3;

    void StartRenderThread() {
        std::error_code ec;
        if (std::filesystem::exists(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_thread_off", ec)) return;
        threaded = true;
        job_thread = std::thread([this] {
            Common::SetCurrentThreadName("NvnRender");
            Core::Memory::SetThreadSkipsRasterizerDownload(opt_unsafe_reads); // [Debug] nvn_unsafe_reads
            t_nvn_role = "NvnRender";
            for (;;) {
                Job j;
                {
                    std::unique_lock lk{job_lock};
                    const double i0 = NvnVkNowMs();
                    job_cv.wait(lk, [&] { return job_stop || !jobs.empty(); });
                    idle_us += static_cast<u64>((NvnVkNowMs() - i0) * 1000.0);
                    if (jobs.empty()) return;
                    j = std::move(jobs.front());
                    jobs.pop_front();
                }
                if (j.present) {
                    PresentNow(*j.mem, j.texture);
                } else {
                    job_dirty = std::move(j.dirty);
                    SubmitNow(*j.mem, std::move(j.lists));
                }
                {
                    std::scoped_lock lk{job_lock};
                    ++jobs_done;
                }
                if (const double now = NvnVkNowMs(); now - last_t_line >= 1000.0) {
                    last_t_line = now;
                    prof.Line(fmt::format("T guest_enqueue_wait {:.0f} ms/s gpu_thread_gate_wait {:.0f} ms/s render_thread_idle {:.0f} ms/s",
                                          enq_wait_us.exchange(0) / 1000.0, gate_wait_us.exchange(0) / 1000.0, idle_us.exchange(0) / 1000.0));
                }
                job_cv.notify_all();
            }
        });
    }
    void StopRenderThread() {
        if (!job_thread.joinable()) return;
        {
            std::scoped_lock lk{job_lock};
            job_stop = true;
        }
        job_cv.notify_all();
        job_thread.join();
    }
    void Enqueue(Job&& j) {
        const double w0 = NvnVkNowMs();
        std::unique_lock lk{job_lock};
        job_cv.wait(lk, [&] { return jobs.size() < kJobDepth; });
        enq_wait_us += static_cast<u64>((NvnVkNowMs() - w0) * 1000.0);
        jobs.push_back(std::move(j));
        ++jobs_enqueued;
        lk.unlock();
        job_cv.notify_all();
    }
    /// Blocks until every job enqueued so far has been replayed (called by suyu's GPU thread before the guest
    /// driver's own submit is processed).
    void WaitRenderIdle() {
        if (!threaded) return;
        static const bool nogate = [] { // [Debug] experiment: signal the guest's fences without waiting (may glitch)
            std::error_code ec;
            return std::filesystem::exists(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_nogate", ec);
        }();
        if (nogate) return;
        std::unique_lock lk{job_lock};
        const u64 want = jobs_enqueued;
        const double w0 = NvnVkNowMs();
        job_cv.wait(lk, [&] { return jobs_done >= want || job_stop; });
        gate_wait_us += static_cast<u64>((NvnVkNowMs() - w0) * 1000.0);
    }
    void Submit(Core::Memory::Memory& mem, std::vector<std::unique_ptr<NR::List>>&& lists) {
        if (!enabled) return;
        if (!threaded) {
            NR::TakeDirty(job_dirty);
            SubmitNow(mem, std::move(lists));
            return;
        }
        Job j;
        j.mem = &mem;
        j.lists = std::move(lists);
        NR::TakeDirty(j.dirty);
        Enqueue(std::move(j));
    }
    void Present(Core::Memory::Memory& mem, u32 texture) {
        if (!enabled) return;
        // guest thread: closes the camera interval of this present (same count as frame_no in PresentBody)
        if (mv_on) Core::GameSettings::MvOnGuestPresent();
        if (!threaded) {
            PresentNow(mem, texture);
            return;
        }
        Job j;
        j.mem = &mem;
        j.present = true;
        j.texture = texture;
        Enqueue(std::move(j));
    }
    std::vector<std::pair<u32, u32>> job_dirty;

    void SubmitNow(Core::Memory::Memory& mem, std::vector<std::unique_ptr<NR::List>>&& lists) {
        if (!enabled) return;
        const u64 t_entry = ProfNow();
        const double t_wall_entry = NvnVkNowMs();
        Kernel::StallRing::TimedGuard lk{lock, Kernel::StallRing::LockRenderer};
        const u64 t_locked = ProfNow();
        prof.Add(PB_Lock, t_locked - t_entry);
        if (!gap_submit_seen) {
            gap_submit_seen = true;
            prof.AddQuiet(PB_GapPre, t_entry - prof.last_present);
        }
        prof.Count(PC_Submits);
        ProfScope ps{prof, PB_Replay};
        try {
            struct Args {
                Renderer* self;
                Core::Memory::Memory* mem;
                std::vector<std::unique_ptr<NR::List>>* lists;
            } args{this, &mem, &lists};
            if (!SehRun([](void* a) { auto* x = static_cast<Args*>(a); x->self->SubmitLocked(*x->mem, std::move(*x->lists)); }, &args)) {
                Fatal(fmt::format("access violation in phase '{}' ({:x}) line {}", g_phase, static_cast<u64>(g_detail), static_cast<int>(g_line)));
            }
        } catch (const std::exception& e) {
            ++errors;
            Note(fmt::format("exception in submit: {}", e.what()));
            Fatal(fmt::format("exception in submit: {}", e.what()));
            if (frame_open) AbandonFrame();
        }
        NR::RecycleLists(std::move(lists)); // replay done with the records: their buffers go back to the recorder
        last_submit_end = ProfNow();
        if (Core::GameSettings::NvnVulkanMode() == 3 && Core::GameSettings::GpuDiagnosticsEnabled()) {
            // Run N: the guest driver's own submit (the fence) comes after this hook returns, i.e. after the replay read.
            PpLine(fmt::format("S{} t_entry {:.1f} locked +{:.1f} replay +{:.1f} end t{:.1f} (guest reads done; fence can only follow)",
                               stats.submits, t_wall_entry, (t_locked - t_entry) * prof.ms_per_tick,
                               (last_submit_end - t_locked) * prof.ms_per_tick, NvnVkNowMs()));
        }
    }

    bool gap_submit_seen{};
    u64 last_submit_end{};

    void PresentNow(Core::Memory::Memory& mem, u32 texture) {
        if (!enabled) return;
        PHASE("present", texture); // crash records: "end frame" no longer covers the whole present path
        const u64 t_entry = ProfNow();
        Kernel::StallRing::TimedGuard lk{lock, Kernel::StallRing::LockRenderer};
        const u64 t_locked = ProfNow();
        prof.Add(PB_Lock, t_locked - t_entry);
        if (gap_submit_seen) prof.AddQuiet(PB_GapPost, t_entry - last_submit_end);
        gap_submit_seen = false;
        PresentBody(mem, texture);
        const u64 t_end = ProfNow();
        prof.Add(PB_Present, t_end - t_locked);
        prof.EndFrame(t_end);
        if (!prewarm_logged && prewarm_end.load() != 0) {
            prewarm_logged = true;
            prof.Line(fmt::format("P prewarm done: programs {} pipelines {} in {:.0f} ms", warm_total[0], warm_total[1],
                                  (prewarm_end.load() - prewarm_start) * prof.ms_per_tick));
        }
        if (t_end - cache_check_tick >= prof.second_ticks * 60) {
            cache_check_tick = t_end;
            ProfScope ps{prof, PB_CacheSave}; // lands in the next frame's line (now only the hand-off to a worker)
            SavePipelineCacheAsync();
        }
    }

    void PresentBody(Core::Memory::Memory& mem, u32 texture) {
        ++stats.presents;
        ++frame_no;
        if (up_probe) UpProbeFrame(mem, texture);
#ifdef SUYU_HAS_FSR_VULKAN
        if (fsr_on) FsrPresent();
#endif
        if (mv_on) MvPresent(mem, texture);
        if (split_on && texture && mem.IsValidVirtualAddressRange(texture, 96)) {
            // the image is re-created at 1X on its next use as a target (RenderTargetLookup)
            if (const u32 k = RtKey(mem, texture); k != 0 && split_present_keys.insert(k).second) rt_memo.clear();
        }
        try {
            if (const auto it = rt_images.find(RtKey(mem, texture)); it != rt_images.end()) {
                last_present = it->second.get();
                cur_tex = tex_index.try_emplace(texture, static_cast<u32>(tex_index.size())).first->second;
                if (Core::GameSettings::NvnVulkanMode() == 3) PresentCopy(*last_present);
                // Frame 600 dump (waits for the GPU, reads back, writes a full-size PNG: a 0.2-0.3 s hitch): gpu_diagnostics only.
                const bool want = dump_all ? (frame_no % 600 == 0)
                                           : ((frame_no == 600 && Core::GameSettings::GpuDiagnosticsEnabled()) || NvnVkCaptureActive());
                if (want && dumped < kMaxDumpFrames) {
                    DumpImage(*it->second, fmt::format("nvn_vk_frame_{}.png", NvnVkCaptureActive() ? 1800u : frame_no));
                    ++dumped;
                    for (auto& ct : cap_tex) HashTexture(ct.slot, *ct.img, ct.tic);
                    cap_tex.clear();
                    if (NvnVkCaptureActive()) {
                        u32 n = 0;
                        for (auto& [k, img] : rt_images) {
                            if (img->aspect != VK_IMAGE_ASPECT_COLOR_BIT && n < 6 && img->width >= 256) {
                                DumpDepth(*img, fmt::format("nvn_vk_depth_{:x}.png", k));
                                ++n;
                            }
                        }
                    }
                    if (NvnVkCaptureActive()) {
                        u32 n = 0;
                        for (auto& [k, img] : rt_images) {
                            if (img->width >= 240 && img->aspect == VK_IMAGE_ASPECT_COLOR_BIT && n < 60 &&
                                (img->format == VK_FORMAT_R8G8B8A8_UNORM || img->format == VK_FORMAT_R8G8B8A8_SRGB || img->format == VK_FORMAT_B8G8R8A8_UNORM)) {
                                DumpImage(*img, fmt::format("nvn_vk_rt_{:x}.png", k));
                                ++n;
                            }
                        }
                    }
                }
            } else {
                ++stats.notes["present: window texture has no image"];
            }
        } catch (const std::exception& e) {
            Note(fmt::format("exception in present: {}", e.what()));
        }
        if (frame_no % 300 == 0) WriteStats();
    }

private:
    // ---- setup helpers ------------------------------------------------------------------------
    void Note(const std::string& s) {
        Kernel::StallRing::TimedGuard lk{note_lock, Kernel::StallRing::LockNote};
        if (log_lines.size() < 400) log_lines.push_back(Common::Log::RedactText(s)); // [Debug] log_redact_paths
    }

    // ---- [Upscaler] nvn_fsr_split_scale ------------------------------------------------------------------------------
    // Scene targets at the render scale (baked into the shaders), present images and the HUD layer at 1X (the output;
    // FsrSplitScale only enables the split when the output is exactly the game's render size, so a 1X image is an
    // unscaled image for the shaders: no rescale bit, frag coords unscaled). Frames FSR cannot take show the render-size
    // picture scaled up by the game's own final draw (bilinear), like the old single-factor Quality modes.
    bool SplitNow() const {
        return split_on && !split_failed;
    }
    /// Logged once per tag (suyu log + nvn_vk notes): a frame / pass that falls back.
    void SplitNote(const char* tag, const std::string& text) {
        if (!split_notes.insert(tag).second) return;
        LOG_INFO(Render_Vulkan, "nvn_vk: split scale: {} (logged once)", text);
        Note("split scale: " + text);
    }
    /// The split cannot go on (a pass mixes 1X and render-scale attachments outside the HUD layer): present images go
    /// back to the render scale from the next lookup on (= the old single-factor behaviour for the rest of the run).
    void SplitFail(const std::string& why) {
        if (split_failed) return;
        split_failed = true;
        LOG_WARNING(Render_Vulkan, "nvn_vk: split scale OFF for the rest of the run (old single render scale): {}", why);
        Note("split scale OFF: " + why);
        rt_memo.clear();
        tex_bind_cache.clear();
        pass_dirty = true;
    }

    void MakeProfile() {
        const auto& fc{device.FloatControlProperties()};
        const VkDriverIdKHR driver_id{device.GetDriverID()};
        const VkShaderStageFlags supported{device.GetSubgroupSupportedStages()};
        u32 subgroup_stages = 0;
        if (supported & VK_SHADER_STAGE_VERTEX_BIT) subgroup_stages |= 1u << static_cast<u32>(Shader::Stage::VertexB);
        if (supported & VK_SHADER_STAGE_FRAGMENT_BIT) subgroup_stages |= 1u << static_cast<u32>(Shader::Stage::Fragment);
        profile = Shader::Profile{
            .supported_spirv = device.SupportedSpirvVersion(),
            .unified_descriptor_binding = true,
            .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
            .support_int8 = device.IsInt8Supported(),
            .support_uniform_and_storage_buffer_8bit = device.IsUniformAndStorageBuffer8BitAccessSupported(),
            .support_storage_buffer_8bit = device.IsStorageBuffer8BitAccessSupported(),
            .support_int16 = device.IsShaderInt16Supported(),
            .support_uniform_and_storage_buffer_16bit = device.IsUniformAndStorageBuffer16BitAccessSupported(),
            .support_storage_buffer_16bit = device.IsStorageBuffer16BitAccessSupported(),
            .support_int64 = device.IsShaderInt64Supported(),
            .support_vertex_instance_id = false,
            .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
            .support_separate_denorm_behavior = fc.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
            .support_separate_rounding_mode = fc.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
            .support_fp16_denorm_preserve = fc.shaderDenormPreserveFloat16 != VK_FALSE,
            .support_fp32_denorm_preserve = fc.shaderDenormPreserveFloat32 != VK_FALSE,
            .support_fp16_denorm_flush = fc.shaderDenormFlushToZeroFloat16 != VK_FALSE,
            .support_fp32_denorm_flush = fc.shaderDenormFlushToZeroFloat32 != VK_FALSE,
            .support_fp16_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
            .support_fp32_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
            .support_fp64_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
            .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
            .support_workgroup_layout_8bit_access = device.IsWorkgroupMemoryExplicitLayout8BitAccessSupported(),
            .support_workgroup_layout_16bit_access = device.IsWorkgroupMemoryExplicitLayout16BitAccessSupported(),
            .support_shader_quad_control = device.IsKhrShaderQuadControlSupported(),
            .support_quad_shuffles = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT),
            .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
            .support_shuffle_relative = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT),
            .supported_subgroup_stages = subgroup_stages,
            .support_viewport_index_layer_non_geometry = device.IsExtShaderViewportIndexLayerSupported(),
            .support_viewport_mask = device.IsNvViewportArray2Supported(),
            .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
            .support_demote_to_helper_invocation = device.IsExtShaderDemoteToHelperInvocationSupported(),
            .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
            .support_shared_int64_atomics = device.IsSharedInt64AtomicsSupported(),
            .support_derivative_control = true,
            .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
            .support_native_ndc = device.IsExtDepthClipControlSupported(),
            .support_scaled_attributes = !device.MustEmulateScaledFormats(),
            .support_multi_viewport = device.SupportsMultiViewport(),
            .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
            .support_sampled_image_array_nonuniform_indexing = device.IsSampledImageArrayNonUniformIndexingSupported(),
            .support_storage_image_array_nonuniform_indexing = device.IsStorageImageArrayNonUniformIndexingSupported(),
            .support_uniform_texel_buffer_array_nonuniform_indexing = device.IsUniformTexelBufferArrayNonUniformIndexingSupported(),
            .support_storage_texel_buffer_array_nonuniform_indexing = device.IsStorageTexelBufferArrayNonUniformIndexingSupported(),
            .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),
            .lower_left_origin_mode = false,
            .need_declared_frag_colors = false,
            .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY || driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                           driver_id == VK_DRIVER_ID_MESA_RADV || driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                           driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA,
            .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
            .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
            .has_broken_fp32_denorm_flush = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
            .has_broken_spirv_subgroup_shuffle = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
            .max_subgroup_size = device.GetMaxSubgroupSize(),
            .has_broken_robust = device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
            .min_ssbo_alignment = device.GetStorageBufferAlignment(),
            .max_user_clip_distances = device.GetMaxUserClipDistances(),
        };
    }

    void MakeHostInfo() {
        host_info = Shader::HostTranslateInfo{
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
            .support_viewport_mask = device.IsNvViewportArray2Supported(),
            .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
            .support_conditional_barrier = device.SupportsConditionalBarriers(),
            .single_lane_geometry_subgroups = true,
        };
        host_info.ApplyDescriptorLimitPolicy();
    }

    // ---- guest memory ---------------------------------------------------------------------------
    bool ReadGpu(Core::Memory::Memory& mem, u64 va, void* dst, u32 size) {
        u32 cpu = 0, avail = 0;
        if (!NR::GpuToCpu(mem, va, cpu, avail) || avail < size) return false;
        mem.ReadBlock(cpu, dst, size);
        return true;
    }

    // Copies guest bytes into the frame arena; returns the offset or ~0.
    size_t Upload(Core::Memory::Memory& mem, u64 va, u32 size, size_t align) {
        auto& f = frames[cur];
        const size_t at = (f.used + align - 1) & ~(align - 1);
        if (size == 0) {
            upload_fail = "size 0";
            return ~size_t{0};
        }
        // Per-submit cache: the guest cannot change memory while one submit is replayed, so a range (address, size,
        // alignment) already copied into this frame's arena is reused instead of read again.
        if (const auto hit = up_cache.find({va, size, align}); hit != up_cache.end()) {
            prof.Count(static_cast<ProfCount>(PC_DupVB + up_cat), size);
            return hit->second;
        }
        if (at + size > kArenaBytes) {
            ++stats.arena_full;
            prof.Count(PC_ArenaFull);
            upload_fail = "upload arena full";
            return ~size_t{0};
        }
        ProfScope ps{prof, PB_Buffer};
        if (!ReadGpu(mem, va, f.arena.Mapped().data() + at, size)) {
            upload_fail = "address not in a known pool";
            return ~size_t{0};
        }
        prof.Count(PC_Bytes, size);
        // Run P probe (logging only): bytes per kind, and bytes of a range already uploaded identically in this submit
        // (the guest cannot change memory mid-replay, so such a repeat is provably the same data).
        prof.Count(static_cast<ProfCount>(PC_BytesVB + up_cat), size);
        up_cache.emplace(std::make_tuple(va, size, align), at);
        f.used = at + size;
        return at;
    }
    /// A guest buffer that could not be used: counted per kind and reason, with the first addresses, in nvn_vk.txt.
    NVN_VK_NOINLINE void UploadFailure(const char* what, u64 va, u32 size) {
        const std::string key = fmt::format("{}: {}", what, upload_fail);
        const u64 n = ++stats.notes[key];
        if (n <= 6) {
            ++stats.notes[fmt::format("  {} #{}: va {:x} size {:x} arena used {:x} of {:x} programs {:x} {:x} submit {}", key, n, va, size,
                                      frames[cur].used, kArenaBytes, st.prog[0], st.prog[1], stats.submits)];
        }
    }
    size_t UploadBytes(const void* src, u32 size, size_t align) {
        auto& f = frames[cur];
        const size_t at = (f.used + align - 1) & ~(align - 1);
        if (at + size > kArenaBytes) {
            ++stats.arena_full;
            return ~size_t{0};
        }
        std::memcpy(f.arena.Mapped().data() + at, src, size);
        prof.Count(PC_Bytes, size); // texture staging: its time is in the texture bucket
        f.used = at + size;
        return at;
    }

    // ---- images -----------------------------------------------------------------------------------
    u32 rt_logged{};
    u32 ir_dumped{};
    u32 draw_logged{};
    u32 shadow_logged{};
    Core::Memory::Memory* last_mem{};
    static u64 Read64(Core::Memory::Memory& mem, u32 a) {
        return mem.Read32(a) | (u64{mem.Read32(a + 4)} << 32);
    }
    static u32 RtKey(Core::Memory::Memory& mem, u32 tex) {
        u32 cpu = 0, size = 0;
        if (!NR::PoolMemory(mem.Read32(tex + 12), cpu, size)) return 0;
        return cpu + mem.Read32(tex + 16);
    }

    static constexpr VkFormat kRtColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kRtDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkFormat RtFormat(u32 nvn_format) {
        switch (nvn_format) {
        case 0x25:
            return kRtColorFormat;
        default:
            ++stats.notes[fmt::format("rt format {:x} unknown, using RGBA8", nvn_format)];
            return kRtColorFormat;
        }
    }
    VkFormat DepthFormat(u32 nvn_format) {
        ++stats.notes[fmt::format("depth format {:x} -> D32S8", nvn_format)];
        return kRtDepthFormat;
    }

    /// Prescan: the format RenderTarget() would hand out for this target, without creating anything or
    /// touching counters. VK_FORMAT_UNDEFINED where RenderTarget() would fail (the draw is skipped).
    VkFormat PeekRtFormat(Core::Memory::Memory& mem, u32 tex, bool depth) {
        if (tex == 0 || !mem.IsValidVirtualAddressRange(tex, 96)) return VK_FORMAT_UNDEFINED;
        const u32 key = RtKey(mem, tex);
        if (key == 0) return VK_FORMAT_UNDEFINED;
        const u32 w = mem.Read32(tex + 28), h = mem.Read32(tex + 32);
        if (w == 0 || h == 0 || w > 16384 || h > 16384) return VK_FORMAT_UNDEFINED;
        if (const auto it = rt_images.find(key); it != rt_images.end() && it->second->width == w && it->second->height == h) {
            return it->second->format;
        }
        return depth ? kRtDepthFormat : kRtColorFormat;
    }

    // Per-frame memo of RenderTarget() / RtKey() results (they are called 8-12 times per draw: MakeKey, FSR, MV, objv,
    // draw counters). Valid only while a frame is being replayed (frame_open): the guest does not change the texture
    // objects or pool memory the replay reads until the replay is done (the same premise as up_cache). Cleared in
    // BeginFrame and whenever RenderTarget() creates or replaces an image; only successful lookups are kept.
    std::unordered_map<u64, Image*> rt_memo;
    std::unordered_map<u32, u32> rtkey_memo;

    Image* RenderTarget(Core::Memory::Memory& mem, u32 tex, bool depth) {
        const u64 mk = (u64{tex} << 1) | (depth ? 1u : 0u);
        if (frame_open) {
            if (const auto m = rt_memo.find(mk); m != rt_memo.end()) {
                m->second->last_use = serial;
                return m->second;
            }
        }
        Image* const img = RenderTargetLookup(mem, tex, depth);
        if (img && frame_open) rt_memo.emplace(mk, img);
        return img;
    }
    /// RtKey() for draw-time callers (memoised per frame like RenderTarget()).
    u32 RtKeyFrame(Core::Memory::Memory& mem, u32 tex) {
        if (frame_open) {
            if (const auto m = rtkey_memo.find(tex); m != rtkey_memo.end()) return m->second;
        }
        const u32 key = RtKey(mem, tex);
        if (key != 0 && frame_open) rtkey_memo.emplace(tex, key);
        return key;
    }

    Image* RenderTargetLookup(Core::Memory::Memory& mem, u32 tex, bool depth) {
        if (tex == 0 || !mem.IsValidVirtualAddressRange(tex, 96)) return nullptr;
        const u32 key = RtKey(mem, tex);
        if (key == 0) {
            ++stats.notes["render target: pool unknown"];
            return nullptr;
        }
        const u32 w = mem.Read32(tex + 28), h = mem.Read32(tex + 32), fmt_code = mem.Read32(tex + 44);
        if (w == 0 || h == 0 || w > 16384 || h > 16384) {
            ++stats.notes["render target: bad size"];
            return nullptr;
        }
        const auto it = rt_images.find(key);
        // nvn_fsr_split_scale: images the game presents stay at 1X (the output); one created before its first present
        // (at the render scale) is replaced once here
        const bool want_out = SplitNow() && !depth && split_present_keys.contains(key);
        if (it != rt_images.end() && it->second->width == w && it->second->height == h && it->second->out_scaled == want_out) {
            it->second->last_use = serial;
            return it->second.get();
        }
        ProfScope ps{prof, PB_Texture};
        prof.Count(PC_Textures);
        auto img = std::make_unique<Image>();
        img->format = depth ? DepthFormat(fmt_code) : RtFormat(fmt_code);
        img->aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
        img->width = w;
        img->height = h;
        if (want_out) {
            img->pw = w; // 1X: the output size
            img->ph = h;
            img->out_scaled = true;
            if (it != rt_images.end() && !it->second->out_scaled) {
                SplitNote("present image", fmt::format("present image {:x} ({}x{}) is kept at the output size {}x{} (the scene targets at x{})", key,
                                                       w, h, w, h, RenderScaleFactor()));
            }
        } else {
            ScaledSize(w, h, img->pw, img->ph);
        }
        img->cpu = key;
        img->bytes = mem.Read32(tex + 80);
        img->render_target = true;
        if (rt_logged < 40) {
            ++rt_logged;
            const u32 pool_obj = mem.Read32(tex + 12);
            ++stats.notes[fmt::format("  rt image {}x{} depth {} cpu {:x} pool obj {:x} offset {:x} | pool words +40 {:x} +56 {:x}", w, h, depth, key,
                                      pool_obj, mem.Read32(tex + 16), mem.IsValidVirtualAddressRange(pool_obj, 80) ? Read64(mem, pool_obj + 40) : 0,
                                      mem.IsValidVirtualAddressRange(pool_obj, 80) ? Read64(mem, pool_obj + 56) : 0)];
        }
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT |
                                        (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        img->image = allocator.CreateImage({
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = img->format,
            .extent = {img->pw, img->ph, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        img->view = dev.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *img->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = img->format,
            .components = {},
            .subresourceRange = {img->aspect, 0, 1, 0, 1},
        });
        if (depth) {
            img->sampled_view = dev.CreateImageView({
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = *img->image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = img->format,
                .components = {},
                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
            });
        }
        pending_transitions.push_back({*img->image, img->aspect});
        ++stats.images;
        auto* raw = img.get();
        raw->last_use = serial;
        if (it != rt_images.end()) {
            graveyard.push_back({serial, std::move(it->second)});
            fb_graveyard.emplace_back(serial, std::make_shared<std::map<std::string, std::unique_ptr<vk::Framebuffer>>>(std::move(framebuffers)));
            framebuffers.clear();
        }
        tex_bind_cache.clear();
        rt_memo.clear(); // an image was created or replaced: memoised pointers may be stale
        rt_images[key] = std::move(img);
        return raw;
    }

    // Texture sampled through a descriptor the guest driver wrote (TIC), decoded with suyu's code.
    Image* SampledImage(Core::Memory::Memory& mem, const Tegra::Texture::TICEntry& tic) {
        const u64 va = tic.Address();
        u32 cpu = 0, avail = 0;
        if (!NR::GpuToCpu(mem, va, cpu, avail)) {
            ++stats.notes["texture: address not in a known pool"];
            if (stats.notes.size() + vp_probe.size() < 60) { // vp_probe: the probe's notes not yet folded in (WriteStats)
                ++stats.notes[fmt::format("  unresolved texture va {:x} {}x{} fmt {}", va, tic.Width(), tic.Height(),
                                          static_cast<int>(tic.format.Value()))];
                static bool pools_logged = false;
                if (!pools_logged) {
                    pools_logged = true;
                    std::vector<std::string> lines;
                    NR::LogPools(mem, lines);
                    for (const auto& l : lines) ++stats.notes[l];
                }
            }
            return nullptr;
        }
        if (const auto rt = rt_images.find(cpu); rt != rt_images.end() && rt->second->width == tic.Width() &&
                                                  rt->second->height == tic.Height()) {
            rt->second->last_use = serial;
            if (stats.submits > 1000) {
                ++stats.notes[fmt::format("  samples render target {:x} {}x{} {} draws {} clears {} tic fmt {}{}", cpu, rt->second->width,
                                          rt->second->height, rt->second->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? "color" : "depth",
                                          rt->second->draws > 0 ? 1 : 0, rt->second->clears > 0 ? 1 : 0, static_cast<int>(tic.format.Value()),
                                          (rt->second->draws + rt->second->clears) == 0 ? " NEVER RENDERED" : "")];
            }
            return rt->second.get();
        }
        std::array<u32, 8> words;
        std::memcpy(words.data(), &tic, 32);
        u64 h = 1469598103934665603ull;
        for (const u32 w : words) h = (h ^ w) * 1099511628211ull;
        h = (h ^ cpu) * 1099511628211ull;
        if (tic.IsPitchLinear()) {
            // The game rewrites these (bone matrices and the like) from the CPU every frame: keep the
            // content in the key, checked once per submit.
            auto& seen = linear_seen[cpu];
            if (seen.serial != serial) {
                u64 c = 1469598103934665603ull;
                const u32 n = std::min<u32>(avail, std::min<u32>(tic.Pitch() * tic.Height(), 4u << 20)) & ~7u;
                // Same 8-byte FNV over the same n bytes, without a zero-filled copy: runs inside one plain-memory page are
                // hashed through the page's host pointer, everything else is read with ReadBlock into a small buffer.
                alignas(8) u8 chunk[512]; // small: without the render thread this runs on the guest fiber stack
                for (u32 i = 0; i < n;) {
                    const u32 a = cpu + i;
                    u32 len = std::min<u32>(n - i, 4096 - (a & 0xfff)) & ~7u;
                    const u8* src = len ? mem.GetPlainPointer(a) : nullptr;
                    if (!src) {
                        len = std::min<u32>(n - i, sizeof(chunk)); // n - i is a multiple of 8
                        mem.ReadBlock(a, chunk, len);
                        src = chunk;
                    }
                    for (u32 k = 0; k < len; k += 8) {
                        u64 v;
                        std::memcpy(&v, src + k, 8);
                        c = (c ^ v) * 1099511628211ull;
                    }
                    i += len;
                }
                if (seen.hash != c && seen.key != 0) {
                    if (const auto old = textures.find(seen.key); old != textures.end()) {
                        graveyard.push_back({serial, std::move(old->second)});
                        textures.erase(old);
                        tex_bind_cache.clear();
                    }
                }
                seen.serial = serial;
                seen.hash = c;
            }
            h = (h ^ seen.hash) * 1099511628211ull;
            seen.key = h;
        }
        if (const auto it = textures.find(h); it != textures.end()) {
            // [Debug] nvn_spot_every N (default 1 = every frame, the old behaviour): compare at most every N frames.
            if (it->second && !tic.IsPitchLinear() && serial - it->second->spot_serial >= opt_spot_every) {
                // The game streams new textures into the same memory without telling us (area reloads):
                // the cache key (descriptor + address) is then unchanged but the content is not.
                it->second->spot_serial = serial;
                if (SpotHash(mem, cpu, it->second->bytes) != it->second->spot) {
                    ++stats.notes["texture: guest memory changed under a cached image, rebuilt"];
                    prof.Count(PC_TexRestream);
                    graveyard.push_back({serial, std::move(it->second)});
                    textures.erase(it);
                    tex_bind_cache.clear();
                    goto build;
                }
            }
            if (it->second) it->second->last_use = serial;
            return it->second.get();
        }
    build:
        PHASE("build texture", cpu);
        const auto t0 = std::chrono::steady_clock::now();
        prof.Count(PC_Textures);
        const u64 pt0 = ProfNow();
        auto img = BuildTexture(mem, tic, cpu, avail);
        prof.Add(PB_Texture, ProfNow() - pt0);
        stats.texture_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!img) {
            ++stats.image_fail;
            textures[h] = nullptr;
            return nullptr;
        }
        img->last_use = serial;
        auto* raw = img.get();
        tex_bind_cache.clear();
        textures[h] = std::move(img);
        ++stats.images;
        return raw;
    }

    /// 256 evenly spread 8-byte samples of a guest range: enough to notice that a texture was replaced.
    static u64 SpotHash(Core::Memory::Memory& mem, u32 cpu, u32 bytes) {
        u64 h = 1469598103934665603ull;
        if (bytes < 8) return h;
        const u32 n = std::min<u32>(256, bytes / 8);
        // Samples inside one plain-memory page are read through the page's host pointer (the bytes ReadBlock would copy);
        // anything else (page-crossing sample, unmapped / rasterizer-cached page) still goes through ReadBlock.
        u32 page = ~0u;
        const u8* page_ptr = nullptr;
        for (u32 i = 0; i < n; ++i) {
            const u32 off = n > 1 ? static_cast<u32>(u64{bytes - 8} * i / (n - 1)) & ~7u : 0;
            const u32 a = cpu + off;
            u64 v;
            if ((a & 0xfff) <= 0xff8) {
                if ((a >> 12) != page) {
                    page = a >> 12;
                    page_ptr = mem.GetPlainPointer(a & ~0xfffu);
                }
                if (page_ptr) {
                    std::memcpy(&v, page_ptr + (a & 0xfff), 8);
                    h = (h ^ v) * 1099511628211ull;
                    continue;
                }
            }
            mem.ReadBlock(a, &v, 8);
            h = (h ^ v) * 1099511628211ull;
        }
        return h;
    }

    std::unique_ptr<Image> BuildTexture(Core::Memory::Memory& mem, const Tegra::Texture::TICEntry& tic, u32 cpu, u32 avail) {
        using namespace VideoCommon;
        const ImageInfo info(tic);
        if ((info.type != ImageType::e2D && info.type != ImageType::Linear) || info.num_samples != 1 || info.format == VideoCore::Surface::PixelFormat::Invalid) {
            ++stats.notes[fmt::format("texture: unsupported type {} samples {} format {}", static_cast<int>(info.type),
                                      info.num_samples, static_cast<int>(info.format))];
            return nullptr;
        }
        const auto fi = MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, false, info.format);
        const bool bcn = VideoCore::Surface::IsPixelFormatBCn(info.format);
        const bool astc = VideoCore::Surface::IsPixelFormatASTC(info.format);
        const bool converted = (bcn && !device.IsOptimalBcnSupported()) || (astc && !device.IsOptimalAstcSupported());
        const u32 guest_size = CalculateGuestSizeInBytes(info);
        if (guest_size == 0 || guest_size > avail || (info.layer_stride == 0 && info.resources.layers > 1)) {
            ++stats.notes["texture: size not in pool"];
            return nullptr;
        }
        std::vector<u8> guest(guest_size);
        mem.ReadBlock(cpu, guest.data(), guest_size);
        {
            static u32 tex_logged = 0;
            if (tex_logged < 40) {
                ++tex_logged;
                u32 nz = 0, step = std::max<u32>(1, guest_size / 4096);
                for (u32 i = 0; i < guest_size; i += step) nz += guest[i] != 0;
                ++stats.notes[fmt::format("  tex {}x{} fmt {} type {} cpu {:x} bytes {:x} nonzero {}/{}", info.size.width, info.size.height,
                                          static_cast<int>(info.format), static_cast<int>(info.type), cpu, guest_size, nz,
                                          (guest_size + step - 1) / step)];
            }
        }
        const u32 unswizzled_size = CalculateUnswizzledSizeBytes(info);
        std::vector<u8> unswizzled(unswizzled_size);
        // gpu_memory is only used by the pitch-linear path.
        const bool linear = info.type == ImageType::Linear;
        auto copies = linear ? FullDownloadCopies(info)
                             : UnswizzleImage(*static_cast<Tegra::MemoryManager*>(nullptr), 0, info, guest, unswizzled);
        if (linear) {
            ++stats.notes["texture: pitch linear uploaded as is"];
            unswizzled = guest;
        }
        std::vector<u8> converted_data;
        std::span<const u8> upload = unswizzled;
        if (converted) {
            converted_data.resize(CalculateConvertedSizeBytes(info));
            ConvertImage(unswizzled, info, converted_data, copies);
            upload = converted_data;
        }
        auto img = std::make_unique<Image>();
        img->format = fi.format;
        img->width = info.size.width;
        img->height = info.size.height;
        img->cpu = cpu;
        img->bytes = guest_size;
        img->spot = SpotHash(mem, cpu, guest_size);
        img->spot_serial = serial;
        img->levels = static_cast<u32>(info.resources.levels);
        img->layers = static_cast<u32>(info.resources.layers);
        img->aspect = VideoCore::Surface::GetFormatType(info.format) == VideoCore::Surface::SurfaceType::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const bool cube = tic.texture_type == Tegra::Texture::TextureType::TextureCubemap ||
                          tic.texture_type == Tegra::Texture::TextureType::TextureCubeArray;
        img->image = allocator.CreateImage({
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = cube ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) : VkImageCreateFlags{0},
            .imageType = VK_IMAGE_TYPE_2D,
            .format = img->format,
            .extent = {img->width, img->height, 1},
            .mipLevels = img->levels,
            .arrayLayers = img->layers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        const auto swz = [](Tegra::Texture::SwizzleSource s) {
            switch (s) {
            case Tegra::Texture::SwizzleSource::Zero:
                return VK_COMPONENT_SWIZZLE_ZERO;
            case Tegra::Texture::SwizzleSource::R:
                return VK_COMPONENT_SWIZZLE_R;
            case Tegra::Texture::SwizzleSource::G:
                return VK_COMPONENT_SWIZZLE_G;
            case Tegra::Texture::SwizzleSource::B:
                return VK_COMPONENT_SWIZZLE_B;
            case Tegra::Texture::SwizzleSource::A:
                return VK_COMPONENT_SWIZZLE_A;
            default:
                return VK_COMPONENT_SWIZZLE_ONE;
            }
        };
        img->view = dev.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *img->image,
            .viewType = cube ? (img->layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
                            : (img->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D),
            .format = img->format,
            .components = {swz(tic.x_source), swz(tic.y_source), swz(tic.z_source), swz(tic.w_source)},
            .subresourceRange = {img->aspect, 0, img->levels, 0, img->layers},
        });
        // Stage the pixels in the util buffer, copy, and leave the image readable.
        const size_t at = UploadBytes(upload.data(), static_cast<u32>(upload.size()), 16);
        if (at == ~size_t{0}) return nullptr;
        texture_uploads.push_back({*img->image, img->aspect, img->levels, img->layers, at, std::move(copies)});
        return img;
    }

    // ---- sampler ----------------------------------------------------------------------------------
    VkSampler Sampler(Core::Memory::Memory& mem, u32 sampler_pool, u32 sid) {
        u32 cpu = 0;
        if (!NR::SamplerDescriptor(sampler_pool, sid, cpu)) return *default_sampler;
        Tegra::Texture::TSCEntry tsc{};
        mem.ReadBlock(cpu, &tsc, sizeof(tsc));
        u64 h = 1469598103934665603ull;
        std::array<u32, 8> w;
        std::memcpy(w.data(), &tsc, 32);
        for (const u32 x : w) h = (h ^ x) * 1099511628211ull;
        if (const auto it = samplers.find(h); it != samplers.end()) return **it->second;
        const auto mag = MaxwellToVK::Sampler::Filter(tsc.mag_filter);
        const auto min = MaxwellToVK::Sampler::Filter(tsc.min_filter);
        const auto mip = MaxwellToVK::Sampler::MipmapMode(tsc.mipmap_filter);
        const f32 aniso = std::clamp(tsc.MaxAnisotropy(), 1.0f, 16.0f);
        // The guest's border colour decides what lies outside a texture (shadow maps rely on white).
        const auto& bc = tsc.border_color;
        VkBorderColor border = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (bc[0] > 0.5f && bc[1] > 0.5f && bc[2] > 0.5f) border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (bc[3] < 0.5f) border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        ++stats.notes[fmt::format("  sampler wrap {} {} {} compare {} func {} border {:.2f} {:.2f} {:.2f} {:.2f}",
                                  static_cast<int>(tsc.wrap_u.Value()), static_cast<int>(tsc.wrap_v.Value()), static_cast<int>(tsc.wrap_p.Value()),
                                  static_cast<int>(tsc.depth_compare_enabled.Value()), static_cast<int>(tsc.depth_compare_func.Value()),
                                  bc[0], bc[1], bc[2], bc[3])];
        auto s = dev.CreateSampler({
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = mag,
            .minFilter = min,
            .mipmapMode = mip,
            .addressModeU = MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_u, tsc.mag_filter),
            .addressModeV = MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_v, tsc.mag_filter),
            .addressModeW = MaxwellToVK::Sampler::WrapMode(device, tsc.wrap_p, tsc.mag_filter),
            .mipLodBias = tsc.LodBias(),
            .anisotropyEnable = aniso > 1.0f ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = aniso,
            .compareEnable = static_cast<VkBool32>(tsc.depth_compare_enabled),
            .compareOp = (Core::GameSettings::NvnVkDebug() & 128) ? VK_COMPARE_OP_ALWAYS : (Core::GameSettings::NvnVkDebug() & 256) ? VK_COMPARE_OP_NEVER : MaxwellToVK::Sampler::DepthCompareFunction(tsc.depth_compare_func),
            .minLod = tsc.mipmap_filter == Tegra::Texture::TextureMipmapFilter::None ? 0.0f : tsc.MinLod(),
            .maxLod = tsc.mipmap_filter == Tegra::Texture::TextureMipmapFilter::None ? 0.25f : tsc.MaxLod(),
            .borderColor = border,
            .unnormalizedCoordinates = VK_FALSE,
        });
        ++stats.samplers;
        const VkSampler raw = *s;
        samplers[h] = std::make_unique<vk::Sampler>(std::move(s));
        return raw;
    }

    // ---- shaders and pipelines --------------------------------------------------------------------
    /// From guest memory (replay == nullptr; the answers are recorded into `capture` when given) or from a
    /// recorded transcript (prewarm).
    std::unique_ptr<Translated> Translate(Core::Memory::Memory* mem, u64 nvn_addr, Shader::Stage stage,
                                          const std::array<u32, 8>& handles, u32 tex_pool, StageTranscript* capture,
                                          const StageTranscript* replay) {
        auto t = std::make_unique<Translated>();
        PHASE("env", nvn_addr);
        if (replay) {
            if (replay->stage != static_cast<u32>(stage)) throw std::runtime_error("shader cache: stage mismatch in record");
            t->env = std::make_unique<TranscriptEnv>(*replay);
        } else {
            if (!mem) throw std::runtime_error("translate: no guest memory and no record");
            t->env = std::make_unique<NvnEnv>(*mem, nvn_addr + 0x30, stage, handles, tex_pool, capture);
        }
        PHASE("cfg", nvn_addr);
        Shader::Maxwell::Flow::CFG cfg(*t->env, t->flow_block, t->env->StartAddress() + static_cast<u32>(sizeof(Shader::ProgramHeader)),
                                       stage == Shader::Stage::VertexB);
        PHASE("translate program body", nvn_addr);
        t->program = Shader::Maxwell::TranslateProgram(t->inst_pool, t->block_pool, *t->env, cfg, host_info);
        return t;
    }

    /// Diagnostics (capture): everything one draw is fed, for the comparison with suyu. Kept out of the draw
    /// function so its locals do not add to the guest fiber stack while shaders translate.
    NVN_VK_NOINLINE void DumpDrawState(Core::Memory::Memory& mem, const State& ds, const std::array<u32, 8> (&handles)[2],
                                            const PipelineKey& key, u32 count) {
        std::string o = fmt::format("DRAW vs={:x} fs={:x} count {} rt {}x{}\n", ds.prog[0], ds.prog[1], count, pass_w, pass_h);
        for (u32 ss = 0; ss < 2; ++ss) {
            for (u32 ii = 0; ii < 16; ++ii) {
                if (ds.ub_addr[ss][ii] == 0) continue;
                u32 ucpu = 0, uav = 0;
                std::array<u8, 256> ub{};
                if (NR::GpuToCpu(mem, ds.ub_addr[ss][ii], ucpu, uav)) mem.ReadBlock(ucpu, ub.data(), std::min<u32>(256, uav));
                std::string hex;
                for (const u8 v : ub) hex += fmt::format("{:02x}", v);
                o += fmt::format("  s{} cb{} size {:x} {}\n", ss, ii + 3, ds.ub_size[ss][ii], hex);
            }
            for (u32 ii = 0; ii < 8; ++ii) {
                if (handles[ss][ii] == 0) continue;
                u32 dcpu = 0;
                if (!NR::TextureDescriptor(ds.tex_pool, handles[ss][ii] & 0xfffff, dcpu)) continue;
                std::array<u32, 8> tw{};
                mem.ReadBlock(dcpu, tw.data(), 32);
                o += fmt::format("  s{} slot{} handle {:x} tic {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}\n", ss, ii, handles[ss][ii],
                                 tw[0], tw[1], tw[2], tw[3], tw[4], tw[5], tw[6], tw[7]);
            }
        }
        o += fmt::format("  state ds {:x} poly {:x} chan {:x} blend_en {:x} b0 {:02x}{:02x}{:02x}{:02x} color_state {:x} alpha_func {:x} vp {} {} {} {} sc {} {} {} {} depth_clamp {} polyoff {} {} {}\n",
                         key.ds, key.poly, key.chan_mask, key.blend_enable, key.blend[0][0], key.blend[0][1], key.blend[0][2], key.blend[0][3], ds.color_state,
                         key.alpha_func, ds.viewport[0], ds.viewport[1], ds.viewport[2], ds.viewport[3], ds.scissor[0], ds.scissor[1], ds.scissor[2],
                         ds.scissor[3], ds.depth_clamp, ds.poly_offset[0], ds.poly_offset[1], ds.poly_offset[2]);
        for (u32 ss = 0; ss < 2; ++ss) {
            for (u32 ii = 0; ii < 8; ++ii) {
                if (handles[ss][ii] == 0) continue;
                u32 scpu = 0;
                if (!NR::SamplerDescriptor(ds.sampler_pool, (handles[ss][ii] >> 20) & 0xfff, scpu)) continue;
                std::array<u32, 8> sw{};
                mem.ReadBlock(scpu, sw.data(), 32);
                o += fmt::format("  s{} samp{} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}\n", ss, ii, sw[0], sw[1], sw[2], sw[3], sw[4], sw[5], sw[6], sw[7]);
            }
        }
        for (u32 a = 0; a < 16; ++a) {
            if (ds.attrib[a] == 0) continue;
            const u32 w = AttribWord(ds.attrib[a] >> 20);
            o += fmt::format("  attr {} buf {} off {} word {:x}\n", a, ds.attrib[a] & 0x1f, (ds.attrib[a] >> 7) & 0x1fff, w & 0xfe7e);
        }
        for (u32 b = 0; b < 4; ++b) {
            if (ds.vb[b].addr == 0) continue;
            u32 vcpu = 0, vav = 0;
            std::array<u8, 48> vbytes{};
            if (NR::GpuToCpu(mem, ds.vb[b].addr, vcpu, vav)) mem.ReadBlock(vcpu, vbytes.data(), std::min<u32>(48, vav));
            std::string hex;
            for (const u8 v : vbytes) hex += fmt::format("{:02x}", v);
            o += fmt::format("  vb {} stride {} {}\n", b, ds.stride[b], hex);
        }
        NvnVkDumpLine("nvn_ours_draws.txt", o);
    }

    /// Diagnostics: FNV hash of a texture as the GPU holds it (all levels and layers, the layout suyu downloads),
    /// for comparison with suyu's copy of the same texture.
    void HashTexture(u32 slot, Image& img, const Tegra::Texture::TICEntry& tic) {
        using namespace VideoCommon;
        const ImageInfo info(tic);
        const auto copies = FullDownloadCopies(info);
        const size_t bytes = CalculateUnswizzledSizeBytes(info);
        if (bytes == 0 || img.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;
        auto buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0, .size = bytes,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr},
                                          MemoryUsage::Download);
        std::vector<VkBufferImageCopy> regions;
        for (const auto& c : copies) {
            regions.push_back({.bufferOffset = c.buffer_offset,
                               .bufferRowLength = c.buffer_row_length,
                               .bufferImageHeight = c.buffer_image_height,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<u32>(c.image_subresource.base_level),
                                                    static_cast<u32>(c.image_subresource.base_layer),
                                                    static_cast<u32>(c.image_subresource.num_layers)},
                               .imageOffset = {c.image_offset.x, c.image_offset.y, c.image_offset.z},
                               .imageExtent = {c.image_extent.width, c.image_extent.height, c.image_extent.depth}});
        }
        SettleUtil();
        util_fence.Reset();
        vk::CommandBuffer cb(util_cmd, device.GetDispatchLoader());
        cb.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr});
        cb.CopyImageToBuffer(*img.image, VK_IMAGE_LAYOUT_GENERAL, *buf, regions);
        cb.End();
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = 0, .pWaitSemaphores = nullptr,
                        .pWaitDstStageMask = nullptr, .commandBufferCount = 1, .pCommandBuffers = &util_cmd, .signalSemaphoreCount = 0,
                        .pSignalSemaphores = nullptr};
        CheckVk(queue.Submit(si, *util_fence), "vkQueueSubmit");
        {
            ProfScope ps{prof, PB_WaitDump};
            CheckVk(util_fence.Wait(), "fence wait");
        }
        buf.Invalidate();
        u64 hash = 1469598103934665603ull;
        for (const u8 v : buf.Mapped().subspan(0, bytes)) hash = (hash ^ v) * 1099511628211ull;
        NvnVkDumpLine("nvn_ours_texhash.txt", fmt::format("hull slot {} {}x{} layers {} levels {} vkformat {} bytes {} hash {:016x}", slot, img.width,
                                                          img.height, img.layers, img.levels, static_cast<int>(img.format), bytes, hash));
    }
    /// Diagnostics: the Maxwell format word (size, type) used for an NVN attribute code.
    static u32 AttribWord(u32 code) {
        if (u32 learned = 0; NR::LearnedAttrib(code, learned)) return learned;
        static constexpr u32 kPairs[][2] = {{0x044, 0x384}, {0x04a, 0x114}, {0x02c, 0x388}, {0x056, 0x086}, {0x04c, 0x094},
                                            {0x04e, 0x214}, {0x052, 0x386}, {0x022, 0x39e}, {0x058, 0x19e}, {0x050, 0x194},
                                            {0x05c, 0x382}, {0x07a, 0x160}};
        for (const auto& f : kPairs) {
            if (f[0] == code) return f[1];
        }
        return 0xffffffff;
    }
    static VkFormat AttribFormat(const Device& device, u32 code, bool& ok, u32& type_out) {
        struct F {
            u32 in, out;
        };
        static constexpr F kFormats[] = {{0x044, 0x384}, {0x04a, 0x114}, {0x02c, 0x388}, {0x056, 0x086}, {0x04c, 0x094},
                                         {0x04e, 0x214}, {0x052, 0x386}, {0x022, 0x39e}, {0x058, 0x19e}, {0x050, 0x194},
                                         {0x05c, 0x382}, {0x07a, 0x160}};
        if (u32 learned = 0; NR::LearnedAttrib(code, learned)) {
            // What the driver itself wrote always wins over the built-in table.
            const auto size = static_cast<Maxwell::VertexAttribute::Size>((learned >> 1) & 0x3f);
            const auto type = static_cast<Maxwell::VertexAttribute::Type>((learned >> 7) & 7);
            type_out = static_cast<u32>(type);
            ok = true;
            return MaxwellToVK::VertexFormat(device, type, size);
        }
        for (const auto& f : kFormats) {
            if (f.in == code) {
                const auto size = static_cast<Maxwell::VertexAttribute::Size>((f.out >> 1) & 0x3f);
                const auto type = static_cast<Maxwell::VertexAttribute::Type>((f.out >> 7) & 7);
                type_out = static_cast<u32>(type);
                ok = true;
                return MaxwellToVK::VertexFormat(device, type, size);
            }
        }
        if (u32 learned = 0; NR::LearnedAttrib(code, learned)) {
            const auto size = static_cast<Maxwell::VertexAttribute::Size>((learned >> 1) & 0x3f);
            const auto type = static_cast<Maxwell::VertexAttribute::Type>((learned >> 7) & 7);
            type_out = static_cast<u32>(type);
            ok = true;
            return MaxwellToVK::VertexFormat(device, type, size);
        }
        ok = false;
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }

    static VkBlendFactor Factor(u8 v) {
        switch (v) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 16: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 17: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
        }
    }

    VkRenderPass RenderPass(const PipelineKey& k) {
        std::string key(reinterpret_cast<const char*>(k.rt_format), sizeof(k.rt_format));
        key.append(reinterpret_cast<const char*>(&k.rt_count), 4);
        key.append(reinterpret_cast<const char*>(&k.depth_format), 4);
        if (const auto it = render_passes.find(key); it != render_passes.end()) return **it->second;
        ProfScope ps{prof, PB_Pipeline};
        std::vector<VkAttachmentDescription> att;
        std::vector<VkAttachmentReference> refs;
        for (u32 i = 0; i < k.rt_count; ++i) {
            att.push_back({0, static_cast<VkFormat>(k.rt_format[i]), VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                           VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
            refs.push_back({i, VK_IMAGE_LAYOUT_GENERAL});
        }
        VkAttachmentReference depth_ref{k.rt_count, VK_IMAGE_LAYOUT_GENERAL};
        if (k.depth_format) {
            att.push_back({0, static_cast<VkFormat>(k.depth_format), VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                           VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL});
        }
        const VkSubpassDescription sub{
            .flags = 0,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .inputAttachmentCount = 0,
            .pInputAttachments = nullptr,
            .colorAttachmentCount = k.rt_count,
            .pColorAttachments = refs.data(),
            .pResolveAttachments = nullptr,
            .pDepthStencilAttachment = k.depth_format ? &depth_ref : nullptr,
            .preserveAttachmentCount = 0,
            .pPreserveAttachments = nullptr,
        };
        auto rp = dev.CreateRenderPass({
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .attachmentCount = static_cast<u32>(att.size()),
            .pAttachments = att.data(),
            .subpassCount = 1,
            .pSubpasses = &sub,
            .dependencyCount = 0,
            .pDependencies = nullptr,
        });
        const VkRenderPass raw = *rp;
        render_passes[key] = std::make_unique<vk::RenderPass>(std::move(rp));
        return raw;
    }

    /// Translates and compiles one vertex + fragment pair. Runs on the guest thread (a miss during the
    /// replay) or on a pool thread (prescan). It touches no shared renderer state except Vulkan object
    /// creation and the dump counters (under dump_lock); notes go to `notes`, failures throw.
    /// capture: record what the environment answered (cold build). replay: build from a record (prewarm).
    NVN_VK_NOINLINE std::unique_ptr<Program> BuildProgram(Core::Memory::Memory* mem, u64 vs_addr, u64 fs_addr, u32 tex_pool,
                                                                const ShaderKey& sk, const std::array<u32, 8> (&handles)[2],
                                                                const bool (&attrib_enabled)[16], std::vector<std::string>& notes,
                                                                ProgramRecord* capture, const ProgramRecord* replay) {
        const struct {
            u64 prog[2];
            u32 tex_pool;
        } s{{vs_addr, fs_addr}, tex_pool};
        auto p = std::make_unique<Program>();
        {
            PHASE("translate vs", s.prog[0]);
            p->t[0] = Translate(mem, s.prog[0], Shader::Stage::VertexB, handles[0], s.tex_pool, capture ? &capture->stage[0] : nullptr,
                                replay ? &replay->stage[0] : nullptr);
            PHASE("translate fs", s.prog[1]);
            p->t[1] = Translate(mem, s.prog[1], Shader::Stage::Fragment, handles[1], s.tex_pool, capture ? &capture->stage[1] : nullptr,
                                replay ? &replay->stage[1] : nullptr);
            p->info[0] = p->t[0]->program.info;
            p->info[1] = p->t[1]->program.info;
            const bool dump_ir = Core::GameSettings::GpuDiagnosticsEnabled(); // IR / SPIR-V files of cold builds: diagnostics only
            for (u32 si = 0; si < 2 && dump_ir; ++si) {
                const u64 pa = s.prog[si];
                if (pa == 0x410220100ull || pa == 0x40fadee00ull || pa == 0x4102cdb00ull || pa == 0x40e27da00ull || pa == 0x40d513800ull ||
                    pa == 0x40cdc3a00ull || pa == 0x40ced0100ull || pa == 0x40e3ac800ull || pa == 0x40ded1300ull || pa == 0x40fd9a200ull ||
                    pa == 0x40dc5b400ull || pa == 0x40e4cc500ull || pa == 0x40e01ad00ull || pa == 0x40ff1c600ull || pa == 0x40e2c7f00ull || pa == 0x410215e00ull) {
                    std::ofstream ir(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / fmt::format("nvn_vk_shadow_ir_{:x}.txt", pa), std::ios::trunc);
                    ir << Shader::IR::DumpProgram(p->t[si]->program);
                }
            }
            for (u32 si = 0; si < 2 && dump_ir; ++si) {
                u32 dump_index = ~0u;
                {
                    Kernel::StallRing::TimedGuard dl{dump_lock, Kernel::StallRing::LockDump};
                    if (ir_dumped < 16) dump_index = ir_dumped++;
                }
                if (dump_index != ~0u) { // file I/O outside the lock: pool threads must not queue behind it
                    std::ofstream ir(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / fmt::format("nvn_vk_ir_{}.txt", dump_index), std::ios::trunc);
                    ir << fmt::format("stage {} program {:x}\n", si, s.prog[si]) << Shader::IR::DumpProgram(p->t[si]->program);
                }
            }
            Shader::RuntimeInfo vs_rt{};
            vs_rt.previous_stage_stores.mask.set();
            for (u32 i = 0; i < 32; ++i) vs_rt.generic_input_types[i] = Shader::AttributeType::Float;
            for (u32 i = 0; i < 16; ++i) {
                if (!attrib_enabled[i]) continue;
                switch (sk.attrib_types[i]) {
                case static_cast<u32>(Maxwell::VertexAttribute::Type::SInt):
                    vs_rt.generic_input_types[i] = Shader::AttributeType::SignedInt;
                    break;
                case static_cast<u32>(Maxwell::VertexAttribute::Type::UInt):
                    vs_rt.generic_input_types[i] = Shader::AttributeType::UnsignedInt;
                    break;
                default:
                    break;
                }
            }
            vs_rt.input_topology = Shader::InputTopology::Triangles;
            Shader::RuntimeInfo fs_rt{};
            fs_rt.previous_stage_stores = p->info[0].stores;
            fs_rt.previous_stage_legacy_stores_mapping = p->info[0].legacy_stores_mapping;
            fs_rt.input_topology = Shader::InputTopology::Triangles;
            if (sk.alpha_func != 8 && sk.alpha_func >= 1 && sk.alpha_func <= 8 && !(Core::GameSettings::NvnVkDebug() & 4)) {
                fs_rt.alpha_test_func = static_cast<Shader::CompareFunction>(sk.alpha_func - 1);
                fs_rt.alpha_test_dynamic = true;
            }
            PHASE("emit vs", s.prog[0]);
            Shader::Backend::Bindings binding;
            Shader::Maxwell::ConvertLegacyToGeneric(p->t[0]->program, vs_rt);
            const auto vs_code = Shader::Backend::SPIRV::EmitSPIRV(profile, vs_rt, p->t[0]->program, binding);
            Shader::Maxwell::ConvertLegacyToGeneric(p->t[1]->program, fs_rt);
            const auto fs_code = Shader::Backend::SPIRV::EmitSPIRV(profile, fs_rt, p->t[1]->program, binding);
            if (Kernel::StallRing::TimedGuard dl{dump_lock, Kernel::StallRing::LockDump}; dump_ir && spv_dumped < 8) {
                const auto dump = [&](const std::vector<u32>& code, const char* tag) {
                    std::ofstream f(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / fmt::format("nvn_vk_{}_{}.spv", tag, spv_dumped), std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
                };
                dump(vs_code, "vs");
                dump(fs_code, "fs");
                notes.push_back(fmt::format("  spv {} = vs {:x} fs {:x}", spv_dumped, s.prog[0], s.prog[1]));
                for (u32 si = 0; si < 2; ++si) {
                    const u32* w = reinterpret_cast<const u32*>(&p->t[si]->env->SPH());
                    std::string hex;
                    for (u32 i = 0; i < 20; ++i) hex += fmt::format("{:08x} ", w[i]);
                    notes.push_back(fmt::format("  sph {} stage {}: {}", spv_dumped, si, hex));
                }
                ++spv_dumped;
            }
            for (u32 si = 0; si < 2; ++si) {
                const u32* w = reinterpret_cast<const u32*>(&p->t[si]->env->SPH());
                std::string hex;
                for (u32 i = 0; i < 20; ++i) hex += fmt::format("{:08x} ", w[i]);
                notes.push_back(fmt::format("  hdr {:x} {}: {}", s.prog[si], si ? "ps" : "vs", hex));
            }
            const auto module = [&](const std::vector<u32>& code) {
                return dev.CreateShaderModule({
                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .codeSize = code.size() * sizeof(u32),
                    .pCode = code.data(),
                });
            };
            PHASE("modules", s.prog[0]);
            p->vs = module(vs_code);
            p->fs = module(fs_code);
#ifdef SUYU_HAS_FSR_VULKAN
            if (ObjvCaptureBuildable()) {
                std::vector<u32> xfb_code = vs_code;
                if (PatchXfbPosition(xfb_code)) {
                    p->vs_xfb = module(xfb_code);
                } else {
                    notes.push_back("nvn_fsr_objvel: vertex shader position could not be marked for transform feedback");
                }
            }
#endif
            for (u32 c = 3; c < Shader::Info::MAX_CBUFS; ++c) {
                p->vs_dyn_cbuf |= p->info[0].constant_buffer_used_sizes[c] >= 0x10000;
            }
            p->vs_tex = !p->info[0].texture_descriptors.empty();
            PHASE("layout", s.prog[0]);
            DescriptorLayoutBuilder builder{device};
            builder.Add(p->info[0], VK_SHADER_STAGE_VERTEX_BIT);
            builder.Add(p->info[1], VK_SHADER_STAGE_FRAGMENT_BIT);
            p->push = builder.CanUsePushDescriptor();
            if (!p->push) {
                notes.push_back("program: needs more than push descriptors");
                p->failed = true;
            } else {
                p->set_layout = builder.CreateDescriptorSetLayout(true);
                p->layout = builder.CreatePipelineLayout(*p->set_layout);
                p->update_template = builder.CreateTemplate(*p->set_layout, *p->layout, true);
            }
            PHASE("masks", s.prog[0]);
            for (u32 i = 0; i < 32; ++i) {
                if (p->info[0].loads.Generic(i)) p->vs_in_mask |= 1u << i;
            }
            for (u32 i = 0; i < 8; ++i) {
                if (p->info[1].stores_frag_color[i]) p->fs_color_mask |= 1u << i;
            }
            // Which driver constant buffers do the shaders read (cb 0..2)? Logged for the next step.
            for (u32 si = 0; si < 2; ++si) {
                const auto& info = p->t[si]->program.info;
                for (u32 c = 0; c < 3; ++c) {
                    if (info.constant_buffer_used_sizes[c] != 0) {
                        notes.push_back(fmt::format("stage {} reads cbuf {} (size {:x})", si, c, info.constant_buffer_used_sizes[c]));
                    }
                }
            }
            p->t[0].reset();
            p->t[1].reset();
        }
        return p;
    }

    // ---- shader cache: content keys, lookups, records ----------------------------------------------------
    /// Per texture slot: what the environment answers for its handle (type, pixel format) and the first
    /// slot holding the same handle. Equal tables give equal answers to every texture query on the slots.
    static void SlotAnswers(Core::Memory::Memory& mem, u32 tex_pool, const std::array<u32, 8>& h, u32 used, u32 (&out)[8][3]) {
        for (u32 i = 0; i < 8; ++i) {
            out[i][0] = out[i][1] = out[i][2] = 0;
            if (!(used & (1u << i))) continue; // not sampled by this stage: whatever is bound there is stale
            const auto tic = TicFor(mem, tex_pool, h[i]);
            out[i][0] = static_cast<u32>(TicTextureType(tic));
            out[i][1] = static_cast<u32>(TicPixelFormat(tic));
            u32 first = i;
            for (u32 j = 0; j < i; ++j) {
                if ((used & (1u << j)) && h[j] == h[i]) {
                    first = j;
                    break;
                }
            }
            out[i][2] = first;
        }
    }
    static u64 ProgramContentKey(u64 vs_hash, u64 fs_hash, const u32 (&attrib_types)[16], u32 alpha_func, const u8 (&ae)[16],
                                 const u32 (&slots)[2][8][3], u32 dbg) {
        u64 h = 1469598103934665603ull;
        FnvV(h, vs_hash);
        FnvV(h, fs_hash);
        Fnv(h, attrib_types, sizeof(attrib_types));
        FnvV(h, alpha_func);
        Fnv(h, ae, sizeof(ae));
        Fnv(h, slots, sizeof(slots));
        FnvV(h, dbg);
        return h ? h : 1; // 0 means "no key"
    }
    static u64 PipelineContentKey(u64 program_key, const PipelineKey& key, const std::vector<VkVertexInputAttributeDescription>& attributes,
                                  u8 flip, u32 dbg) {
        PipelineKey z = key;
        std::memset(z.prog, 0, sizeof(z.prog));
        std::memset(z.tex_sig, 0, sizeof(z.tex_sig));
        u64 h = 1469598103934665603ull;
        FnvV(h, program_key);
        Fnv(h, &z, offsetof(PipelineKey, restart) + sizeof(z.restart));
        const u32 n = static_cast<u32>(attributes.size());
        FnvV(h, n);
        if (n) Fnv(h, attributes.data(), n * sizeof(VkVertexInputAttributeDescription));
        FnvV(h, flip);
        FnvV(h, dbg);
        return h ? h : 1;
    }
    u64 PipelineContentKeyNow(u64 program_key, const PipelineKey& key, const std::vector<VkVertexInputAttributeDescription>& attributes) const {
        return PipelineContentKey(program_key, key, attributes, flip_front ? 1 : 0, static_cast<u32>(Core::GameSettings::NvnVkDebug()));
    }

    /// After a cold build: completes the record and returns its key, or 0 when the program cannot be keyed
    /// by content (a texture query on a handle that is not one of the bound slots).
    static u64 FinishProgramRecord(Core::Memory::Memory& mem, ProgramRecord& r, u32 tex_pool, const std::array<u32, 8> (&handles)[2],
                                   const ShaderKey& sk, const bool (&ae)[16]) {
        for (u32 si = 0; si < 2; ++si) {
            auto& t = r.stage[si];
            const auto bound = [&](u32 h) { return std::find(handles[si].begin(), handles[si].end(), h) != handles[si].end(); };
            for (const auto& kv : t.tex_type) {
                if (!bound(kv.first)) return 0;
            }
            for (const auto& kv : t.tex_format) {
                if (!bound(kv.first)) return 0;
            }
            FinishTranscript(t);
            SlotAnswers(mem, tex_pool, handles[si], UsedSlotMask(t), r.slots[si]);
        }
        std::memcpy(r.attrib_types, sk.attrib_types, sizeof(r.attrib_types));
        r.alpha_func = sk.alpha_func;
        for (u32 i = 0; i < 16; ++i) r.attrib_enabled[i] = ae[i] ? 1 : 0;
        r.dbg = static_cast<u32>(Core::GameSettings::NvnVkDebug());
        r.key = ProgramContentKey(r.stage[0].hash, r.stage[1].hash, r.attrib_types, r.alpha_func, r.attrib_enabled, r.slots, r.dbg);
        return r.key;
    }

    /// The recorded stage that is the program at this guest address, by content hash; 0 if none. Exact:
    /// every instruction word the recorded translation read must read the same now.
    NVN_VK_NOINLINE const StageTranscript* MatchStage(Core::Memory::Memory& mem, u64 nvn_addr, Shader::Stage stage, bool* had_candidates = nullptr) {
        const u64 sph_va = nvn_addr + 0x30;
        const u32 start = static_cast<u32>(sph_va & 0xff);
        const u64 base = sph_va - start;
        std::array<u8, 0x50> sph{};
        ReadGpuEnv(mem, sph_va, sph.data(), 0x50);
        u64 first = 0;
        ReadGpuEnv(mem, base + FirstCodeAddress(start), &first, 8);
        const auto it = warm_stage_index.find(StageQuick(static_cast<u32>(stage), start, sph.data(), first));
        if (it == warm_stage_index.end()) return nullptr;
        if (had_candidates) *had_candidates = true;
        for (const StageTranscript* t : it->second) {
            if (t->stage != static_cast<u32>(stage) || t->start_address != start || std::memcmp(t->sph.data(), sph.data(), 0x50) != 0) continue;
            bool same = true;
            for (const auto& [a, v] : t->instr) {
                u64 now = 0;
                ReadGpuEnv(mem, base + a, &now, 8);
                if (now != v) {
                    same = false;
                    break;
                }
            }
            if (same) return t;
        }
        return nullptr;
    }

    /// One-time diagnostic (capped at 50 lines): a program missed the warm cache although the file has an
    /// entry for the same stage code. Says which part of the key differs.
    NVN_VK_NOINLINE void DiagWarmMiss(u64 vs_addr, u64 fs_addr, bool vs_cand, bool fs_cand, const StageTranscript* vt,
                                           const StageTranscript* ft, const ShaderKey& sk, const u8 (&ae8)[16], const u32 (&slots)[2][8][3]) {
        if (warm_diag_lines >= 50) return;
        const auto line = [&](const std::string& s) {
            if (warm_diag_lines < 50) Note(fmt::format("warm miss {:x} {:x}: {}", vs_addr, fs_addr, s));
            ++warm_diag_lines;
        };
        if (!vt || !ft) {
            line(fmt::format("stage code differs (vs: {}, fs: {}); an entry with the same stage/start/SPH/first word exists",
                             vt ? "same" : (vs_cand ? "words differ" : "no entry"), ft ? "same" : (fs_cand ? "words differ" : "no entry")));
            return;
        }
        const u32 dbg = static_cast<u32>(Core::GameSettings::NvnVkDebug());
        int shown = 0;
        size_t same_stages = 0;
        for (const auto& [key, e] : warm_programs) {
            const ProgramRecord& r = *e->rec;
            if (r.stage[0].hash != vt->hash || r.stage[1].hash != ft->hash) continue;
            ++same_stages;
            if (shown >= 2) continue;
            ++shown;
            std::string d;
            if (std::memcmp(r.attrib_types, sk.attrib_types, sizeof(r.attrib_types)) != 0) d += " attrib_types";
            if (r.alpha_func != sk.alpha_func) d += fmt::format(" alpha_func({} vs {})", r.alpha_func, sk.alpha_func);
            if (std::memcmp(r.attrib_enabled, ae8, sizeof(ae8)) != 0) d += " attrib_enabled";
            if (r.dbg != dbg) d += " dbg";
            for (u32 si = 0; si < 2; ++si) {
                for (u32 i = 0; i < 8; ++i) {
                    for (u32 c = 0; c < 3; ++c) {
                        if (r.slots[si][i][c] != slots[si][i][c]) {
                            d += fmt::format(" stage{}.slot{}.{}({} vs {})", si, i, c == 0 ? "type" : c == 1 ? "format" : "sharing", r.slots[si][i][c], slots[si][i][c]);
                        }
                    }
                }
            }
            line(d.empty() ? "entry has the same key parts (key hash differs?)" : "differs in:" + d);
        }
        if (same_stages == 0) line("no entry has this vs/fs stage pair (new pairing of known stages)");
    }

    /// Waits (guest thread) until a prewarm entry is finished; the wait goes to the prewarm bucket.
    template <typename F>
    void WaitWarm(F ready) {
        std::unique_lock lk{warm_lock};
        if (ready()) return;
        const u64 t0 = ProfNow();
        warm_cv.wait(lk, ready);
        prof.Add(PB_Prewarm, ProfNow() - t0);
    }

    /// The prewarmed program for this draw's program pair, or null when the cache does not have it.
    NVN_VK_NOINLINE std::shared_ptr<Program> WarmProgramFor(Core::Memory::Memory& mem, u64 vs_addr, u64 fs_addr, u32 tex_pool,
                                                                 const ShaderKey& sk, const std::array<u32, 8> (&handles)[2],
                                                                 const bool (&ae)[16]) {
        if (warm_programs.empty()) {
            prof.Count(PC_ProgMiss);
            return {};
        }
        bool vs_cand = false, fs_cand = false;
        const StageTranscript* vt = MatchStage(mem, vs_addr, Shader::Stage::VertexB, &vs_cand);
        const StageTranscript* ft = vt ? MatchStage(mem, fs_addr, Shader::Stage::Fragment, &fs_cand) : nullptr;
        u32 slots[2][8][3] = {};
        u8 ae8[16];
        for (u32 i = 0; i < 16; ++i) ae8[i] = ae[i] ? 1 : 0;
        if (vt && ft) {
            SlotAnswers(mem, tex_pool, handles[0], UsedSlotMask(*vt), slots[0]);
            SlotAnswers(mem, tex_pool, handles[1], UsedSlotMask(*ft), slots[1]);
            const u64 key = ProgramContentKey(vt->hash, ft->hash, sk.attrib_types, sk.alpha_func, ae8, slots, static_cast<u32>(Core::GameSettings::NvnVkDebug()));
            if (const auto it = warm_programs.find(key); it != warm_programs.end()) return TakeWarmProgram(*it->second, key);
        }
        if (!vt) MatchStage(mem, fs_addr, Shader::Stage::Fragment, &fs_cand);
        prof.Count(PC_ProgMiss);
        DiagWarmMiss(vs_addr, fs_addr, vs_cand, fs_cand, vt, ft, sk, ae8, slots);
        return {};
    }

    template <typename E>
    std::shared_ptr<Program> TakeWarmProgram(E& e, u64 key) {
        WaitWarm([&] { return e.done; });
        if (!e.error.empty()) Fatal(fmt::format("shader cache: prewarmed program {:x} failed: {}", key, e.error));
        if (!e.notes_merged) {
            for (const auto& n : e.notes) ++stats.notes[n];
            e.notes_merged = true;
        }
        prof.Count(PC_WarmHits);
        if (e.from_run) prof.Count(PC_WarmRunHits);
        return e.prog;
    }

    /// Makes a program built in this run findable by the warm lookup (same as a file record after prewarm).
    void AddRunProgram(std::unique_ptr<ProgramRecord> rec, const std::shared_ptr<Program>& prog) {
        if (rec->key == 0 || warm_programs.contains(rec->key)) return;
        auto e = std::make_unique<WarmProgramEntry>();
        e->rec = rec.get();
        e->prog = prog;
        e->done = true;
        e->notes_merged = true;
        e->from_run = true;
        for (const auto& t : rec->stage) warm_stage_index[t.quick].push_back(&t);
        warm_programs.emplace(rec->key, std::move(e));
        program_records.push_back(std::move(rec));
    }

    /// The prewarmed pipeline with this content key, or null.
    std::shared_ptr<Pipeline> WarmPipelineFor(u64 content_key) {
        const auto it = warm_pipelines.find(content_key);
        if (it == warm_pipelines.end()) return {};
        WarmPipelineEntry& e = *it->second;
        WaitWarm([&] { return e.done; });
        if (!e.error.empty()) Fatal(fmt::format("shader cache: prewarmed pipeline {:x} failed: {}", content_key, e.error));
        prof.Count(PC_WarmHits);
        return e.pipe;
    }

    /// Run Q probe (logging only): a program was just built cold although its content key is already recorded
    /// ("rebuilt_file" = it was in the cache file at start, "rebuilt_run" = built earlier in this run). For the first
    /// file cases, says which part of the stage lookup key differs from the record.
    u32 rebuilt_diag{};
    void NoteRebuilt(Core::Memory::Memory& mem, u64 vs, u64 fs, const ProgramRecord& r) {
        if (r.key == 0 || !recorded_programs.contains(r.key)) return;
        const bool on_file = loaded_programs.contains(r.key);
        prof.Count(on_file ? PC_RebuiltFile : PC_RebuiltRun);
        if (!on_file || rebuilt_diag >= 20) return;
        ++rebuilt_diag;
        const ProgramRecord* old = nullptr;
        for (const auto& pr : program_records) {
            if (pr->key == r.key) {
                old = pr.get();
                break;
            }
        }
        std::string s = fmt::format("rebuilt although on file: vs {:x} fs {:x} key {:x} record {}", vs, fs, r.key, old ? "found" : "NOT FOUND");
        for (u32 si = 0; si < 2 && old; ++si) {
            const u64 sph_va = (si ? fs : vs) + 0x30;
            const u32 start = static_cast<u32>(sph_va & 0xff);
            std::array<u8, 0x50> sph{};
            ReadGpuEnv(mem, sph_va, sph.data(), 0x50);
            u64 first = 0;
            ReadGpuEnv(mem, (sph_va & ~u64{0xff}) + FirstCodeAddress(start), &first, 8);
            const StageTranscript& ot = old->stage[si];
            const u64* of = FindInstr(ot, FirstCodeAddress(ot.start_address));
            const u64 qnow = StageQuick(ot.stage, start, sph.data(), first);
            s += fmt::format(" | stage{} start now {:x} file {:x}, sph {}, first word now {:x} file {}, quick {}, file instrs {}", si, start,
                             ot.start_address, std::memcmp(ot.sph.data(), sph.data(), 0x50) == 0 ? "same" : "DIFFERS", first,
                             of ? fmt::format("{:x}", *of) : std::string("(not in transcript)"), qnow == ot.quick ? "same" : "DIFFERS",
                             ot.instr.size());
        }
        Note(s);
    }

    Program* GetProgram(Core::Memory::Memory& mem, u64 vs_addr, u64 fs_addr, u32 tex_pool, const ShaderKey& sk,
                        const std::array<u32, 8> (&handles)[2], const bool (&attrib_enabled)[16]) {
        if (const auto it = programs.find(sk); it != programs.end()) return it->second.get();
        if (auto w = WarmProgramFor(mem, vs_addr, fs_addr, tex_pool, sk, handles, attrib_enabled)) {
            auto* raw = w.get();
            programs[sk] = std::move(w);
            return raw;
        }
        PHASE("translate program", sk.prog[0]);
        ProfScope ps{prof, PB_Translate};
        prof.Count(PC_Programs);
        prof.Count(PC_LatePrograms);
        prof.Count(PC_ColdBuilds);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::string> notes;
        std::unique_ptr<Program> p;
        auto rec = std::make_unique<ProgramRecord>(); // heap: the guest fiber stack is small
        try {
            p = BuildProgram(&mem, vs_addr, fs_addr, tex_pool, sk, handles, attrib_enabled, notes, rec.get(), nullptr);
        } catch (const std::exception& e) {
            ++stats.shader_fail;
            ++stats.notes[fmt::format("shader failure: {}", e.what())];
            Fatal(fmt::format("shader failure: {} (phase {}, line {}, programs {:x} {:x})", e.what(), g_phase, static_cast<int>(g_line), vs_addr, fs_addr));
        }
        for (const auto& n : notes) ++stats.notes[n];
        ++stats.shader_ok;
        stats.translate_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        p->content_key = FinishProgramRecord(mem, *rec, tex_pool, handles, sk, attrib_enabled);
        NoteRebuilt(mem, vs_addr, fs_addr, *rec);
        WriteProgramRecord(*rec);
        auto* raw = p.get();
        programs[sk] = std::move(p);
        AddRunProgram(std::move(rec), programs[sk]);
        return raw;
    }

    // ---- recording ----------------------------------------------------------------------------------
    struct Transition {
        VkImage image;
        VkImageAspectFlags aspect;
    };
    struct TexUpload {
        VkImage image;
        VkImageAspectFlags aspect;
        u32 levels, layers;
        size_t arena_offset;
        boost::container::small_vector<VideoCommon::BufferImageCopy, 16> copies;
    };

    // ---- GPU pass profile (only with [Debug] gpu_diagnostics = true; nvn_perf_gpu = 0 turns just this part off) ----
    // One timestamp per finished render pass / marked compute stage in the frame's command buffer(s), BOTTOM_OF_PIPE. The GPU time
    // between two marks is attributed to the later mark's label (so a pass's interval also holds the barrier, copies and uploads that
    // precede it). Results are read when the slot's fence has signalled (BeginFrame of its reuse), summed per label per second and
    // written into nvn_perf.txt. Off = one bool test per pass end; nothing is recorded or allocated.
    static constexpr u32 kGpQ = 1024; // marks per slot (a frame has ~50-150)
    struct GpSlot {
        u32 n{};
        u64 frame{};
        std::array<u16, kGpQ> label{};
        std::array<u16, kGpQ> draws{};
    };
    struct GpAgg {
        double sum{}, max{};
        u64 cnt{}, draws{};
    };
    std::array<GpSlot, kSlots> gp_slot;
    bool gp_ini{true}, gp_on{}, gp_ready{};
    vk::QueryPool gp_pool;
    PFN_vkCmdWriteTimestamp gp_write{};
    double gp_period{};
    std::vector<std::string> gp_names;
    std::unordered_map<std::string, u32> gp_name_ids;
    std::unordered_map<u64, u32> gp_pass_ids;
    std::vector<GpAgg> gp_agg;
    u32 gp_pass_label{}, gp_pass_draws{};
    double gp_span_sum{}, gp_slow_span{};
    u64 gp_overflow{}, gp_dropped{}, gp_secs{}, gp_last_read_frame{}, gp_sec_start{};
    std::map<u64, double> gp_by_frame;
    std::string gp_slow_timeline;

    u32 GpLabel(const std::string& name) {
        const auto it = gp_name_ids.find(name);
        if (it != gp_name_ids.end()) return it->second;
        const u32 id = static_cast<u32>(gp_names.size());
        gp_names.push_back(name);
        gp_agg.emplace_back();
        gp_name_ids.emplace(name, id);
        return id;
    }
    void GpInit() {
        gp_ready = true;
        gp_on = prof.on && prof.perf.is_open() && gp_ini;
        if (!gp_on) return;
        try {
            gp_write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(device.GetDispatchLoader().vkGetDeviceProcAddr(*dev, "vkCmdWriteTimestamp"));
            gp_pool = dev.CreateQueryPool({VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, kGpQ * kSlots, 0});
            gp_period = device.GetPhysical().GetProperties().limits.timestampPeriod;
            GpLabel("start");
            gp_sec_start = ProfNow();
            if (!gp_write) gp_on = false;
        } catch (const std::exception& e) {
            gp_on = false;
            prof.PerfLine(std::string("# GPU timestamps unavailable: ") + e.what());
        }
    }
    void GpMark(u32 label, u32 draws = 0) {
        if (!gp_on) return;
        GpSlot& s = gp_slot[cur];
        if (s.n >= kGpQ) {
            ++gp_overflow;
            return;
        }
        gp_write(*vkcmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *gp_pool, cur * kGpQ + s.n);
        s.label[s.n] = static_cast<u16>(label);
        s.draws[s.n] = static_cast<u16>(std::min<u32>(draws, 65535));
        ++s.n;
    }
    void GpMarkNamed(const char* name) {
        if (gp_on) GpMark(GpLabel(name));
    }
    /// BeginFrame, after the slot's fence wait and before its fence reset: take the finished slot's timestamps.
    void GpRead(u32 slot) {
        GpSlot& s = gp_slot[slot];
        const u32 n = s.n;
        s.n = 0;
        if (n < 2) return;
        u64 t[kGpQ];
        if (frames[slot].fence.GetStatus() != VK_SUCCESS ||
            dev.GetQueryResults(*gp_pool, slot * kGpQ, n, n * sizeof(u64), t, sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
            ++gp_dropped;
            return;
        }
        double span = 0.0;
        std::string line;
        for (u32 i = 1; i < n; ++i) {
            if (t[i] < t[i - 1]) continue;
            const double d = double(t[i] - t[i - 1]) * gp_period / 1e6;
            GpAgg& a = gp_agg[s.label[i]];
            a.sum += d;
            a.max = std::max(a.max, d);
            ++a.cnt;
            a.draws += s.draws[i];
            span += d;
            if (i < 400) line += fmt::format(" {}:{:.3f}:{}", gp_names[s.label[i]], d, s.draws[i]);
        }
        gp_span_sum += span;
        gp_by_frame[s.frame] += span;
        gp_last_read_frame = std::max(gp_last_read_frame, s.frame);
        if (span > gp_slow_span) {
            gp_slow_span = span;
            gp_slow_timeline = fmt::format("T frame={} submit_gpu={:.3f} marks={} (label:ms:draws, in order){}", s.frame, span, n, line);
        }
    }
    /// BeginFrame, after vkcmd.Begin.
    void GpStart() {
        if (!gp_ready) GpInit();
        if (!gp_on) return;
        GpSlot& s = gp_slot[cur];
        s.n = 0;
        s.frame = prof.frame + 1;
        vkcmd.ResetQueryPool(*gp_pool, cur * kGpQ, kGpQ);
        gp_pass_draws = 0;
        GpMark(0);
    }
    u32 GpPassLabel(u32 pw, u32 ph, u32 rt_count, bool depth) {
        const u64 k = (u64{pw} << 40) | (u64{ph} << 16) | (u64{rt_count} << 4) | (depth ? 1u : 0u);
        const auto it = gp_pass_ids.find(k);
        if (it != gp_pass_ids.end()) return it->second;
        const u32 id = GpLabel(fmt::format("pass_{}x{}_c{}_{}", pw, ph, rt_count, depth ? "d" : "nd"));
        gp_pass_ids.emplace(k, id);
        return id;
    }
    /// Second boundary (from Prof::PerfSecond): GPU part of the S line.
    std::string GpSecond(u64 presents) {
        std::string tag = " fsr=off";
#ifdef SUYU_HAS_FSR_VULKAN
        if (fsr_active && !fsr_failed) {
            tag = " fsr=3.1";
#ifdef SUYU_HAS_FSR4_BRIDGE
            if (fsr4 && fsr4_ok && fsr4_want && fsr4_ran_last) tag = " fsr=4";
#endif
            // objvel = geometry object vectors (transform-feedback capture, Numpad9), blockmatch = 8x8 block matching of the
            // FSR input (Numpad9, ini nvn_fsr_objmv, default ON), anim = animated-surface reactive (Numpad0)
            tag += fmt::format(" objvel={} blockmatch={} anim={}", objv_on ? 1 : 0, fsr_objmv ? 1 : 0, fsr_anim ? 1 : 0);
            // objv_cap = object-velocity captures per frame (game draws run with the capture variant + capture-only draws)
            tag += fmt::format(" objv_cap={:.1f}", double(objv_perf_caps) / double(std::max<u64>(1, presents)));
#ifdef SUYU_HAS_FSR4_BRIDGE
            // FSR 4: D3D12 GPU time of ffxDispatch (last full second of nvn_fsr.txt's FSR backend line). The cross-API handoff
            // overhead ~= g.fsr4_dx12_gap_incl_queue_wait - fsr4_d3d12_ms (calibrated timestamps are not enabled on the device)
            if (fsr_backend_ini == 4) tag += fmt::format(" fsr4_d3d12_ms={:.3f}", fsr4_sec_d3d12_ms);
#endif
        }
        objv_perf_caps = 0;
        if (bench_active) tag += BenchSecondTag(); // tags this second with the run / state / phase, then moves the state machine on
#endif
        if (!gp_on) return tag + " gpu=off";
        const u64 now = ProfNow();
        const double secs = double(now - gp_sec_start) / 1e9;
        gp_sec_start = now;
        std::vector<float> fr;
        for (auto it = gp_by_frame.begin(); it != gp_by_frame.end();) {
            if (it->first + 3 <= gp_last_read_frame) { // older than the in-flight slots: complete
                fr.push_back(static_cast<float>(it->second));
                it = gp_by_frame.erase(it);
            } else {
                break;
            }
        }
        std::sort(fr.begin(), fr.end());
        const double n = static_cast<double>(std::max<u64>(1, presents));
        std::string s = tag + fmt::format(" gpu_avg={:.3f} gpu_util={:.1f} gpu_p50={:.3f} gpu_p95={:.3f} gpu_p99={:.3f} gpu_max={:.3f} gpu_frames={} gpu_dropped={} "
                                    "gpu_overflow={}",
                                    gp_span_sum / n, secs > 0 ? 100.0 * gp_span_sum / (secs * 1000.0) : 0.0, Prof::Pct(fr, 0.5), Prof::Pct(fr, 0.95),
                                    Prof::Pct(fr, 0.99), fr.empty() ? 0.0f : fr.back(), fr.size(), gp_dropped, gp_overflow);
        std::vector<u32> order;
        for (u32 i = 1; i < gp_agg.size(); ++i) {
            if (gp_agg[i].cnt) order.push_back(i);
        }
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return gp_agg[a].sum > gp_agg[b].sum; });
        for (size_t i = 0; i < order.size() && i < 24; ++i) {
            const GpAgg& a = gp_agg[order[i]];
            s += fmt::format(" g.{}={:.3f}/{:.3f}/{:.1f}", gp_names[order[i]], a.sum / n, a.max, double(a.draws) / n);
        }
        for (auto& a : gp_agg) a = {};
        gp_span_sum = 0.0;
        if (++gp_secs % 5 == 0 && !gp_slow_timeline.empty()) {
            prof.PerfLine(gp_slow_timeline);
            gp_slow_timeline.clear();
            gp_slow_span = 0.0;
        }
        return s;
    }

    void BeginFrame() {
        cur = (cur + 1) % kSlots;
        auto& f = frames[cur];
        if (f.pending) {
            ProfScope ps{prof, PB_WaitFrame};
            CheckVk(f.fence.Wait(), "fence wait");
            f.pending = false;
            ++completed_serial_hint;
        }
        if (gp_on) GpRead(cur);
        f.fence.Reset();
        f.used = 0;
        up_cache.clear();
        idx_range_cache.clear();
        tex_bind_cache.clear();
        rt_memo.clear();
        rtkey_memo.clear();
        VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .pNext = nullptr,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                    .pInheritanceInfo = nullptr};
        vkcmd = vk::CommandBuffer(f.cmd, device.GetDispatchLoader());
        vkcmd.Begin(bi);
        GpStart();
        frame_open = true;
        end_cmd_override = VK_NULL_HANDLE; // nvn_fsr_backend = 4 split (EndFrame)
        end_wait_sem = VK_NULL_HANDLE;
        end_wait_value = 0;
        pass_active = false;
        query_on = NvnVkCaptureActive();
        pick_on = NvnVkCaptureActive();
        pn = 0;
        qn = 0;
        if (query_on) {
            if (!qpool) {
                qpool = dev.CreateQueryPool({VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_OCCLUSION, 256, 0});
            }
            vkcmd.ResetQueryPool(*qpool, 0, 256);
        }
        // Free what no submission can still use.
        std::erase_if(graveyard, [&](const auto& g) { return g.first + kSlots + 1 < serial; });
        std::erase_if(fb_graveyard, [&](const auto& g) { return g.first + kSlots + 1 < serial; });
    }

    void EndFrame() {
        EndPass();
        GpMarkNamed("tail_after_last_pass");
        vkcmd.End();
        auto& f = frames[cur];
        // nvn_fsr_backend = 4 split the frame (FsrDispatch): the slot's second command buffer is submitted here and waits
        // for the D3D12 FSR 4 dispatch (timeline value); the first part was submitted at the split
        VkCommandBuffer cb = end_cmd_override ? end_cmd_override : f.cmd;
        const VkSemaphore wait_sem = end_wait_sem;
        const bool wait = wait_sem != VK_NULL_HANDLE;
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        const u64 wait_value = end_wait_value;
        const VkTimelineSemaphoreSubmitInfo tsi{.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                .pNext = nullptr,
                                                .waitSemaphoreValueCount = 1,
                                                .pWaitSemaphoreValues = &wait_value,
                                                .signalSemaphoreValueCount = 0,
                                                .pSignalSemaphoreValues = nullptr};
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .pNext = wait ? &tsi : nullptr,
                              .waitSemaphoreCount = wait ? 1u : 0u,
                              .pWaitSemaphores = wait ? &wait_sem : nullptr,
                              .pWaitDstStageMask = wait ? &wait_stage : nullptr,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &cb,
                              .signalSemaphoreCount = 0,
                              .pSignalSemaphores = nullptr};
        end_cmd_override = VK_NULL_HANDLE;
        end_wait_sem = VK_NULL_HANDLE;
        end_wait_value = 0;
        {
            ProfScope ps{prof, PB_Submit};
            CheckVk(queue.Submit(si, *f.fence), "vkQueueSubmit");
        }
        f.pending = true;
        frame_open = false;
        ++serial;
    }

    void AbandonFrame() {
        // Finish what was recorded so the fence and command buffer stay consistent.
        try {
            EndFrame();
        } catch (...) {
            frame_open = false;
        }
    }

    void Barrier(VkPipelineStageFlags src, VkPipelineStageFlags dst, VkAccessFlags sa, VkAccessFlags da) {
        const VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sa, da};
        vkcmd.PipelineBarrier(src, dst, 0, mb);
    }

    void FlushTransitions() {
        for (const auto& t : pending_transitions) {
            const VkImageMemoryBarrier b{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = t.image,
                .subresourceRange = {t.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
            };
            vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, b);
        }
        pending_transitions.clear();
    }

    void FlushTextureUploads() {
        for (auto& u : texture_uploads) {
            const VkImageMemoryBarrier to_dst{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = u.image,
                .subresourceRange = {u.aspect, 0, u.levels, 0, u.layers},
            };
            vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, to_dst);
            std::vector<VkBufferImageCopy> regions;
            for (const auto& c : u.copies) {
                regions.push_back({
                    .bufferOffset = u.arena_offset + c.buffer_offset,
                    .bufferRowLength = c.buffer_row_length,
                    .bufferImageHeight = c.buffer_image_height,
                    .imageSubresource = {u.aspect, static_cast<u32>(c.image_subresource.base_level),
                                         static_cast<u32>(c.image_subresource.base_layer),
                                         static_cast<u32>(c.image_subresource.num_layers)},
                    .imageOffset = {c.image_offset.x, c.image_offset.y, c.image_offset.z},
                    .imageExtent = {c.image_extent.width, c.image_extent.height, c.image_extent.depth},
                });
            }
            vkcmd.CopyBufferToImage(*frames[cur].arena, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions);
            const VkImageMemoryBarrier to_read{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = u.image,
                .subresourceRange = {u.aspect, 0, u.levels, 0, u.layers},
            };
            vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, to_read);
        }
        texture_uploads.clear();
    }

    void EndPass() {
        if (pass_active) {
            vkcmd.EndRenderPass();
            pass_active = false;
            Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            if (gp_on) {
                GpMark(gp_pass_label, gp_pass_draws);
                gp_pass_draws = 0;
            }
        }
    }

    // Starts (or restarts after state change) the render pass for the current targets.
    bool BeginPass(Core::Memory::Memory& mem, const PipelineKey& key) {
        if (pass_active && pass_key_rt == key.rt_count && !pass_dirty) return true;
        EndPass();
        FlushTransitions();
        FlushTextureUploads();
        std::vector<VkImageView> views;
        u32 w = 0, h = 0, pw = 0, ph = 0;
        // nvn_fsr_split_scale: allocated sizes of all attachments (1X present images / HUD layer next to render-scale ones?)
        u32 min_pw = ~0u, min_ph = ~0u, max_pw = 0, max_ph = 0;
        bool via_layer = false, any_out = false;
        const auto track = [&](u32 apw, u32 aph) {
            min_pw = std::min(min_pw, apw);
            min_ph = std::min(min_ph, aph);
            max_pw = std::max(max_pw, apw);
            max_ph = std::max(max_ph, aph);
        };
        for (u32 i = 0; i < st.rt_count; ++i) {
            Image* img = RenderTarget(mem, st.rt_tex[i], false);
            if (!img) return false;
            u32 apw = img->pw, aph = img->ph;
#ifdef SUYU_HAS_FSR_VULKAN
            // nvn_fsr HUD layer: while the redirect is on, H's attachment is the HUD layer (same format; same size, or 1X
            // with the split scale)
            if (fsr_tm_target && img == fsr_layer_h && fsr_tm) {
                views.push_back(*fsr_tm->view); // multiply draw, second pass: the transmittance image
                apw = fsr_tm->width;
                aph = fsr_tm->height;
                via_layer = true;
            } else if (fsr_layer_on && img == fsr_layer_h && fsr_layer) {
                views.push_back(*fsr_layer->view);
                apw = fsr_layer->width;
                aph = fsr_layer->height;
                via_layer = true;
            } else {
                views.push_back(*img->view);
            }
#else
            views.push_back(*img->view);
#endif
            w = img->width;
            h = img->height;
            pw = apw;
            ph = aph;
            track(apw, aph);
            any_out |= img->out_scaled;
        }
        if (key.depth_format) {
            Image* d = RenderTarget(mem, st.depth_tex, true);
            if (!d) return false;
            views.push_back(*d->view);
            track(d->pw, d->ph);
            if (w == 0) {
                w = d->width;
                h = d->height;
                pw = d->pw;
                ph = d->ph;
            }
        }
        if (split_on && (any_out || via_layer) && (min_pw != max_pw || min_ph != max_ph)) {
            // A 1X attachment (HUD layer / present image) next to render-scale ones (depth, MRT): one framebuffer size
            // cannot serve both.
#ifdef SUYU_HAS_FSR_VULKAN
            if (via_layer && (fsr_layer_on || fsr_tm_target)) {
                // HUD layer: back into H for the rest of this frame (H at the render scale), then this pass again
                SplitNote("layer pass", fmt::format("a HUD-layer pass binds render-scale attachments too ({}x{} vs {}x{}): HUD layer "
                                                    "abandoned for that frame (HUD at the render size, upscaled)",
                                                    max_pw, max_ph, min_pw, min_ph));
                const bool tm_pass = fsr_tm_target;
                fsr_tm_target = false;
                FsrLayerAbort("pass mixes the 1X HUD layer with render-scale attachments (split scale)");
                // the transmittance pass repeats a draw already made: after the merge it must not reach H a second time
                if (tm_pass) return false;
                if (!fsr_layer_on) return BeginPass(mem, key);
            }
#endif
            if (any_out) {
                SplitFail(fmt::format("a pass binds a 1X present image with render-scale attachments ({}x{} vs {}x{})", max_pw, max_ph,
                                      min_pw, min_ph));
            }
            // valid for this pass: the framebuffer fits every attachment (the 1X image is re-created at the render scale next)
            pw = min_pw;
            ph = min_ph;
        }
        FlushTransitions();
        const VkRenderPass rp = RenderPass(key);
        std::string fkey(reinterpret_cast<const char*>(views.data()), views.size() * sizeof(VkImageView));
        fkey.append(reinterpret_cast<const char*>(&rp), sizeof(rp));
        auto it = framebuffers.find(fkey);
        if (it == framebuffers.end()) {
            ProfScope ps{prof, PB_Pipeline};
            auto fb = dev.CreateFramebuffer({
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .renderPass = rp,
                .attachmentCount = static_cast<u32>(views.size()),
                .pAttachments = views.data(),
                .width = pw,
                .height = ph,
                .layers = 1,
            });
            it = framebuffers.emplace(fkey, std::make_unique<vk::Framebuffer>(std::move(fb))).first;
        }
        const VkRenderPassBeginInfo bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = rp,
            .framebuffer = **it->second,
            .renderArea = {{0, 0}, {pw, ph}},
            .clearValueCount = 0,
            .pClearValues = nullptr,
        };
        vkcmd.BeginRenderPass(bi, VK_SUBPASS_CONTENTS_INLINE);
        pass_active = true;
        pass_dirty = false;
        if (gp_on) gp_pass_label = GpPassLabel(pw, ph, key.rt_count, key.depth_format != 0);
        pass_key_rt = key.rt_count;
        pass_w = w;
        pass_h = h;
        pass_pw = pw;
        pass_ph = ph;
        pass_viewport_applied = false;
        return true;
    }

    /// The real pass creates missing render targets (peek = false); the prescan only looks (peek = true).
    VkFormat RtKeyFormat(Core::Memory::Memory& mem, u32 tex, bool depth, bool peek) {
        if (peek) return PeekRtFormat(mem, tex, depth);
        Image* img = RenderTarget(mem, tex, depth);
        return img ? img->format : VK_FORMAT_UNDEFINED;
    }

    /// Pipeline key for a draw with state `cs` (the live state in the replay, a copy in the prescan).
    PipelineKey MakeKey(Core::Memory::Memory& mem, const State& cs, u32 topology, const std::array<u32, 8> (&handles)[2], bool& ok,
                        bool peek) {
        PipelineKey k{};
        ok = true;
        k.prog[0] = cs.prog[0];
        k.prog[1] = cs.prog[1];
        // Texture signature: what the shaders' translation depends on, per bound slot.
        for (u32 s = 0; s < 2; ++s) {
            for (u32 i = 0; i < 8; ++i) {
                if (handles[s][i] == 0) continue;
                u32 cpu = 0;
                Tegra::Texture::TICEntry tic{};
                if (NR::TextureDescriptor(cs.tex_pool, handles[s][i] & 0xfffff, cpu)) mem.ReadBlock(cpu, &tic, sizeof(tic));
                std::array<u32, 8> w;
                std::memcpy(w.data(), &tic, 32);
                // the translation sees the texture type and pixel format, nothing else (no addresses)
                k.tex_sig[s * 8 + i] = 0x80000000u | (static_cast<u32>(TicTextureType(tic)) << 16) | static_cast<u32>(TicPixelFormat(tic));
            }
        }
        std::memcpy(k.attrib, cs.attrib, sizeof(k.attrib));
        std::memcpy(k.stride, cs.stride, sizeof(k.stride));
        k.topology = topology;
        k.poly = cs.poly_flags;
        k.ds = cs.ds_flags;
        k.alpha_func = cs.color_state >> 24;
        k.chan_mask = cs.chan_mask;
        k.blend_enable = (cs.color_state >> 8) & 0xff;
        for (u32 i = 0; i < 8; ++i) {
            k.blend[i][0] = cs.blend[i].src_c;
            k.blend[i][1] = cs.blend[i].dst_c;
            k.blend[i][2] = cs.blend[i].src_a;
            k.blend[i][3] = cs.blend[i].dst_a;
        }
        k.rt_count = cs.rt_count;
        for (u32 i = 0; i < cs.rt_count; ++i) {
            const VkFormat f = RtKeyFormat(mem, cs.rt_tex[i], false, peek);
            if (f == VK_FORMAT_UNDEFINED) {
                ok = false;
                return k;
            }
            k.rt_format[i] = f;
        }
        if (cs.depth_tex) {
            const VkFormat f = RtKeyFormat(mem, cs.depth_tex, true, peek);
            if (f == VK_FORMAT_UNDEFINED) {
                ok = false;
                return k;
            }
            k.depth_format = f;
        }
        k.restart = cs.prim_restart;
        return k;
    }

    /// What a pipeline key asks of the shaders: the program key and the vertex attributes.
    struct PipeInputs {
        ShaderKey sk{};
        bool attrib_enabled[16]{};
        std::vector<VkVertexInputAttributeDescription> attributes;
        std::vector<std::string> notes;
    };
    void PipelineInputs(const PipelineKey& key, PipeInputs& in) const {
        in.sk.prog[0] = key.prog[0];
        in.sk.prog[1] = key.prog[1];
        std::memcpy(in.sk.tex_sig, key.tex_sig, sizeof(in.sk.tex_sig));
        in.sk.alpha_func = key.alpha_func;
        for (u32 i = 0; i < 16; ++i) {
            const u32 o = key.attrib[i];
            if (o == 0) continue;
            bool ok = false;
            u32 type = 0;
            const VkFormat f = AttribFormat(device, o >> 20, ok, type);
            if (!ok) {
                in.notes.push_back(fmt::format("vertex attribute format code {:x} unknown", o >> 20));
            } else if (u32 lo = 0; NR::LearnedAttrib(o >> 20, lo)) {
                in.notes.push_back(fmt::format("  attribute code {:x} learned from the driver: maxwell {:x}", o >> 20, lo));
            }
            in.attrib_enabled[i] = true;
            in.sk.attrib_types[i] = type;
            in.attributes.push_back({i, o & 0x1f, f, (o >> 7) & 0x1fff});
        }
    }

    /// The Vulkan pipeline for `key` from `prog`. Used on the guest thread (a miss during the replay) and
    /// on pool threads (prescan): it reads only immutable renderer state; failures throw.
    /// hud_class != 0: nvn_fsr HUD layer variant of the same pipeline (FsrHudBlendClass; attachment 0 only): the colour
    /// blend stays the game's, alpha accumulates coverage (1 over: A' = a + A(1 - a); 2 additive: A unchanged; 3 opaque /
    /// replace: alpha forced to 1 with alphaToOne; 4 opaque / replace without alphaToOne: colour as the game's, then the
    /// same draw with the 5 variant: alpha-only write, logic op SET = 1), alpha always written.
    /// vs_override: another vertex shader module for the same program (nvn_fsr_objvel transform feedback capture).
    NVN_VK_NOINLINE vk::Pipeline CreatePipelineObject(const PipelineKey& key, const Program& prog,
                                                       std::vector<VkVertexInputAttributeDescription> attributes, VkRenderPass rp,
                                                       u32 hud_class = 0, VkShaderModule vs_override = VK_NULL_HANDLE,
                                                       bool raster_discard = false) {
        std::erase_if(attributes, [&](const auto& a) { return !(prog.vs_in_mask & (1u << a.location)); });
        std::vector<VkVertexInputBindingDescription> bindings;
        for (u32 b = 0; b < 4; ++b) {
            bool used = false;
            for (const auto& a : attributes) used = used || a.binding == b;
            if (used) bindings.push_back({b, key.stride[b], VK_VERTEX_INPUT_RATE_VERTEX});
        }
        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = static_cast<u32>(bindings.size()),
            .pVertexBindingDescriptions = bindings.data(),
            .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
            .pVertexAttributeDescriptions = attributes.data(),
        };
        PHASE("pipeline step", 0);
        const bool strip = key.topology == 3 || key.topology == 5 || key.topology == 2 || key.topology == 6;
        const VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
            MaxwellToVK::PrimitiveTopology(device, static_cast<Maxwell::PrimitiveTopology>(key.topology)),
            static_cast<VkBool32>(strip && key.restart)};
        const VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
        PHASE("pipeline step", 0);
        const int dbg = Core::GameSettings::NvnVkDebug();
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.depthClampEnable = (dbg & 64) ? VK_TRUE : VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = ((key.poly & 2) && !(dbg & 16)) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        const bool ccw = (key.poly & 4) != 0;
        rs.frontFace = (ccw != flip_front) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        rs.depthBiasEnable = ((key.poly & 0x20000) && !(dbg & 32)) ? VK_TRUE : VK_FALSE;
        rs.lineWidth = 1.0f;
        rs.rasterizerDiscardEnable = raster_discard ? VK_TRUE : VK_FALSE; // nvn_fsr_objvel capture-only draws
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        const u32 f = key.ds;
        if (key.depth_format) {
            ds.depthTestEnable = ((f & 1) && !(dbg & 8)) ? VK_TRUE : VK_FALSE;
            ds.depthWriteEnable = (f & 2) ? VK_TRUE : VK_FALSE;
            const u32 fn = (f >> 4) & 0xf;
            ds.depthCompareOp = (fn >= 1 && fn <= 8) ? static_cast<VkCompareOp>(fn - 1) : VK_COMPARE_OP_LESS_OR_EQUAL;
        }
        PHASE("pipeline step", 0);
        std::vector<VkPipelineColorBlendAttachmentState> ba(std::max(1u, key.rt_count));
        for (u32 i = 0; i < ba.size(); ++i) {
            ba[i] = {};
            u32 mask = 0;
            for (u32 c = 0; c < 4; ++c) {
                if ((key.chan_mask >> (c * 8 + i)) & 1) mask |= 1u << c;
            }
            ba[i].colorWriteMask = (dbg & 2) ? 0xf : mask;
            ba[i].blendEnable = (((key.blend_enable >> i) & 1) && !(dbg & 1)) ? VK_TRUE : VK_FALSE;
            ba[i].srcColorBlendFactor = Factor(key.blend[i][0]);
            ba[i].dstColorBlendFactor = Factor(key.blend[i][1]);
            ba[i].colorBlendOp = VK_BLEND_OP_ADD;
            ba[i].srcAlphaBlendFactor = Factor(key.blend[i][2]);
            ba[i].dstAlphaBlendFactor = Factor(key.blend[i][3]);
            ba[i].alphaBlendOp = VK_BLEND_OP_ADD;
        }
        if (hud_class != 0) {
            auto& b = ba[0];
            // a draw that writes no colour channel leaves H's picture alone: it must not touch the layer's coverage either
            b.colorWriteMask = (b.colorWriteMask & 0x7) ? (b.colorWriteMask | VK_COLOR_COMPONENT_A_BIT) : 0;
            if (hud_class == 1) {
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            } else if (hud_class == 2) {
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            } else if (hud_class == 3) {
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // alphaToOne makes the source alpha 1
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                ms.alphaToOneEnable = VK_TRUE;
            } else if (hud_class == 6) {
                // multiply into the layer: colour as the game blends it, the layer's coverage unchanged
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            } else if (hud_class == 4) {
                // opaque / replace without alphaToOne (AMD): colour as the game blends it, alpha = the shader's for
                // now; the same draw follows with the class 5 variant, which sets the layer's alpha to 1 where it ran
                b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            } else {
                // class 5: alpha-only pass, logic op SET (all bits 1 = alpha 1.0 in the UNORM layer), colour untouched
                b.colorWriteMask = (b.colorWriteMask & 0x7) ? VK_COLOR_COMPONENT_A_BIT : 0;
                b.blendEnable = VK_FALSE;
            }
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = key.rt_count;
        cb.pAttachments = ba.data();
        if (hud_class == 5) {
            cb.logicOpEnable = VK_TRUE;
            cb.logicOp = VK_LOGIC_OP_SET;
        }
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                      VK_DYNAMIC_STATE_DEPTH_BIAS};
        const VkPipelineDynamicStateCreateInfo dyn_ci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 4, dyn};
        PHASE("pipeline step", 0);
        const VkPipelineShaderStageCreateInfo stages[] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
             vs_override != VK_NULL_HANDLE ? vs_override : *prog.vs, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, *prog.fs, "main", nullptr},
        };
        const VkGraphicsPipelineCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &ia,
            .pTessellationState = nullptr,
            .pViewportState = &vp,
            .pRasterizationState = &rs,
            .pMultisampleState = &ms,
            .pDepthStencilState = &ds,
            .pColorBlendState = &cb,
            .pDynamicState = &dyn_ci,
            .layout = *prog.layout,
            .renderPass = rp,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        PHASE("pipeline step", 0);
        PHASE("pipeline step", 0);
        return dev.CreateGraphicsPipeline(ci, *pipeline_cache);
    }

    Pipeline* GetPipeline(Core::Memory::Memory& mem, const PipelineKey& key, const std::array<u32, 8> (&handles)[2]) {
        if (const auto it = pipelines.find(key); it != pipelines.end()) return it->second.get();
        auto pl = std::make_shared<Pipeline>();
        const auto t0 = std::chrono::steady_clock::now();
        try {
            auto in = std::make_unique<PipeInputs>(); // heap: the guest fiber stack is small
            PipelineInputs(key, *in);
            for (const auto& n : in->notes) ++stats.notes[n];
            PHASE("pipeline step", 0);
            Program* prog = GetProgram(mem, key.prog[0], key.prog[1], st.tex_pool, in->sk, handles, in->attrib_enabled);
            if (!prog || prog->failed) {
                pl->failed = true;
                ++stats.pipeline_fail;
                auto* raw = pl.get();
                pipelines[key] = std::move(pl);
                return raw;
            }
            const u64 ck = prog->content_key ? PipelineContentKeyNow(prog->content_key, key, in->attributes) : 0;
            if (ck) {
                if (auto w = WarmPipelineFor(ck)) {
                    auto* raw = w.get();
                    pipelines[key] = std::move(w);
                    return raw;
                }
            }
            pl->program = prog;
            const VkRenderPass rp = RenderPass(key);
            {
                ProfScope ps{prof, PB_Pipeline};
                pl->pipeline = CreatePipelineObject(key, *prog, in->attributes, rp);
            }
            prof.Count(PC_Pipelines);
            prof.Count(PC_LatePipelines);
            prof.Count(PC_ColdBuilds);
            ++stats.pipeline_ok;
            if (ck) WritePipelineRecord(ck, prog->content_key, key, in->attributes);
        } catch (const std::exception& e) {
            pl->failed = true;
            ++stats.pipeline_fail;
            ++stats.notes[fmt::format("pipeline failure: {}", e.what())];
            Fatal(fmt::format("pipeline failure: {}", e.what()));
        }
        stats.pipeline_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        auto* raw = pl.get();
        pipelines[key] = std::move(pl);
        return raw;
    }

    // ---- prebuilding programs and pipelines on the worker pool ---------------------------------------
    struct ProgramJob {
        ShaderKey sk{};
        u64 vs{}, fs{};
        u32 tex_pool{};
        std::array<u32, 8> handles[2]{};
        bool attrib_enabled[16]{};
        std::unique_ptr<ProgramRecord> rec = std::make_unique<ProgramRecord>();
        std::unique_ptr<Program> result;
        std::vector<std::string> notes;
        std::string error;
        double ms{};
    };
    struct PipelineJob {
        PipelineKey key{};
        PipeInputs in;
        Program* prog{};
        VkRenderPass rp{};
        vk::Pipeline result;
        std::string error;
        double ms{};
    };

    void RunProgramJob(Core::Memory::Memory& mem, ProgramJob& job) {
        struct Args {
            Renderer* self;
            Core::Memory::Memory* mem;
            ProgramJob* job;
        } args{this, &mem, &job};
        const auto t0 = std::chrono::steady_clock::now();
        if (!SehRun(
                [](void* a) {
                    auto* x = static_cast<Args*>(a);
                    try {
                        x->job->result = x->self->BuildProgram(x->mem, x->job->vs, x->job->fs, x->job->tex_pool, x->job->sk, x->job->handles,
                                                               x->job->attrib_enabled, x->job->notes, x->job->rec.get(), nullptr);
                    } catch (const std::exception& e) {
                        x->job->error = e.what();
                    }
                },
                &args)) {
            job.error = fmt::format("access violation in phase '{}'", g_phase);
        }
        if (job.error.empty() && !job.result) job.error = "no program built";
        job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void RunPipelineJob(PipelineJob& job) {
        struct Args {
            Renderer* self;
            PipelineJob* job;
        } args{this, &job};
        const auto t0 = std::chrono::steady_clock::now();
        if (!SehRun(
                [](void* a) {
                    auto* x = static_cast<Args*>(a);
                    try {
                        x->job->result = x->self->CreatePipelineObject(x->job->key, *x->job->prog, x->job->in.attributes, x->job->rp);
                    } catch (const std::exception& e) {
                        x->job->error = e.what();
                    }
                },
                &args)) {
            job.error = fmt::format("access violation in phase '{}'", g_phase);
        }
        job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    /// Before the replay: walk the lists with a copy of the state, collect every program and pipeline the
    /// draws will ask for that is not cached yet, take what the shader cache prewarmed, and build the rest
    /// on the worker pool. The replay then finds everything in the caches. The objects are the ones the
    /// replay would have built itself (same keys, same inputs, first use in list order), so the output does
    /// not change. The walk creates no Vulkan resources, records nothing and leaves `st` alone.
    /// Scans lists[li0] from record ri0 on (and every later list), starting from the live `st`. The eager prescan runs
    /// before BeginFrame from (0, 0); the lazy one ([Debug] nvn_prescan_lazy) from the first draw whose pipeline is missing.
    NVN_VK_NOINLINE void PrebuildForSubmit(Core::Memory::Memory& mem, const std::vector<std::unique_ptr<NR::List>>& lists, size_t li0 = 0,
                                           size_t ri0 = 0) {
        PHASE("prescan", 0);
        const u64 t_scan = ProfNow();
        auto s = std::make_unique<State>(st); // heap: the guest fiber stack is small
        std::vector<std::unique_ptr<ProgramJob>> pjobs;
        std::vector<std::unique_ptr<PipelineJob>> qjobs;
        std::unordered_set<ShaderKey, ShaderKeyHash> want_prog;
        std::unordered_set<PipelineKey, PipelineKeyHash> want_pipe;
        u64 warm_ticks = 0;
        for (size_t li = li0; li < lists.size(); ++li) {
            const auto& recs = lists[li]->recs;
            for (size_t ri = li == li0 ? ri0 : 0; ri < recs.size(); ++ri) {
                const auto& rec = recs[ri];
                if (rec.op != NR::Op::DrawArrays && rec.op != NR::Op::DrawElements) {
                    ApplyState(*s, rec);
                    continue;
                }
                // Same conditions as DoDraw before it asks for the pipeline.
                if (s->prog[0] == 0 || s->prog[1] == 0 || (s->rt_count == 0 && s->depth_tex == 0)) continue;
                std::array<u32, 8> handles[2]{};
                for (u32 a = 0; a < 2; ++a) {
                    for (u32 i = 0; i < 8; ++i) handles[a][i] = s->tex_handle[a][i];
                }
                bool ok = false;
                const PipelineKey key = MakeKey(mem, *s, rec.r[1], handles, ok, true);
                if (!ok || pipelines.contains(key) || !want_pipe.insert(key).second) continue;
                auto q = std::make_unique<PipelineJob>();
                q->key = key;
                PipelineInputs(key, q->in);
                // The shader cache first (the replay would take these entries too).
                const u64 tw = ProfNow();
                auto pit = programs.find(q->in.sk);
                if (pit == programs.end() && !want_prog.contains(q->in.sk)) {
                    if (auto w = WarmProgramFor(mem, key.prog[0], key.prog[1], s->tex_pool, q->in.sk, handles, q->in.attrib_enabled)) {
                        pit = programs.emplace(q->in.sk, std::move(w)).first;
                    }
                }
                if (pit != programs.end() && pit->second->content_key && !pit->second->failed) {
                    if (auto w = WarmPipelineFor(PipelineContentKeyNow(pit->second->content_key, key, q->in.attributes))) {
                        pipelines.emplace(key, std::move(w));
                        warm_ticks += ProfNow() - tw;
                        continue;
                    }
                }
                warm_ticks += ProfNow() - tw;
                if (pit == programs.end() && want_prog.insert(q->in.sk).second) {
                    auto p = std::make_unique<ProgramJob>();
                    p->sk = q->in.sk;
                    p->vs = key.prog[0];
                    p->fs = key.prog[1];
                    p->tex_pool = s->tex_pool;
                    for (u32 a = 0; a < 2; ++a) p->handles[a] = handles[a];
                    std::memcpy(p->attrib_enabled, q->in.attrib_enabled, sizeof(p->attrib_enabled));
                    pjobs.push_back(std::move(p));
                }
                qjobs.push_back(std::move(q));
            }
        }
        // Cache lookups (and any wait for an unfinished prewarm entry, which is also in the prewarm bucket)
        // are not part of the scan time.
        prof.Add(PB_Prescan, ProfNow() - t_scan - warm_ticks);
        if (qjobs.empty()) return;

        // 1. Programs, in parallel.
        if (!pjobs.empty()) {
            PHASE("prebuild programs", pjobs.size());
            std::vector<std::function<void()>> fns;
            for (auto& p : pjobs) {
                ProgramJob* job = p.get();
                fns.push_back([this, job, &mem] { RunProgramJob(mem, *job); });
            }
            const u64 t0 = ProfNow();
            workers->Run(fns);
            prof.Add(PB_Translate, ProfNow() - t0);
            for (auto& p : pjobs) {
                if (!p->error.empty()) {
                    ++stats.shader_fail;
                    ++stats.notes[fmt::format("shader failure: {}", p->error)];
                    Fatal(fmt::format("shader failure on a worker: {} (programs {:x} {:x})", p->error, p->vs, p->fs));
                }
                for (const auto& n : p->notes) ++stats.notes[n];
                ++stats.shader_ok;
                stats.translate_ms += p->ms;
                prof.Count(PC_Programs);
                prof.Count(PC_ColdBuilds);
                p->result->content_key = FinishProgramRecord(mem, *p->rec, p->tex_pool, p->handles, p->sk, p->attrib_enabled);
                NoteRebuilt(mem, p->vs, p->fs, *p->rec);
                WriteProgramRecord(*p->rec);
                programs[p->sk] = std::move(p->result);
                AddRunProgram(std::move(p->rec), programs[p->sk]);
            }
        }

        // 2. Pipelines, in parallel. A program that cannot be used is left to the replay, which records the
        // failed pipeline exactly as before.
        PHASE("prebuild pipelines", qjobs.size());
        std::vector<std::function<void()>> fns;
        for (auto& q : qjobs) {
            const auto it = programs.find(q->in.sk);
            if (it == programs.end() || !it->second) {
                Fatal(fmt::format("prebuild: program {:x} {:x} missing after the translate batch", q->key.prog[0], q->key.prog[1]));
            }
            if (it->second->failed) continue;
            q->prog = it->second.get();
            q->rp = RenderPass(q->key);
            PipelineJob* job = q.get();
            fns.push_back([this, job] { RunPipelineJob(*job); });
        }
        const u64 t1 = ProfNow();
        workers->Run(fns);
        prof.Add(PB_Pipeline, ProfNow() - t1);
        for (auto& q : qjobs) {
            if (!q->prog) continue;
            if (!q->error.empty()) {
                ++stats.pipeline_fail;
                ++stats.notes[fmt::format("pipeline failure: {}", q->error)];
                Fatal(fmt::format("pipeline failure on a worker: {} (programs {:x} {:x})", q->error, q->key.prog[0], q->key.prog[1]));
            }
            for (const auto& n : q->in.notes) ++stats.notes[n];
            auto pl = std::make_shared<Pipeline>();
            pl->pipeline = std::move(q->result);
            pl->program = q->prog;
            ++stats.pipeline_ok;
            stats.pipeline_ms += q->ms;
            prof.Count(PC_Pipelines);
            prof.Count(PC_ColdBuilds);
            if (q->prog->content_key) {
                WritePipelineRecord(PipelineContentKeyNow(q->prog->content_key, q->key, q->in.attributes), q->prog->content_key, q->key,
                                    q->in.attributes);
            }
            pipelines[q->key] = std::move(pl);
        }
    }

    // ---- shader cache file and prewarm -------------------------------------------------------------------
    struct WarmProgramEntry {
        const ProgramRecord* rec{};
        std::shared_ptr<Program> prog;
        std::vector<std::string> notes;
        std::string error;
        bool done{};
        bool notes_merged{};
        bool from_run{}; // built in this run, not prewarmed from the file
    };
    struct WarmPipelineEntry {
        const PipelineRecord* rec{};
        VkRenderPass rp{};
        Program* prog{};
        std::shared_ptr<Pipeline> pipe;
        std::string error;
        bool done{};
    };

    struct ByteWriter {
        std::string b;
        template <typename T>
        void Put(const T& v) {
            b.append(reinterpret_cast<const char*>(&v), sizeof(v));
        }
        void Bytes(const void* p, size_t n) {
            b.append(static_cast<const char*>(p), n);
        }
    };
    struct ByteReader {
        const u8* p;
        const u8* end;
        void Bytes(void* d, size_t n) {
            if (static_cast<size_t>(end - p) < n) throw std::runtime_error("record shorter than its contents");
            std::memcpy(d, p, n);
            p += n;
        }
        template <typename T>
        T Get() {
            T v;
            Bytes(&v, sizeof(v));
            return v;
        }
        u32 Count(size_t item_size) {
            const u32 n = Get<u32>();
            if (static_cast<size_t>(end - p) / item_size < n) throw std::runtime_error("record count past its end");
            return n;
        }
    };
    static void PutStage(ByteWriter& w, const StageTranscript& t) {
        w.Put(t.stage);
        w.Put(t.start_address);
        w.Bytes(t.sph.data(), t.sph.size());
        w.Put(static_cast<u32>(t.instr.size()));
        for (const auto& [a, v] : t.instr) {
            w.Put(a);
            w.Put(v);
        }
        w.Put(static_cast<u32>(t.cbuf.size()));
        for (const auto& [k, v] : t.cbuf) {
            w.Put(k.first);
            w.Put(k.second);
            w.Put(v);
        }
        for (const auto* m : {&t.tex_type, &t.tex_format}) {
            w.Put(static_cast<u32>(m->size()));
            for (const auto& [h, v] : *m) {
                w.Put(h);
                w.Put(v);
            }
        }
    }
    static void GetStage(ByteReader& r, StageTranscript& t) {
        t.stage = r.Get<u32>();
        t.start_address = r.Get<u32>();
        r.Bytes(t.sph.data(), t.sph.size());
        const u32 ni = r.Count(12);
        t.instr.reserve(ni);
        for (u32 i = 0; i < ni; ++i) {
            const u32 a = r.Get<u32>();
            const u64 v = r.Get<u64>();
            if (!t.instr.empty() && t.instr.back().first >= a) throw std::runtime_error("instruction addresses out of order");
            t.instr.emplace_back(a, v);
        }
        const u32 nc = r.Count(12);
        for (u32 i = 0; i < nc; ++i) {
            const u32 idx = r.Get<u32>();
            const u32 off = r.Get<u32>();
            t.cbuf[{idx, off}] = r.Get<u32>();
        }
        for (auto* m : {&t.tex_type, &t.tex_format}) {
            const u32 n = r.Count(8);
            for (u32 i = 0; i < n; ++i) {
                const u32 h = r.Get<u32>();
                (*m)[h] = r.Get<u32>();
            }
        }
        FinishTranscript(t);
    }

    static std::filesystem::path ShaderCachePath() {
        // Shaders are translated with the rescaling pass baked in, so each scale has its own cache.
        RenderScaleFactor(); // settles the (capped) scale before anything is translated
        const auto& res = Settings::values.resolution_info;
        const std::string name = res.active ? fmt::format("nvn_vk_shaders_s{}_{}.bin", res.up_scale, res.down_shift)
                                            : std::string{"nvn_vk_shaders.bin"};
        return Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name;
    }

    void WriteRecord(u32 type, const std::string& payload) {
        u64 sum = 1469598103934665603ull;
        Fnv(sum, payload.data(), payload.size());
        const u32 size = static_cast<u32>(payload.size());
        // Header and payload in one write: a crash mid-record leaves at most a cut-off last record (LoadShaderCache drops it).
        std::string rec(16 + payload.size(), char{0});
        std::memcpy(rec.data(), &type, 4);
        std::memcpy(rec.data() + 4, &size, 4);
        std::memcpy(rec.data() + 8, &sum, 8);
        std::memcpy(rec.data() + 16, payload.data(), payload.size());
        shader_out.write(rec.data(), static_cast<std::streamsize>(rec.size()));
        shader_out.flush();
        if (!shader_out) Fatal("shader cache: cannot write " + ShaderCachePath().string());
    }

    void WriteProgramRecord(const ProgramRecord& r) {
        if (r.key == 0) {
            ++stats.notes["shader cache: program not recordable (texture query outside the bound slots)"];
            return;
        }
        if (!recorded_programs.insert(r.key).second) return;
        ByteWriter w;
        w.Put(r.key);
        w.Bytes(r.attrib_types, sizeof(r.attrib_types));
        w.Put(r.alpha_func);
        w.Bytes(r.attrib_enabled, sizeof(r.attrib_enabled));
        w.Put(r.dbg);
        w.Bytes(r.slots, sizeof(r.slots));
        PutStage(w, r.stage[0]);
        PutStage(w, r.stage[1]);
        WriteRecord(1, w.b);
        ++shader_written[0];
    }

    void WritePipelineRecord(u64 key, u64 program, const PipelineKey& pk, const std::vector<VkVertexInputAttributeDescription>& attributes) {
        if (!recorded_pipelines.insert(key).second) return;
        PipelineKey z = pk;
        std::memset(z.prog, 0, sizeof(z.prog));
        std::memset(z.tex_sig, 0, sizeof(z.tex_sig));
        ByteWriter w;
        w.Put(key);
        w.Put(program);
        w.Bytes(&z, sizeof(z));
        w.Put(static_cast<u32>(attributes.size()));
        if (!attributes.empty()) w.Bytes(attributes.data(), attributes.size() * sizeof(VkVertexInputAttributeDescription));
        w.Put(static_cast<u8>(flip_front ? 1 : 0));
        w.Put(static_cast<u32>(Core::GameSettings::NvnVkDebug()));
        WriteRecord(2, w.b);
        ++shader_written[1];
    }

    /// Reads user/nvn_vk_shaders.bin (any damage is a loud stop) and opens it for appending.
    void LoadShaderCache() {
        const auto path = ShaderCachePath();
        std::error_code ec;
        bool exists = std::filesystem::exists(path, ec);
        if (exists) {
            // A cache from another format/key size is stale, not damaged: set it aside and start fresh.
            u8 head[16] = {};
            {
                std::ifstream hf(path, std::ios::binary);
                hf.read(reinterpret_cast<char*>(head), sizeof(head));
            }
            u32 hv = 0, hk = 0;
            std::memcpy(&hv, head + 8, 4);
            std::memcpy(&hk, head + 12, 4);
            if (std::memcmp(head, kShaderCacheMagic, 8) == 0 && (hv != kShaderCacheVersion || hk != sizeof(PipelineKey))) {
                auto bak = path;
                bak += fmt::format(".v{}.bak", hv);
                std::filesystem::remove(bak, ec);
                std::filesystem::rename(path, bak, ec);
                if (ec) Fatal("shader cache: stale format " + std::to_string(hv) + ", cannot move " + path.string() + " aside");
                Note(fmt::format("shader cache: format {} is stale, moved to {}; starting a fresh cache", hv, bak.string()));
                exists = false;
            }
        }
        if (exists) {
            std::ifstream f(path, std::ios::binary);
            if (!f) Fatal("shader cache: cannot open " + path.string());
            std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (f.bad()) Fatal("shader cache: cannot read " + path.string());
            f.close(); // the file may be truncated below (damaged last record)
            const auto corrupt = [&](const std::string& why) {
                Fatal(fmt::format("shader cache file is damaged ({}). Delete {} (it is rebuilt while playing).", why, path.string()));
            };
            if (data.size() < 16 || std::memcmp(data.data(), kShaderCacheMagic, 8) != 0) corrupt("bad header");
            u32 version = 0, pk_size = 0;
            std::memcpy(&version, data.data() + 8, 4);
            std::memcpy(&pk_size, data.data() + 12, 4);
            if (version != kShaderCacheVersion || pk_size != sizeof(PipelineKey)) {
                Fatal(fmt::format("shader cache file has format {} / key size {}, this build uses {} / {}. Delete {} (it is rebuilt while playing).",
                                  version, pk_size, kShaderCacheVersion, sizeof(PipelineKey), path.string()));
            }
            size_t pos = 16;
            // A damaged LAST record (cut off by a crash or kill while it was written) is dropped: the file is truncated to
            // the last good record and the game starts (the record is rebuilt while playing). Damage before the last
            // record is still a loud stop.
            const auto drop_tail = [&](size_t rec_start, const std::string& why) {
                std::error_code tec;
                std::filesystem::resize_file(path, rec_start, tec);
                if (tec) corrupt(why + ", and truncating the file failed: " + tec.message());
                Note(fmt::format("shader cache: {}: the last record was incomplete and is dropped (file cut to {} bytes)", why, rec_start));
            };
            while (pos < data.size()) {
                const size_t rec_start = pos;
                if (data.size() - pos < 16) {
                    drop_tail(rec_start, fmt::format("truncated record header at {}", pos));
                    break;
                }
                u32 type = 0, size = 0;
                u64 sum = 0;
                std::memcpy(&type, data.data() + pos, 4);
                std::memcpy(&size, data.data() + pos + 4, 4);
                std::memcpy(&sum, data.data() + pos + 8, 8);
                pos += 16;
                if (data.size() - pos < size) {
                    drop_tail(rec_start, fmt::format("truncated record at {}", pos));
                    break;
                }
                u64 check = 1469598103934665603ull;
                Fnv(check, data.data() + pos, size);
                if (check != sum) {
                    if (pos + size != data.size()) corrupt(fmt::format("checksum mismatch at {}", pos));
                    drop_tail(rec_start, fmt::format("checksum mismatch at {}", pos));
                    break;
                }
                ByteReader r{data.data() + pos, data.data() + pos + size};
                try {
                    if (type == 1) {
                        auto rec = std::make_unique<ProgramRecord>();
                        rec->key = r.Get<u64>();
                        r.Bytes(rec->attrib_types, sizeof(rec->attrib_types));
                        rec->alpha_func = r.Get<u32>();
                        r.Bytes(rec->attrib_enabled, sizeof(rec->attrib_enabled));
                        rec->dbg = r.Get<u32>();
                        r.Bytes(rec->slots, sizeof(rec->slots));
                        GetStage(r, rec->stage[0]);
                        GetStage(r, rec->stage[1]);
                        const u64 k = ProgramContentKey(rec->stage[0].hash, rec->stage[1].hash, rec->attrib_types, rec->alpha_func,
                                                        rec->attrib_enabled, rec->slots, rec->dbg);
                        if (k != rec->key) throw std::runtime_error("program key does not match its contents");
                        if (recorded_programs.insert(rec->key).second) {
                            loaded_programs.insert(rec->key);
                            program_records.push_back(std::move(rec));
                        }
                    } else if (type == 2) {
                        auto rec = std::make_unique<PipelineRecord>();
                        rec->key = r.Get<u64>();
                        rec->program = r.Get<u64>();
                        r.Bytes(&rec->pk, sizeof(rec->pk));
                        const u32 n = r.Count(sizeof(VkVertexInputAttributeDescription));
                        rec->attributes.resize(n);
                        if (n) r.Bytes(rec->attributes.data(), n * sizeof(VkVertexInputAttributeDescription));
                        rec->flip_front = r.Get<u8>();
                        rec->dbg = r.Get<u32>();
                        if (PipelineContentKey(rec->program, rec->pk, rec->attributes, rec->flip_front, rec->dbg) != rec->key) {
                            throw std::runtime_error("pipeline key does not match its contents");
                        }
                        if (recorded_pipelines.insert(rec->key).second) pipeline_records.push_back(std::move(rec));
                    } else {
                        throw std::runtime_error(fmt::format("unknown record type {}", type));
                    }
                    if (r.p != r.end) throw std::runtime_error("record longer than its contents");
                } catch (const std::exception& e) {
                    corrupt(fmt::format("record at {}: {}", pos, e.what()));
                }
                pos += size;
            }
            for (const auto& p : pipeline_records) {
                if (!recorded_programs.contains(p->program)) corrupt(fmt::format("pipeline {:x} names a program that is not in the file", p->key));
            }
        }
        shader_out.open(path, std::ios::binary | std::ios::app);
        if (!shader_out) Fatal("shader cache: cannot open " + path.string() + " for writing");
        if (!exists) {
            shader_out.write(kShaderCacheMagic, 8);
            shader_out.write(reinterpret_cast<const char*>(&kShaderCacheVersion), 4);
            const u32 pk_size = sizeof(PipelineKey);
            shader_out.write(reinterpret_cast<const char*>(&pk_size), 4);
            shader_out.flush();
            if (!shader_out) Fatal("shader cache: cannot write " + path.string());
        }
        Note(fmt::format("shader cache: {} programs, {} pipelines on file", program_records.size(), pipeline_records.size()));
    }

    /// Starts rebuilding everything on file on the worker pool, programs first (in the order the game first
    /// used them), each program's pipelines as soon as it is ready. Called once at start, before any submit.
    void StartPrewarm() {
        const u32 dbg = static_cast<u32>(Core::GameSettings::NvnVkDebug());
        const u8 flip = flip_front ? 1 : 0;
        u32 skipped = 0;
        std::vector<WarmProgramEntry*> order;
        for (const auto& r : program_records) {
            if (r->dbg != dbg) {
                ++skipped;
                continue;
            }
            auto e = std::make_unique<WarmProgramEntry>();
            e->rec = r.get();
            order.push_back(e.get());
            warm_programs.emplace(r->key, std::move(e));
            for (const auto& t : r->stage) warm_stage_index[t.quick].push_back(&t);
        }
        for (const auto& r : pipeline_records) {
            if (r->dbg != dbg || r->flip_front != flip || !warm_programs.contains(r->program)) {
                ++skipped; // built with other debug flags or options: it could never match
                continue;
            }
            auto e = std::make_unique<WarmPipelineEntry>();
            e->rec = r.get();
            e->rp = RenderPass(r->pk);
            warm_by_program[r->program].push_back(e.get());
            warm_pipelines.emplace(r->key, std::move(e));
        }
        warm_total[0] = warm_programs.size();
        warm_total[1] = warm_pipelines.size();
        warm_left = warm_programs.size() + warm_pipelines.size();
        prewarm_start = ProfNow();
        prof.Line(fmt::format("P prewarm start: programs {} pipelines {} skipped {} (other debug flags/options)", warm_total[0], warm_total[1], skipped));
        if (warm_left == 0) {
            prewarm_end = prewarm_start;
            return;
        }
        for (WarmProgramEntry* e : order) workers->Post([this, e] { RunWarmProgram(*e); });
    }

    void WarmFinished(size_t n) {
        if (warm_left.fetch_sub(n) == n) prewarm_end = ProfNow();
    }

    void RunWarmProgram(WarmProgramEntry& e) {
        struct Args {
            Renderer* self;
            WarmProgramEntry* e;
            std::unique_ptr<Program> p;
            std::vector<std::string> notes;
            std::string error;
        } args{this, &e, {}, {}, {}};
        if (!SehRun(
                [](void* a) {
                    auto* x = static_cast<Args*>(a);
                    try {
                        const ProgramRecord& r = *x->e->rec;
                        ShaderKey sk{};
                        std::memcpy(sk.attrib_types, r.attrib_types, sizeof(sk.attrib_types));
                        sk.alpha_func = r.alpha_func;
                        bool ae[16];
                        for (u32 i = 0; i < 16; ++i) ae[i] = r.attrib_enabled[i] != 0;
                        const std::array<u32, 8> handles[2]{};
                        x->p = x->self->BuildProgram(nullptr, 0, 0, 0, sk, handles, ae, x->notes, nullptr, &r);
                    } catch (const std::exception& ex) {
                        x->error = ex.what();
                    }
                },
                &args)) {
            args.error = fmt::format("access violation in phase '{}'", g_phase);
        }
        if (args.error.empty() && !args.p) args.error = "no program built";
        std::vector<WarmPipelineEntry*> pipes;
        if (const auto it = warm_by_program.find(e.rec->key); it != warm_by_program.end()) pipes = it->second;
        size_t finished = 1;
        {
            std::scoped_lock lk{warm_lock};
            if (args.p) {
                args.p->content_key = e.rec->key;
                e.prog = std::move(args.p);
            }
            e.notes = std::move(args.notes);
            e.error = args.error;
            e.done = true;
            if (!e.error.empty() || e.prog->failed) {
                // Its pipelines cannot be built; anyone asking for them stops loudly.
                for (WarmPipelineEntry* pe : pipes) {
                    pe->error = e.error.empty() ? "its program cannot be used (more than push descriptors)" : "its program failed: " + e.error;
                    pe->done = true;
                    ++finished;
                }
                if (!e.error.empty()) {
                    warm_error = fmt::format("program {:x}: {}", e.rec->key, e.error);
                    warm_failed = true;
                }
                pipes.clear();
            } else {
                for (WarmPipelineEntry* pe : pipes) pe->prog = e.prog.get();
            }
        }
        warm_cv.notify_all();
        for (WarmPipelineEntry* pe : pipes) workers->Post([this, pe] { RunWarmPipeline(*pe); });
        WarmFinished(finished);
    }

    void RunWarmPipeline(WarmPipelineEntry& e) {
        struct Args {
            Renderer* self;
            WarmPipelineEntry* e;
            vk::Pipeline pipeline;
            std::string error;
        } args{this, &e, {}, {}};
        if (!SehRun(
                [](void* a) {
                    auto* x = static_cast<Args*>(a);
                    try {
                        x->pipeline = x->self->CreatePipelineObject(x->e->rec->pk, *x->e->prog, x->e->rec->attributes, x->e->rp);
                    } catch (const std::exception& ex) {
                        x->error = ex.what();
                    }
                },
                &args)) {
            args.error = fmt::format("access violation in phase '{}'", g_phase);
        }
        {
            std::scoped_lock lk{warm_lock};
            if (args.error.empty()) {
                auto pl = std::make_shared<Pipeline>();
                pl->pipeline = std::move(args.pipeline);
                pl->program = e.prog;
                e.pipe = std::move(pl);
            } else {
                e.error = args.error;
                warm_error = fmt::format("pipeline {:x}: {}", e.rec->key, e.error);
                warm_failed = true;
            }
            e.done = true;
        }
        warm_cv.notify_all();
        WarmFinished(1);
    }

    // ---- persistent Vulkan pipeline cache ----------------------------------------------------------------
    static std::filesystem::path PipelineCachePath() {
        return Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_vk_pipelines.bin";
    }

    void LoadPipelineCache() {
        const auto path = PipelineCachePath();
        std::vector<u8> data;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream f(path, std::ios::binary);
            if (!f) Fatal("pipeline cache: cannot open " + path.string());
            data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            if (f.bad()) Fatal("pipeline cache: cannot read " + path.string());
            // VkPipelineCacheHeaderVersionOne: header size, header version, vendor id, device id, cache UUID.
            if (data.size() < 32) Fatal("pipeline cache file is corrupt (shorter than its header). Delete " + path.string());
            u32 hdr[4];
            std::memcpy(hdr, data.data(), sizeof(hdr));
            if (hdr[0] < 32 || hdr[0] > data.size() || hdr[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
                Fatal(fmt::format("pipeline cache file is corrupt (header size {} version {}). Delete {}", hdr[0], hdr[1], path.string()));
            }
            const VkPhysicalDeviceProperties props = device.GetPhysical().GetProperties();
            if (hdr[2] != props.vendorID || hdr[3] != props.deviceID || std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
                // Not corruption: a GPU or driver change. The driver would ignore the data anyway; say so and start over.
                Note(fmt::format("pipeline cache: written by another GPU or driver ({:x}:{:x}), starting a new one", hdr[2], hdr[3]));
                data.clear();
            }
        }
        try {
            pipeline_cache = dev.CreatePipelineCache({
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .initialDataSize = data.size(),
                .pInitialData = data.empty() ? nullptr : data.data(),
            });
        } catch (const std::exception& e) {
            Fatal(fmt::format("pipeline cache: vkCreatePipelineCache failed ({}) for {}", e.what(), path.string()));
        }
        cache_saved_size = data.size();
        Note(fmt::format("pipeline cache: loaded {} bytes", data.size()));
    }

    /// Writes the cache when it grew since the last write (temp file, then replace).
    /// A failure is noted (nvn_vk.txt) and the save is retried next time; the old file stays as it was. It used to stop the
    /// game (Fatal), e.g. when an antivirus held the file.
    void SavePipelineCache() {
        size_t size = 0;
        if (const VkResult r = pipeline_cache.Read(&size, nullptr); r != VK_SUCCESS) {
            Note(fmt::format("pipeline cache: size query failed ({})", static_cast<int>(r)));
            return;
        }
        if (size <= cache_saved_size) return;
        std::vector<u8> data(size);
        if (const VkResult r = pipeline_cache.Read(&size, data.data()); r != VK_SUCCESS) {
            Note(fmt::format("pipeline cache: read failed ({})", static_cast<int>(r)));
            return;
        }
        data.resize(size);
        const auto path = PipelineCachePath();
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            f.close();
            if (!f) {
                Note("pipeline cache: cannot write " + tmp.string());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            Note("pipeline cache: cannot replace " + path.string() + ": " + ec.message());
            return;
        }
        cache_saved_size = size;
    }

    /// The periodic save (vkGetPipelineCacheData on a ~22 MB cache + file write: ~45 ms) on a worker instead of the render
    /// thread. One save at a time. VkPipelineCache is created without EXTERNALLY_SYNCHRONIZED, so reading its data while
    /// pipelines are created is allowed. ~Renderer joins the workers (a running save finishes, a queued one is dropped)
    /// and then saves once more on its own thread, so the file is complete at exit.
    std::atomic<bool> cache_save_busy{false};
    void SavePipelineCacheAsync() {
        if (!workers) {
            SavePipelineCache();
            return;
        }
        if (cache_save_busy.exchange(true)) return;
        workers->Post([this] {
            SavePipelineCache();
            cache_save_busy = false;
        });
    }

    // ---- executing a recorded list --------------------------------------------------------------------
    void SubmitLocked(Core::Memory::Memory& mem, std::vector<std::unique_ptr<NR::List>>&& lists) {
        last_mem = &mem;
        ++stats.submits;
        std::vector<std::pair<u32, u32>> dirty;
        dirty = std::move(job_dirty);
        job_dirty.clear();
        for (const auto& [addr, size] : dirty) {
            RetireIf([&](const Image& i) { return i.cpu < addr + size && addr < i.cpu + i.bytes; });
        }
        if (warm_failed) {
            std::string why;
            {
                std::scoped_lock lk{warm_lock};
                why = warm_error;
            }
            Fatal("shader cache prewarm failed: " + why);
        }
        // [Debug] nvn_prescan_lazy: no walk up front; DoDraw runs it from the first draw whose pipeline is missing.
        const bool lazy = opt_prescan && workers && opt_prescan_lazy;
        if (opt_prescan && workers && !lazy) PrebuildForSubmit(mem, lists);
        lazy_lists = lazy ? &lists : nullptr;
        PHASE("begin frame", stats.submits);
        BeginFrame();
        // The GPU keeps its bindings from one submit to the next, and the game relies on it
        // (textures and uniform buffers it binds once are never rebound), so `st` is not reset here.
        for (size_t li = 0; li < lists.size(); ++li) {
            ++stats.lists;
            const auto& recs = lists[li]->recs;
            for (size_t ri = 0; ri < recs.size(); ++ri) {
                ++stats.recs;
                lazy_li = li;
                lazy_ri = ri;
                Execute(mem, recs[ri]);
            }
        }
        lazy_lists = nullptr;
        PHASE("end frame", stats.submits);
        EndFrame();
        if (Core::GameSettings::FenceHold()) {
            // [Debug] fence_hold: the guest driver's own submit (and so its fence) happens after this hook returns;
            // hold it until our GPU work for this submit has finished.
            const double h0 = NvnVkNowMs();
            {
                ProfScope ps{prof, PB_WaitCapture};
                CheckVk(frames[cur].fence.Wait(), "fence wait");
            }
            frames[cur].pending = false;
            PpLine(fmt::format("H{} t{:.1f} fence_hold waited {:.1f} ms", stats.submits, h0, NvnVkNowMs() - h0));
        }
        if (query_on && qn > 0) {
            {
                ProfScope ps{prof, PB_WaitCapture};
                CheckVk(frames[cur].fence.Wait(), "fence wait");
            }
            std::array<u64, 256> res{};
            dev.GetQueryResults(*qpool, 0, qn, sizeof(res), res.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            for (u32 i = 0; i < qn; ++i) {
                const auto& m = qmeta[i];
                std::string px;
                if (qprobe[i]) {
                    probe_buf.Invalidate();
                    const u8* d = probe_buf.Mapped().data() + i * 64;
                    px = fmt::format(" px {:02x}{:02x}{:02x}{:02x} {:02x}{:02x}{:02x}{:02x}", d[0], d[1], d[2], d[3], d[20], d[21], d[22], d[23]);
                }
                ++stats.notes[fmt::format("  occl s{} [{:03}] prog {:x} {:x} count {} pass {}x{} rt {} samples {}{}", stats.submits, i, m.p0, m.p1, m.count, m.w, m.h, m.rt, res[i], px)];
            }
            query_on = false;
        }
        if (pick_on && pn > 0) {
            {
                ProfScope ps{prof, PB_WaitCapture};
                CheckVk(frames[cur].fence.Wait(), "fence wait");
            }
            pick_buf.Invalidate();
            u32 last = 0xdeadbeef;
            for (u32 i = 0; i < pn; ++i) {
                const u8* reg = pick_buf.Mapped().data() + size_t{i} * 32768;
                u64 hash = 1469598103934665603ull;
                u64 sum[4]{};
                for (u32 k = 0; k < 128 * 64; ++k) {
                    for (u32 c = 0; c < 4; ++c) {
                        hash = (hash ^ reg[k * 4 + c]) * 1099511628211ull;
                        sum[c] += reg[k * 4 + c];
                    }
                }
                const u8* c0 = reg + (size_t{50} * 128 + 150) * 4;
                NvnVkDumpLine("nvn_ours_regions.txt", fmt::format("pn {} hash {:016x} mean {} {} {} {} px250,650 {:02x}{:02x}{:02x}{:02x}  {}", i, hash,
                                                                  sum[0] / 8192, sum[1] / 8192, sum[2] / 8192, sum[3] / 8192, c0[0], c0[1], c0[2], c0[3], pick_meta[i]));
                u32 v;
                std::memcpy(&v, c0, 4);
                if (v != last) {
                    ++stats.notes[fmt::format("  pick[{:03}] pixel(250,650) {:08x} <- {}", i, v, pick_meta[i])];
                    last = v;
                }
            }
            pick_on = false;
        }
        // Keep the texture cache bounded.
        if (textures.size() > 4000) {
            RetireIf([&](const Image& i) { return i.last_use + 600 < serial; });
            std::erase_if(textures, [&](const auto& kv) { return !kv.second; });
        }
    }

    /// Textures leave the cache into the graveyard: earlier frames may still be reading them.
    template <typename Pred>
    void RetireIf(Pred pred) {
        for (auto it = textures.begin(); it != textures.end();) {
            if (it->second && pred(*it->second)) {
                graveyard.push_back({serial, std::move(it->second)});
                it = textures.erase(it);
                tex_bind_cache.clear();
            } else {
                ++it;
            }
        }
    }

    /// The pure state part of a recorded call, shared by the replay and the prescan (no resources, no
    /// counters, no Vulkan). Returns the number of cb1 macro calls for BindProgram, and for
    /// SetRenderTargets 1 when the targets changed; 0 otherwise.
    static u32 ApplyState(State& s, const NR::Rec& r) {
        using NR::Op;
        switch (r.op) {
        case Op::BindProgram: {
            s.prog[0] = r.program[0];
            s.prog[1] = r.program[1];
            s.cb1[0] = s.cb1[1] = 0;
            // Macro calls on method 0xE18 (IncreaseOnce, 5 args): last argument << 8 = a GPU address. Calls in order -> vertex, pixel.
            u32 call = 0;
            for (u32 k = 0; k < r.pblock_n; ++k) {
                const u32 w = r.pblock[k];
                if ((w >> 29) == 5 && (w & 0x1fff) == 0xe18 && ((w >> 16) & 0x1fff) == 5 && k + 5 < r.pblock_n + 0u) {
                    if (call < 2) s.cb1[call] = u64{r.pblock[k + 5]} << 8;
                    ++call;
                    k += 5;
                }
            }
            return call;
        }
        case Op::BindUniform:
            if (r.r[1] < 2 && r.r[2] < 16) {
                s.ub_addr[r.r[1]][r.r[2]] = r.sp[0] | (u64{r.sp[1]} << 32);
                s.ub_size[r.r[1]][r.r[2]] = r.sp[2];
            }
            return 0;
        case Op::BindTexture:
            if (r.r[1] < 2 && r.r[2] < 8) s.tex_handle[r.r[1]][r.r[2]] = r.sp[0];
            return 0;
        case Op::BindVertexBuffer:
            if (r.r[1] < 4) s.vb[r.r[1]] = {r.r[2] | (u64{r.r[3]} << 32), r.sp[0]};
            return 0;
        case Op::BindVertexAttrib:
            if (r.snap_words == 16) std::memcpy(s.attrib, r.snap, 64);
            return 0;
        case Op::BindVertexStream:
            for (u32 i = 0; i < 4 && i < r.r[1] && i < r.snap_words; ++i) s.stride[i] = r.snap[i];
            return 0;
        case Op::BindBlend:
            if (r.snap_words >= 2 && (r.snap[0] & 0xfffffff8u) == 0x110c0000u) {
                auto& b = s.blend[r.snap[0] & 7];
                b.src_c = r.snap[1] & 0xff;
                b.dst_c = (r.snap[1] >> 8) & 0xff;
                b.src_a = (r.snap[1] >> 16) & 0xff;
                b.dst_a = r.snap[1] >> 24;
            }
            return 0;
        case Op::BindColor:
            if (r.snap_words >= 1) s.color_state = r.snap[0];
            return 0;
        case Op::BindChannelMask:
            if (r.snap_words >= 1) s.chan_mask = r.snap[0];
            return 0;
        case Op::BindDepthStencil:
            if (r.snap_words >= 1) s.ds_flags = r.snap[0];
            return 0;
        case Op::BindPolygon:
            if (r.snap_words >= 1) s.poly_flags = r.snap[0];
            return 0;
        case Op::SetDepthClamp:
            s.depth_clamp = r.r[1] != 0;
            return 0;
        case Op::SetViewport:
            s.viewport[0] = static_cast<f32>(r.r[1]);
            s.viewport[1] = static_cast<f32>(r.r[2]);
            s.viewport[2] = static_cast<f32>(r.r[3]);
            s.viewport[3] = static_cast<f32>(r.sp[0]);
            s.viewport_set = true;
            return 0;
        case Op::SetViewports:
            if (r.snap_words == 4) {
                for (u32 i = 0; i < 4; ++i) std::memcpy(&s.viewport[i], &r.snap[i], 4);
                s.viewport_set = true;
            }
            return 0;
        case Op::SetScissor:
            s.scissor[0] = static_cast<s32>(r.r[1]);
            s.scissor[1] = static_cast<s32>(r.r[2]);
            s.scissor[2] = static_cast<s32>(r.r[3]);
            s.scissor[3] = static_cast<s32>(r.sp[0]);
            s.scissor_set = true;
            return 0;
        case Op::SetScissors:
            if (r.snap_words == 4) {
                for (u32 i = 0; i < 4; ++i) s.scissor[i] = static_cast<s32>(r.snap[i]);
                s.scissor_set = true;
            }
            return 0;
        case Op::SetBlendColor:
            if (r.snap_words == 4) for (u32 i = 0; i < 4; ++i) std::memcpy(&s.blend_color[i], &r.snap[i], 4);
            return 0;
        case Op::SetAlphaRef:
            std::memcpy(&s.alpha_ref, &r.s[0], 4);
            return 0;
        case Op::SetPolygonOffsetClamp:
            for (u32 i = 0; i < 3; ++i) std::memcpy(&s.poly_offset[i], &r.s[i], 4);
            return 0;
        case Op::SetPrimitiveRestart:
            s.prim_restart = r.r[1] != 0;
            return 0;
        case Op::SetTexturePool:
            s.tex_pool = r.r[1];
            return 0;
        case Op::SetSamplerPool:
            s.sampler_pool = r.r[1];
            return 0;
        case Op::SetRenderTargets: {
            const u32 n = std::min<u32>(r.r[1], 4);
            if (s.rt_count != n || s.depth_tex != r.snap[4] || std::memcmp(s.rt_tex, r.snap, n * 4) != 0) {
                s.rt_count = n;
                for (u32 i = 0; i < 4; ++i) s.rt_tex[i] = i < n ? r.snap[i] : 0;
                s.depth_tex = r.snap[4];
                // NVN: binding new render targets puts the viewport and scissor back to the full target; a stale one
                // from the previous pass (1920x1080) would cover only the corner of a bigger target (3440x1440)
                s.viewport_set = false;
                s.scissor_set = false;
                return 1;
            }
            return 0;
        }
        default:
            return 0;
        }
    }

    void Execute(Core::Memory::Memory& mem, const NR::Rec& r) {
        using NR::Op;
        PHASE("execute op", static_cast<u32>(r.op));
        switch (r.op) {
        case Op::BindProgram:
            {
                const u32 call = ApplyState(st, r);
                if (NvnVkCaptureActive()) {
                    static std::set<u64> logged;
                    if (logged.insert(r.program[0] ^ (r.program[1] << 1)).second) {
                        std::string o = fmt::format("bindprog vs {:x} fs {:x} obj", r.program[0], r.program[1]);
                        for (u32 k = 0; k < 12; ++k) o += fmt::format(" {:08x}", r.pobj[k]);
                        o += fmt::format("\n  block n={}:", r.pblock_n);
                        for (u32 k = 0; k < r.pblock_n; ++k) o += fmt::format(" {:08x}", r.pblock[k]);
                        o += fmt::format("\n  parsed calls {} cb1 vs {:x} fs {:x}", call, st.cb1[0], st.cb1[1]);
                        NvnVkDumpLine("nvn_ours_bindprog.txt", o);
                    }
                }
            }
            break;
        case Op::BindUniform:
            if (NvnVkCaptureActive()) {
                static u32 seen[2]{};
                if (r.r[1] < 2 && r.r[2] < 32 && !(seen[r.r[1]] & (1u << r.r[2]))) {
                    seen[r.r[1]] |= 1u << r.r[2];
                    NvnVkDumpLine("nvn_ours_bindprog.txt", fmt::format("api BindUniformBuffer stage {} index {} -> hw cbuf {}", r.r[1], r.r[2], r.r[2] + 3));
                }
            }
            ApplyState(st, r);
            break;
        case Op::BindVertexStream:
            ApplyState(st, r);
            {
                static u32 vs_logged = 0;
                if (vs_logged < 14) {
                    ++vs_logged;
                    ++stats.notes[fmt::format("  stream state: count {} words {:x} {:x} {:x} {:x} {:x} {:x} {:x} {:x}", r.r[1], r.snap[0],
                                              r.snap[1], r.snap[2], r.snap[3], r.snap[4], r.snap[5], r.snap[6], r.snap[7])];
                }
            }
            break;
        case Op::SetRenderTargets:
            if (ApplyState(st, r)) pass_dirty = true;
            break;
        case Op::ClearColor:
            DoClearColor(mem, r);
            break;
        case Op::ClearDepthStencil:
            DoClearDepth(mem, r);
            break;
        case Op::DrawArrays:
        case Op::DrawElements:
            DoDraw(mem, r);
            break;
        case Op::Barrier:
            EndPass();
            break;
        default:
            ApplyState(st, r);
            break;
        }
    }

    bool ClearSetup(Core::Memory::Memory& mem) {
        if (st.rt_count == 0 && st.depth_tex == 0) return false;
        PipelineKey k{};
        k.rt_count = st.rt_count;
        for (u32 i = 0; i < st.rt_count; ++i) {
            Image* img = RenderTarget(mem, st.rt_tex[i], false);
            if (!img) return false;
            k.rt_format[i] = img->format;
        }
        if (st.depth_tex) {
            Image* d = RenderTarget(mem, st.depth_tex, true);
            if (!d) return false;
            k.depth_format = d->format;
        }
        return BeginPass(mem, k);
    }

    // A clear covers the scissor box when one is set (the game clears one shadow tile at a time).
    VkClearRect ClearRect() const {
        VkClearRect rect{{{0, 0}, {pass_w, pass_h}}, 0, 1};
        if (st.scissor_set) {
            const s32 x = std::clamp(st.scissor[0], 0, static_cast<s32>(pass_w));
            const s32 y = std::clamp(st.scissor[1], 0, static_cast<s32>(pass_h));
            const s32 w = std::clamp(st.scissor[2], 0, static_cast<s32>(pass_w) - x);
            const s32 h = std::clamp(st.scissor[3], 0, static_cast<s32>(pass_h) - y);
            rect.rect = {{x, y}, {static_cast<u32>(w), static_cast<u32>(h)}};
        }
        // pass_w/h and the scissor are in the game's units; the targets are scaled
        const double sx = pass_w ? static_cast<double>(pass_pw) / pass_w : 1.0;
        const double sy = pass_h ? static_cast<double>(pass_ph) / pass_h : 1.0;
        const s32 x0 = static_cast<s32>(std::floor(rect.rect.offset.x * sx));
        const s32 y0 = static_cast<s32>(std::floor(rect.rect.offset.y * sy));
        const s32 x1 = std::min<s32>(static_cast<s32>(pass_pw), static_cast<s32>(std::ceil((rect.rect.offset.x + rect.rect.extent.width) * sx)));
        const s32 y1 = std::min<s32>(static_cast<s32>(pass_ph), static_cast<s32>(std::ceil((rect.rect.offset.y + rect.rect.extent.height) * sy)));
        rect.rect = {{x0, y0}, {static_cast<u32>(std::max(0, x1 - x0)), static_cast<u32>(std::max(0, y1 - y0))}};
        return rect;
    }

    void DoClearColor(Core::Memory::Memory& mem, const NR::Rec& r) {
        ++stats.clears;
        if (!ClearSetup(mem) || r.r[1] >= st.rt_count || r.snap_words != 4) return;
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = r.r[1];
        Image* ct = RenderTarget(mem, st.rt_tex[r.r[1]], false);
        if (ct) ++ct->clears;
        std::memcpy(ca.clearValue.color.float32, r.snap, 16);
#ifdef SUYU_HAS_FSR_VULKAN
        if (fsr_layer_on && ct && ct == fsr_layer_h) {
            // a clear of H while its draws go to the HUD layer: H would become the clear colour, i.e. an opaque layer texel
            ca.clearValue.color.float32[3] = 1.0f;
            ++fsr_sec_clears;
        }
#endif
        const VkClearRect rect = ClearRect();
        vkcmd.ClearAttachments(ca, rect);
    }

    void DoClearDepth(Core::Memory::Memory& mem, const NR::Rec& r) {
        ++stats.clears;
        if (!ClearSetup(mem) || st.depth_tex == 0) return;
        VkClearAttachment ca{};
        ca.aspectMask = 0;
        if (r.r[1]) ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (r.r[3]) ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (ca.aspectMask == 0) return;
        std::memcpy(&ca.clearValue.depthStencil.depth, &r.s[0], 4);
        ca.clearValue.depthStencil.stencil = r.r[2];
        const VkClearRect rect = ClearRect();
        if (Image* d = RenderTarget(mem, st.depth_tex, true)) {
            ++d->clears;
            if (d->width == 1024)
                ++stats.notes[fmt::format("  shadow clear depth {} sc {} [{} {} {} {}] rect {} {} {} {}", ca.clearValue.depthStencil.depth,
                                          st.scissor_set, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3],
                                          rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height)];
        }
        vkcmd.ClearAttachments(ca, rect);
    }

    void DoDraw(Core::Memory::Memory& mem, const NR::Rec& r) {
        using NR::Op;
        ++stats.draws;
        prof.Count(PC_Draws);
        ++gp_pass_draws;
        ProfScope ps_draw{prof, PB_Draw};
        const bool indexed = r.op == Op::DrawElements;
        const u32 topology = r.r[1];
        if (st.prog[0] == 0 || st.prog[1] == 0 || (st.rt_count == 0 && st.depth_tex == 0)) {
            ++stats.notes[fmt::format("draw skipped: vs {} fs {} rt_count {}", st.prog[0] != 0, st.prog[1] != 0, st.rt_count)];
            return;
        }
        std::array<u32, 8> handles[2]{};
        for (u32 s = 0; s < 2; ++s) {
            for (u32 i = 0; i < 8; ++i) handles[s][i] = st.tex_handle[s][i];
        }
        bool ok = false;
        std::optional<ProfScope> ps_k;
        ps_k.emplace(prof, PB_DKey);
        const PipelineKey key = MakeKey(mem, st, topology, handles, ok, false);
        if (!ok) {
            ++stats.notes["draw skipped: render target unavailable"];
            return;
        }
        if (lazy_lists && !pipelines.contains(key)) {
            // [Debug] nvn_prescan_lazy: first miss of this submit: prebuild for this draw and the rest of the submit
            // (state copy = the live `st` at this draw, i.e. what the up-front walk would have had here).
            const auto* lists = lazy_lists;
            lazy_lists = nullptr;
            PrebuildForSubmit(mem, *lists, lazy_li, lazy_ri);
        }
        PHASE("get pipeline", st.prog[0]);
        Pipeline* pl = GetPipeline(mem, key, handles);
        ps_k.reset();
        if (!pl || pl->failed || !pl->program) {
            ++stats.notes["draw skipped: pipeline failed"];
            return;
        }
        // Textures first: uploads must happen outside a pass.
        struct Bound {
            Image* img{};
            VkSampler sampler{};
        };
        Bound bound[2][8]{};
        std::optional<ProfScope> ps_t;
        ps_t.emplace(prof, PB_DTex);
        for (u32 s = 0; s < 2; ++s) {
            for (u32 i = 0; i < 8; ++i) {
                if (handles[s][i] == 0) continue;
                const std::pair<u64, u32> bkey{(u64{st.tex_pool} << 32) | st.sampler_pool, handles[s][i]};
                if (const auto hit = tex_bind_cache.find(bkey); hit != tex_bind_cache.end()) {
                    bound[s][i].img = hit->second.img;
                    bound[s][i].sampler = hit->second.sampler;
                    if (bound[s][i].img) bound[s][i].img->last_use = serial;
                    continue;
                }
                u32 cpu = 0;
                Tegra::Texture::TICEntry tic{};
                if (!NR::TextureDescriptor(st.tex_pool, handles[s][i] & 0xfffff, cpu)) continue;
                mem.ReadBlock(cpu, &tic, sizeof(tic));
                bound[s][i].img = SampledImage(mem, tic);
                bound[s][i].sampler = Sampler(mem, st.sampler_pool, (handles[s][i] >> 20) & 0xfff);
                tex_bind_cache[bkey] = {bound[s][i].img, bound[s][i].sampler};
            }
        }
        if (NvnVkCaptureActive() && st.prog[1] == 0x40ff1c600ull && cap_tex.empty()) {
            for (u32 i = 0; i < 8; ++i) {
                if (!bound[1][i].img) continue;
                Tegra::Texture::TICEntry ctic{};
                u32 ccpu = 0;
                if (NR::TextureDescriptor(st.tex_pool, handles[1][i] & 0xfffff, ccpu)) mem.ReadBlock(ccpu, &ctic, sizeof(ctic));
                cap_tex.push_back({i, bound[1][i].img, ctic});
            }
        }
        if (!placeholder) MakePlaceholder();
        if (pass_active && (!texture_uploads.empty() || !pending_transitions.empty())) EndPass();
#ifdef SUYU_HAS_FSR_VULKAN
        if (fsr_on) {
            // bound: every handle slot set (bindings persist across draws, so this includes stale ones);
            // used: only the slots this draw's program reads (texture descriptors on constant buffer 2, as below)
            std::array<Image*, 16> fsr_sampled{}, fsr_used{};
            for (u32 ss = 0; ss < 2; ++ss) {
                for (u32 ii = 0; ii < 8; ++ii) fsr_sampled[ss * 8 + ii] = bound[ss][ii].img;
            }
            u32 nu = 0;
            for (u32 ss = 0; ss < 2; ++ss) {
                for (const auto& td : pl->program->info[ss].texture_descriptors) {
                    const u32 slot = td.cbuf_index == 2 && td.cbuf_offset >= 0x20 ? (td.cbuf_offset - 0x20) / 8 : 99;
                    if (slot < 8 && bound[ss][slot].img && nu < fsr_used.size()) fsr_used[nu++] = bound[ss][slot].img;
                }
            }
            const FsrDrawInfo fdi{indexed, topology, r.r[3], key.blend_enable & 1u, st.prog[1], &key};
            FsrOnDraw(mem, fsr_sampled, fsr_used, fdi); // may copy the HUD-free image (outside the pass); marks the final draw
            if (fsr_redirect_now) {
                // HUD layer: the same pipeline with the alpha channel accumulating coverage
                fsr_hud_pipe = FsrHudPipeline(key, *pl, fsr_redirect_class);
                if (fsr_hud_pipe == VK_NULL_HANDLE) {
                    FsrLayerAbort(fmt::format("draw {}: HUD layer pipeline variant could not be built", fsr_draw_idx - 1));
                } else {
                    ++fsr_layer_draws;
                    if (fsr_redirect_class == 4) {
                        fsr_hud_pipe2 = FsrHudAlphaPipeline(key, *pl);
                        if (fsr_hud_pipe2 == VK_NULL_HANDLE) {
                            FsrLayerAbort(fmt::format("draw {}: HUD layer alpha-pass pipeline could not be built", fsr_draw_idx - 1));
                        } else {
                            ++fsr_sec_alpha_pass;
                        }
                    }
                }
            }
            if (!fsr_redirect_now) fsr_hud_pipe2 = VK_NULL_HANDLE;
        }
        fsr_sub_pos.clear();
        fsr_sub_rescale.clear();
#endif
        ps_t.reset();
        std::optional<ProfScope> ps_d;
        ps_d.emplace(prof, PB_DDesc);
        size_t index_at = 0;
        VkIndexType index_type = VK_INDEX_TYPE_UINT16;
        u32 index_count = r.r[3];
        s32 base_vertex = 0;
        u32 vtx_shift = 0;
        if (indexed) {
            const u32 type = r.r[2];
            const u32 bytes = type == 2 ? 4 : 2;
            if (type == 0) {
                ++stats.notes["draw skipped: 8-bit indices"];
                return;
            }
            index_type = type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            up_cat = 1;
            index_at = Upload(mem, r.sp[0] | (u64{r.sp[1]} << 32), index_count * bytes, 4);
            base_vertex = static_cast<s32>(r.sp[2]);
            if (index_at == ~size_t{0}) {
                UploadFailure("draw skipped: index buffer", r.sp[0] | (u64{r.sp[1]} << 32), index_count * bytes);
                return;
            }
        }
        // Only the vertices this draw can reach are copied: [first, first + n) for plain draws, [min, max] of the
        // indices (plus base vertex) for indexed ones. The copy starts at that vertex and the draw's vertex offset
        // is shifted to match, so draws that use different slices of one big buffer no longer copy it whole.
        u32 vb_first = 0, vb_count = 0;
        bool vb_ranged = false;
        bool idx_restart = indexed; // the index data holds the restart index (or was not / could not be read)
        if (indexed && index_count > 0 && base_vertex >= 0 && index_at != ~size_t{0}) {
            const bool wide = index_type == VK_INDEX_TYPE_UINT32;
            const u64 iva = r.sp[0] | (u64{r.sp[1]} << 32);
            u32 lo = ~0u, hi = 0;
            bool restart = false;
            const auto ck = std::make_tuple(iva, index_count, wide);
            if (const auto it = idx_range_cache.find(ck); it != idx_range_cache.end()) {
                lo = it->second.lo; hi = it->second.hi; restart = it->second.restart;
            } else {
                // read the guest copy, not the arena: the arena is write-combined memory and slow to read back
                idx_scratch.resize(size_t{index_count} * (wide ? 4 : 2));
                if (ReadGpu(mem, iva, idx_scratch.data(), static_cast<u32>(idx_scratch.size()))) {
                    const u8* ip = idx_scratch.data();
                    for (u32 k = 0; k < index_count; ++k) {
                        u32 v;
                        if (wide) { std::memcpy(&v, ip + k * 4, 4); restart |= v == 0xffffffffu; }
                        else { u16 w; std::memcpy(&w, ip + k * 2, 2); v = w; restart |= w == 0xffff; }
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                } else {
                    restart = true;
                }
                idx_range_cache[ck] = {lo, hi, restart};
            }
            idx_restart = restart;
            if (!restart) { vb_first = static_cast<u32>(base_vertex) + lo; vb_count = hi - lo + 1; vb_ranged = true; vtx_shift = lo; }
        } else if (!indexed && r.r[3] > 0) {
            vb_first = r.r[2];
            vb_count = r.r[3];
            vb_ranged = true;
        }
        // Vertex / index data.
        std::array<VkBuffer, 4> vbuf{};
        std::array<VkDeviceSize, 4> voff{};
        for (u32 b = 0; b < 4; ++b) {
            vbuf[b] = *frames[cur].arena;
            if (st.vb[b].addr == 0) continue;
            bool used = false;
            for (u32 a = 0; a < 16; ++a) used = used || (st.attrib[a] != 0 && (st.attrib[a] & 0x1f) == b);
            if (!used) continue;
            u32 size = std::min<u32>(st.vb[b].size, 32u << 20);
            u64 vb_va = st.vb[b].addr;
            bool streamed = false;
            if (vb_ranged && st.stride[b] != 0) {
                const u64 begin = u64{vb_first} * st.stride[b];
                const u64 end = std::min<u64>(size, (u64{vb_first} + vb_count) * st.stride[b]);
                if (begin < end) {
                    vb_va += begin;
                    size = static_cast<u32>(end - begin);
                    streamed = true;
                }
            }
            up_cat = 0;
            const size_t at = Upload(mem, vb_va, size, 16);
            if (at == ~size_t{0}) {
                UploadFailure("draw skipped: vertex buffer", st.vb[b].addr, size);
                return;
            }
            voff[b] = at;
            if (vb_ranged && !streamed) voff[b] += u64{vb_first} * st.stride[b];
            static u32 vb_logged = 0;
            if (vb_logged < 30 && stats.submits > 300) {
                ++vb_logged;
                u32 vcpu = 0, vav = 0;
                std::array<u8, 32> vbytes{};
                u32 nz = 0;
                if (NR::GpuToCpu(mem, st.vb[b].addr, vcpu, vav)) {
                    mem.ReadBlock(vcpu, vbytes.data(), 32);
                    for (const u8 v : vbytes) nz += v != 0;
                }
                std::string hex;
                for (u32 i = 0; i < 32; ++i) hex += fmt::format("{:02x}", vbytes[i]);
                ++stats.notes[fmt::format("  vb[{}] va {:x} size {:x} cpu {:x} nonzero {}/32 {} attrib0 {:x} stride {} {} {} {}", b, st.vb[b].addr, size,
                                          vcpu, nz, hex, st.attrib[0], st.stride[0], st.stride[1], st.stride[2], st.stride[3])];
            }
        }
        // Descriptors in the binding order the layout builder used.
        // reused across draws (DoDraw runs on the one render thread): no heap allocation per draw
        static thread_local std::vector<DescriptorUpdateEntry> data, part_cb[2], part_tex[2];
        data.clear();
        for (u32 s = 0; s < 2; ++s) { part_cb[s].clear(); part_tex[s].clear(); }
        VkBuffer arena = *frames[cur].arena;
        for (u32 s = 0; s < 2; ++s) {
            const auto& info = pl->program->info[s];
            for (const auto& cbd : info.constant_buffer_descriptors) {
                for (u32 c = 0; c < cbd.count; ++c) {
                    const u32 idx = cbd.index + c;
                    VkDescriptorBufferInfo bi{*zero_buffer, 0, 4096};
                    const char* bound_as = "ZERO";
                    if (idx == 1 && st.cb1[s] != 0) {
                        const u32 size = std::max<u32>(std::min<u32>(info.constant_buffer_used_sizes[1], 65536), 16);
                        up_cat = 2;
                        const size_t at = Upload(mem, st.cb1[s], size, device.GetUniformBufferAlignment());
                        if (at != ~size_t{0}) {
                            bi = {arena, at, size};
                            bound_as = "CB1-FROM-PROGRAM-BLOCK";
                        } else {
                            bound_as = "CB1-UNREADABLE";
                            UploadFailure("constant buffer 1 bound as zeros", st.cb1[s], size);
                        }
                    }
                    if (idx >= 3 && idx - 3 < 16 && st.ub_addr[s][idx - 3] != 0) bound_as = "UB";
                    if (NvnVkCaptureActive() && idx < 3) {
                        NvnVkDumpLine("nvn_ours_cbinds.txt", fmt::format("vs {:x} fs {:x} stage {} cbuf {} used {:x} bound {} addr {:x}", st.prog[0], st.prog[1], s, idx,
                                                                         info.constant_buffer_used_sizes[idx], bound_as, idx == 1 ? st.cb1[s] : 0));
                    }
                    if (idx >= 3&& idx - 3 < 16 && st.ub_addr[s][idx - 3] != 0) {
                        const u32 want = std::max<u32>(std::min<u32>(info.constant_buffer_used_sizes[idx], 65536), 16);
                        const u32 have = std::max<u32>(st.ub_size[s][idx - 3], 16);
                        const u32 size = std::min(want, have);
                        up_cat = 3;
                        const size_t at = Upload(mem, st.ub_addr[s][idx - 3], size, device.GetUniformBufferAlignment());
                        if (at != ~size_t{0}) {
                            bi = {arena, at, std::max<u32>(size, 16)};
                        } else {
                            UploadFailure("uniform buffer bound as zeros", st.ub_addr[s][idx - 3], size);
                        }
                        static u32 ub_logged = 0;
                        static u32 ub_shadow = 0;
                        if (s == 1 && (st.prog[1] == 0x410220100ull || st.prog[1] == 0x40fadee00ull) && ub_shadow < 45) {
                            ++ub_shadow;
                            u32 ucpu = 0, uav = 0;
                            std::array<u8, 32> ub{};
                            if (NR::GpuToCpu(mem, st.ub_addr[s][idx - 3], ucpu, uav)) mem.ReadBlock(ucpu, ub.data(), 32);
                            std::string hex;
                            for (u32 i = 0; i < 32; ++i) hex += fmt::format("{:02x}", ub[i]);
                            ++stats.notes[fmt::format("  shadow ub cbuf {} prog {:x} va {:x} size {:x} want {:x} {}", idx, st.prog[1], st.ub_addr[s][idx - 3], st.ub_size[s][idx - 3], want, hex)];
                        }
                        if (ub_logged < 40 && stats.submits > 300) {
                            ++ub_logged;
                            u32 ucpu = 0, uav = 0, unz = 0;
                            std::array<u8, 32> ub{};
                            if (NR::GpuToCpu(mem, st.ub_addr[s][idx - 3], ucpu, uav)) {
                                mem.ReadBlock(ucpu, ub.data(), 32);
                                for (const u8 v : ub) unz += v != 0;
                            }
                            std::string hex;
                            for (u32 i = 0; i < 32; ++i) hex += fmt::format("{:02x}", ub[i]);
                            ++stats.notes[fmt::format("  ub stage {} cbuf {} va {:x} size {:x} want {:x} at {:x} cpu {:x} nonzero {}/32 {}", s, idx,
                                                      st.ub_addr[s][idx - 3], st.ub_size[s][idx - 3], want, at, ucpu, unz, hex)];
                        }
                    }
                    DescriptorUpdateEntry e{};
                    e.buffer = bi;
                    part_cb[s].push_back(e);
                }
            }
            if (!info.storage_buffers_descriptors.empty() || !info.texture_buffer_descriptors.empty() ||
                !info.image_buffer_descriptors.empty() || !info.image_descriptors.empty()) {
                ++stats.notes["draw skipped: shader uses storage buffers or images"];
                return;
            }
        }
        // suyu's rescaling: one bit per texture descriptor (stages in order) says "this texture is a scaled
        // render target", so the shaders scale texelFetch/textureSize coordinates for it.
        std::array<u32, Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> rescale_words{};
        u32 rescale_index = 0;
        for (u32 s = 0; s < 2; ++s) {
            const auto& info = pl->program->info[s];
            for (const auto& td : info.texture_descriptors) {
                bool td_scaled = false;
                for (u32 c = 0; c < td.count; ++c) {
                    // handle slot: constant buffer 2 at 0x20 + 8 * slot
                    u32 slot = td.cbuf_index == 2 && td.cbuf_offset >= 0x20 ? (td.cbuf_offset - 0x20) / 8 : 99;
                    Bound b{};
                    if (slot < 8) b = bound[s][slot];
                    DescriptorUpdateEntry e{};
                    td_scaled |= b.img && b.img->pw != 0 && !b.img->out_scaled; // split scale: 1X present images are unscaled
                    if (b.img) {
                        e.image = {b.sampler ? b.sampler : *default_sampler, b.img->sampled_view ? *b.img->sampled_view : *b.img->view, VK_IMAGE_LAYOUT_GENERAL};
                    } else {
                        if (!placeholder) MakePlaceholder();
                        e.image = {*default_sampler, *placeholder->view, VK_IMAGE_LAYOUT_GENERAL};
                        ++stats.notes["texture slot without image (placeholder used)"];
                        const u32 hv = slot < 8 ? handles[s][slot] : 0;
                        u32 dcpu = 0;
                        const bool dok = slot < 8 && hv != 0 && NR::TextureDescriptor(st.tex_pool, hv & 0xfffff, dcpu);
                        ++stats.notes[fmt::format("  placeholder reason: stage {} cbuf {} off {:x} slot {} handle {} desc {}", s,
                                                  td.cbuf_index, td.cbuf_offset, slot, hv ? "set" : "zero", dok)];
                    }
#ifdef SUYU_HAS_FSR_VULKAN
                    if (fsr_sub_now && b.img && b.img == fsr_final_src) {
                        fsr_sub_pos.push_back({s, static_cast<u32>(part_tex[s].size())});
                        // split scale: the composite that replaces H is 1X, so this descriptor is not a scaled texture then
                        if (split_on) fsr_sub_rescale.push_back(td.count == 1 ? rescale_index : ~0u);
                    }
#endif
                    part_tex[s].push_back(e);
                }
                if (td_scaled && Settings::values.resolution_info.active &&
                    rescale_index < Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS * 32) {
                    rescale_words[rescale_index / 32] |= 1u << (rescale_index % 32);
                }
                ++rescale_index;
            }
        }
        for (u32 s = 0; s < 2; ++s) {
            data.insert(data.end(), part_cb[s].begin(), part_cb[s].end());
            data.insert(data.end(), part_tex[s].begin(), part_tex[s].end());
        }
        ps_d.reset();
        std::optional<ProfScope> ps_r;
        ps_r.emplace(prof, PB_DRec);
        if (mv_on) MvBeforeDraw(mem); // may run the motion vector pass (outside the render pass) before the final draw
#ifdef SUYU_HAS_FSR_VULKAN
        // nvn_fsr, the game's final draw: FSR + HUD composite, then this draw samples the composite instead of the
        // game's 1080p image (same UVs; the composite has the present image's size)
        bool fsr_composited = false;
        if (fsr_sub_now && !fsr_sub_pos.empty() && FsrDispatch(mem)) {
            fsr_composited = true;
            for (const auto& [s, i] : fsr_sub_pos) {
                size_t at = part_cb[0].size() + i;
                if (s == 1) at += part_tex[0].size() + part_cb[1].size();
                data[at].image.imageView = *fsr_comp->view;
            }
            // split scale: the composite has the 1X present size, not H's render scale; clear the rescale bit of those
            // descriptors (texelFetch / textureSize of it are then in its own pixels = the game's units at 1X)
            if (SplitNow() && fsr_comp->width != (fsr_final_src->pw ? fsr_final_src->pw : fsr_final_src->width)) {
                for (const u32 rix : fsr_sub_rescale) {
                    if (rix == ~0u) {
                        SplitNote("final draw array", "the final draw samples H through a descriptor array: its rescale bit stays (texel "
                                                      "reads of the composite may be off)");
                    } else if (rix < Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS * 32) {
                        rescale_words[rix / 32] &= ~(1u << (rix % 32));
                    }
                }
            }
        }
        if (fsr_final_now) FsrFinalLayer(fsr_composited); // HUD layer not composited -> merged back into H first
        // nvn_fsr_objvel: is this a dynamic object to capture (transform feedback of its clip positions)?
        objv_cap_now = false;
        if (fsr_on && objv_on) {
            // the per-draw CPU timer feeds the log lines only: debug hooks or gpu_diagnostics
            const bool objv_timed = fsr_dbg || prof.on;
            const double objv_t0 = objv_timed ? NvnVkNowMs() : 0.0;
            ObjvOnDraw(mem, key, *pl, indexed, topology, r.r[3], indexed ? base_vertex : static_cast<s32>(r.r[2]), idx_restart,
                       indexed ? (r.sp[0] | (u64{r.sp[1]} << 32)) : 0, index_type == VK_INDEX_TYPE_UINT32);
            if (objv_timed) objv_sec_cpu_ms += NvnVkNowMs() - objv_t0;
        }
#endif
        PHASE("begin pass", st.rt_tex[0]);
        if (!BeginPassFor(mem, key)) {
            ++stats.notes["draw skipped: render pass unavailable"];
#ifdef SUYU_HAS_FSR_VULKAN
            if (objv_cap_now && !objv_cur.empty()) objv_cur.pop_back(); // nothing was captured for it
            objv_cap_now = false;
#endif
            return;
        }
        PHASE("record draw", st.prog[0]);
        if (Kernel::StallRing::On()) { // [Debug] probes: which viewport/scissor/pass sizes the game draws with (counted per combination)
            // Counted by raw value here; WriteStats formats them into the same "draw vp ..." notes (no string per draw).
            ++vp_probe[{st.viewport_set ? 1u : 0u, std::bit_cast<u32>(st.viewport[0]), std::bit_cast<u32>(st.viewport[1]),
                        std::bit_cast<u32>(st.viewport[2]), std::bit_cast<u32>(st.viewport[3]), st.scissor_set ? 1u : 0u,
                        static_cast<u32>(st.scissor[0]), static_cast<u32>(st.scissor[1]), static_cast<u32>(st.scissor[2]),
                        static_cast<u32>(st.scissor[3]), pass_w, pass_h}];
        }
        if (stats.submits >= 600 && stats.submits < 603 && draw_logged < 60) {
            ++draw_logged;
            ++stats.notes[fmt::format("  draw[{}] topo {} count {} idx {} vp {} [{:.0f} {:.0f} {:.0f} {:.0f}] sc {} [{} {} {} {}] pass {}x{} rt {} depth {:x} ds {:x} poly {:x} color {:x} chan {:x} blend {:x} bl {} {} {} {} prog {:x} {:x}",
                                      draw_logged, topology, r.r[3], indexed, st.viewport_set, st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3],
                                      st.scissor_set, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3], pass_w, pass_h, st.rt_count, st.depth_tex,
                                      st.ds_flags, st.poly_flags, st.color_state, st.chan_mask, key.blend_enable, key.blend[0][0], key.blend[0][1], key.blend[0][2], key.blend[0][3], st.prog[0], st.prog[1])];
        }
#ifdef SUYU_HAS_FSR_VULKAN
        const bool objv_expanded = objv_cap_now && !objv_cur.empty() && objv_cur.back().expanded;
        vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_redirect_now && fsr_hud_pipe ? fsr_hud_pipe
                                                            : objv_cap_now && !objv_expanded ? objv_cap_pipe
                                                                                             : *pl->pipeline);
#else
        vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pl->pipeline);
#endif
        // viewport / scissor
        const f32 vx = st.viewport_set ? st.viewport[0] : 0, vy = st.viewport_set ? st.viewport[1] : 0;
        const f32 vw = st.viewport_set ? st.viewport[2] : static_cast<f32>(pass_w);
        const f32 vh = st.viewport_set ? st.viewport[3] : static_cast<f32>(pass_h);
        const float sx = pass_w ? static_cast<float>(pass_pw) / static_cast<float>(pass_w) : 1.0f;
        const float sy = pass_h ? static_cast<float>(pass_ph) / static_cast<float>(pass_h) : 1.0f;
        VkViewport vp{vx * sx, (vy + vh) * sy, vw * sx, -vh * sy, 0.0f, 1.0f};
        if (no_flip_y) vp = {vx * sx, vy * sy, vw * sx, vh * sy, 0.0f, 1.0f};
        vkcmd.SetViewport(0, vp);
#ifdef SUYU_HAS_FSR_VULKAN
        if (objv_cap_now && !objv_cur.empty()) objv_cur.back().vp = vp; // the velocity pass rasterizes with the same transform
#endif
        VkRect2D sc{{0, 0}, {pass_pw, pass_ph}};
        if (st.scissor_set) {
            const s32 x0 = static_cast<s32>(std::floor(std::max(0, st.scissor[0]) * sx));
            const s32 y0 = static_cast<s32>(std::floor(std::max(0, st.scissor[1]) * sy));
            const s32 x1 = static_cast<s32>(std::ceil((std::max(0, st.scissor[0]) + std::max(0, st.scissor[2])) * sx));
            const s32 y1 = static_cast<s32>(std::ceil((std::max(0, st.scissor[1]) + std::max(0, st.scissor[3])) * sy));
            sc.offset = {x0, y0};
            sc.extent = {static_cast<u32>(std::max(0, x1 - x0)), static_cast<u32>(std::max(0, y1 - y0))};
        }
        vkcmd.SetScissor(0, sc);
        vkcmd.SetBlendConstants(st.blend_color);
        vkcmd.SetDepthBias(st.poly_offset[1], st.poly_offset[2], st.poly_offset[0]);
        // push constants: rescaling (none), render area, alpha reference
        static_assert(sizeof(Shader::Backend::SPIRV::RescalingLayout) + sizeof(Shader::Backend::SPIRV::RenderAreaLayout) <= 256);
        // As in suyu's graphics pipeline: which textures are scaled, and the down factor frag coords are
        // multiplied by (1 when the targets of this pass are not scaled).
        Shader::Backend::SPIRV::RescalingLayout rescaling{};
        std::memcpy(&rescaling, rescale_words.data(), sizeof(rescale_words));
        const f32 down_factor = Settings::values.resolution_info.active && pass_pw != pass_w ? Settings::values.resolution_info.down_factor : 1.0f;
        std::memcpy(reinterpret_cast<u8*>(&rescaling) + Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET, &down_factor, sizeof(down_factor));
        vkcmd.PushConstants(*pl->program->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, static_cast<u32>(sizeof(Shader::Backend::SPIRV::RescalingLayout)), &rescaling);
        vkcmd.PushConstants(*pl->program->layout, VK_SHADER_STAGE_ALL_GRAPHICS, Shader::Backend::SPIRV::ALPHA_REF_LAYOUT_OFFSET, 4, &st.alpha_ref);
        if (pl->program->update_template) {
            vkcmd.PushDescriptorSetWithTemplateKHR(*pl->program->update_template, *pl->program->layout, 0, data.data());
        }
        for (u32 b = 0; b < 4; ++b) {
            bool used = false;
            for (u32 a = 0; a < 16; ++a) used = used || (st.attrib[a] != 0 && (st.attrib[a] & 0x1f) == b);
            if (used) vkcmd.BindVertexBuffer(b, vbuf[b], voff[b]);
        }
        const bool qd = query_on && qn < 256 && st.rt_count == 0;
        if (qd) {
            vkcmd.BeginQuery(*qpool, qn, 0);
            qmeta[qn] = {st.prog[0], st.prog[1], indexed ? index_count : r.r[3], pass_w, pass_h, st.rt_count};
        }
        const auto issue = [&] {
            if (indexed) {
                vkcmd.DrawIndexed(index_count, 1, 0, vb_ranged ? -static_cast<s32>(vtx_shift) : base_vertex, 0);
            } else {
                vkcmd.Draw(r.r[3], 1, vb_ranged ? 0 : r.r[2], 0);
            }
        };
        if (indexed) vkcmd.BindIndexBuffer(arena, index_at, index_type);
#ifdef SUYU_HAS_FSR_VULKAN
        if (objv_cap_now) {
            // nvn_fsr_objvel: this draw's clip positions go to the capture buffer of this frame (one vec4 per vertex of
            // every assembled triangle, in primitive order)
            const ObjvEnt& oe = objv_cur.back();
            const VkBuffer xb = *objv_xfb[objv_slot];
            const VkDeviceSize xo = oe.offset, xs = VkDeviceSize{oe.verts} * 16;
            // [Debug] nvn_fsr_capture_ts (with gpu_diagnostics): a timestamp before and after each capture, so the S line gets
            // g.objv_capture_xfb (the game's draw run with the capture variant) and g.objv_capture_only (the extra
            // capture-only draws of expanded strips). The marks serialise the GPU around these draws: A/B only
            const bool cap_ts = objv_cap_ts && gp_on;
            if (objv_expanded) {
                // strip / fan with primitive restart: the game's draw as it is, then a capture-only draw (rasterizer
                // discard) of the expanded triangle list over the same vertex buffers and vertex offset
                issue();
                if (cap_ts) GpMark(gp_pass_label, 0);
                vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, objv_cap_pipe);
                vkcmd.BindIndexBuffer(arena, oe.list_at, VK_INDEX_TYPE_UINT32);
                vkcmd.BindTransformFeedbackBuffersEXT(0, 1, &xb, &xo, &xs);
                vkcmd.BeginTransformFeedbackEXT(0, 0, nullptr, nullptr);
                vkcmd.DrawIndexed(oe.verts, 1, 0, vb_ranged ? -static_cast<s32>(vtx_shift) : base_vertex, 0);
                vkcmd.EndTransformFeedbackEXT(0, 0, nullptr, nullptr);
                if (cap_ts) GpMark(GpLabel("objv_capture_only"), 1);
            } else {
                if (cap_ts) GpMark(gp_pass_label, 0);
                vkcmd.BindTransformFeedbackBuffersEXT(0, 1, &xb, &xo, &xs);
                vkcmd.BeginTransformFeedbackEXT(0, 0, nullptr, nullptr);
                issue();
                vkcmd.EndTransformFeedbackEXT(0, 0, nullptr, nullptr);
                if (cap_ts) GpMark(GpLabel("objv_capture_xfb"), 1);
            }
            ++objv_perf_caps;
        } else {
            issue();
        }
        if (fsr_redirect_now && fsr_hud_pipe && fsr_hud_pipe2) {
            // HUD layer, opaque draw without alphaToOne: the same draw again, alpha only (logic op SET -> coverage 1)
            vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_hud_pipe2);
            issue();
        }
        if (fsr_redirect_now && fsr_hud_pipe && fsr_redirect_class == 6) {
            // HUD layer, multiply draw: the same draw with the game's own pipeline into the transmittance image (cleared to
            // 1 at the frame's first multiply draw), which the composite applies to the scene under the layer. Bound state
            // (descriptors, push constants, viewport, scissor, buffers) carries over into the new render pass.
            if (FsrTmPrepare()) {
                EndPass();
                fsr_tm_target = true;
                if (BeginPassFor(mem, key)) {
                    vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pl->pipeline);
                    vkcmd.SetViewport(0, vp);
                    vkcmd.SetScissor(0, sc);
                    issue();
                    EndPass();
                    ++fsr_sec_multiply;
                }
                fsr_tm_target = false;
                pass_dirty = true;
            }
        }
#else
        issue();
#endif
        if (qd) {
            vkcmd.EndQuery(*qpool, qn);
            if (st.rt_count > 0) {
                if (Image* t = RenderTarget(mem, st.rt_tex[0], false)) {
                    if (!probe_buf) {
                        probe_buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                            .size = 256 * 64, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                            .pQueueFamilyIndices = nullptr},
                                                           MemoryUsage::Download);
                    }
                    EndPass();
                    const VkBufferImageCopy region{qn * 64ull, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                   {static_cast<s32>(t->width / 2) - 2, static_cast<s32>(t->height / 2) - 2, 0}, {4, 4, 1}};
                    vkcmd.CopyImageToBuffer(*t->image, VK_IMAGE_LAYOUT_GENERAL, *probe_buf, region);
                    Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
                    qprobe[qn] = true;
                }
            }
            ++qn;
        }
        if (pick_on && st.rt_count > 0) {
            if (Image* rimg = RenderTarget(mem, st.rt_tex[0], false)) {
                NvnVkDumpLine("nvn_ours_rtdraws.txt", fmt::format("rt {:x} {}x{} vkfmt {} vs {:x} fs {:x} count {} blend {:x} ds {:x} tex1.0 {:x}", rimg->cpu, rimg->width, rimg->height, static_cast<int>(rimg->format), st.prog[0], st.prog[1],
                                                                 indexed ? index_count : r.r[3], key.blend_enable, key.ds, handles[1][0]));
            }
        }
        if (pick_on) DumpDrawState(mem, st, handles, key, indexed ? index_count : r.r[3]);
        if (pick_on && pn < 512 && st.rt_count > 0 && pass_w == 1920) {
            if (Image* t = RenderTarget(mem, st.rt_tex[0], false)) {
                if (!pick_buf) {
                    pick_buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                       .size = 512 * 32768, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                       .pQueueFamilyIndices = nullptr},
                                                      MemoryUsage::Download);
                }
                EndPass();
                const VkBufferImageCopy region{pn * 32768ull, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {100, 600, 0}, {128, 64, 1}};
                vkcmd.CopyImageToBuffer(*t->image, VK_IMAGE_LAYOUT_GENERAL, *pick_buf, region);
                Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
                std::string m = fmt::format("rt {:x} fmt {} prog {:x} {:x} count {} blend {:x} ds {:x} tex:", t->cpu, static_cast<int>(t->format),
                                            st.prog[0], st.prog[1], indexed ? index_count : r.r[3], key.blend_enable, st.ds_flags);
                for (u32 ss = 0; ss < 2; ++ss) {
                    for (u32 ii = 0; ii < 8; ++ii) {
                        if (bound[ss][ii].img) m += fmt::format(" [{}.{} fmt {} {}x{} {:x}]", ss, ii, static_cast<int>(bound[ss][ii].img->format),
                                                                bound[ss][ii].img->width, bound[ss][ii].img->height, bound[ss][ii].img->cpu);
                    }
                }
                pick_meta[pn] = std::move(m);
                ++pn;
            }
        }
        ps_r.reset();
        ++stats.draws_done;
        if (up_probe) {
            std::array<const Image*, 16> up_sampled{};
            for (u32 ss = 0; ss < 2; ++ss) {
                for (u32 ii = 0; ii < 8; ++ii) up_sampled[ss * 8 + ii] = bound[ss][ii].img;
            }
            UpProbeDraw(mem, up_sampled.data());
        }
        for (u32 i = 0; i < st.rt_count; ++i) {
            if (Image* t = RenderTarget(mem, st.rt_tex[i], false)) ++t->draws;
        }
        if (st.rt_count == 0 && st.depth_tex) {
            if (Image* d = RenderTarget(mem, st.depth_tex, true)) {
                ++d->draws;
                if (d->width == 1024 && (d->draws <= 40 || (d->draws >= 20000 && d->draws < 20040))) {
                    ++shadow_logged;
                    ++stats.notes[fmt::format("  shadow draw[{}] count {} idx {} vp [{:.0f} {:.0f} {:.0f} {:.0f}] sc {} [{} {} {} {}] ds {:x} poly {:x} prog {:x} {:x} off {} {} {}",
                                              shadow_logged, r.r[3], indexed, st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3],
                                              st.scissor_set, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3], st.ds_flags, st.poly_flags,
                                              st.prog[0], st.prog[1], st.poly_offset[0], st.poly_offset[1], st.poly_offset[2])];
                }
            }
        }
    }

    bool BeginPassFor(Core::Memory::Memory& mem, const PipelineKey& key) {
        return BeginPass(mem, key);
    }

    void MakePlaceholder() {
        placeholder = std::make_unique<Image>();
        placeholder->format = VK_FORMAT_R8G8B8A8_UNORM;
        placeholder->width = placeholder->height = 1;
        placeholder->image = allocator.CreateImage({
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {1, 1, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        placeholder->view = dev.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *placeholder->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .components = {},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        });
        pending_transitions.push_back({*placeholder->image, VK_IMAGE_ASPECT_COLOR_BIT});
    }

    // ---- output ------------------------------------------------------------------------------------------
    static u32 Crc(u32 crc, const u8* p, size_t n) {
        static u32 table[256];
        static bool init = false;
        if (!init) {
            for (u32 i = 0; i < 256; ++i) {
                u32 c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        crc = ~crc;
        for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
        return ~crc;
    }

    static void WritePng(const std::filesystem::path& path, u32 w, u32 h, const u8* rgba) {
        std::vector<u8> raw;
        raw.reserve((w * 4 + 1) * h);
        for (u32 y = 0; y < h; ++y) {
            raw.push_back(0);
            raw.insert(raw.end(), rgba + y * w * 4, rgba + (y + 1) * w * 4);
        }
        std::vector<u8> z{0x78, 0x01};
        size_t pos = 0;
        while (pos < raw.size()) {
            const size_t n = std::min<size_t>(65535, raw.size() - pos);
            z.push_back(pos + n == raw.size() ? 1 : 0);
            z.push_back(n & 0xff);
            z.push_back(n >> 8);
            z.push_back(~n & 0xff);
            z.push_back((~n >> 8) & 0xff);
            z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
            pos += n;
        }
        u32 a = 1, b = 0;
        for (const u8 c : raw) {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        const u32 adler = (b << 16) | a;
        for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 0xff);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        const auto be32 = [](u32 v) { return std::array<u8, 4>{u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)}; };
        const auto chunk = [&](const char* type, const std::vector<u8>& d) {
            const auto len = be32(static_cast<u32>(d.size()));
            f.write(reinterpret_cast<const char*>(len.data()), 4);
            std::vector<u8> td(type, type + 4);
            td.insert(td.end(), d.begin(), d.end());
            f.write(reinterpret_cast<const char*>(td.data()), td.size());
            const auto crc = be32(Crc(0, td.data(), td.size()));
            f.write(reinterpret_cast<const char*>(crc.data()), 4);
        };
        const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        f.write(reinterpret_cast<const char*>(sig), 8);
        std::vector<u8> ihdr;
        for (const u32 v : {w, h}) {
            const auto x = be32(v);
            ihdr.insert(ihdr.end(), x.begin(), x.end());
        }
        ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
        chunk("IHDR", ihdr);
        chunk("IDAT", z);
        chunk("IEND", {});
    }

    // Depth image as a grey picture (near = dark, far = bright, normalized to what the image holds).
    void DumpDepth(Image& img, const std::string& name) {
        WaitAllFrames(PB_WaitDump);
        const size_t bytes = size_t{img.width} * img.height * 4;
        auto buf = allocator.CreateBuffer(
            {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0, .size = bytes,
             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
             .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr},
            MemoryUsage::Download);
        SettleUtil();
        util_fence.Reset();
        vk::CommandBuffer cb(util_cmd, device.GetDispatchLoader());
        cb.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr});
        const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {img.width, img.height, 1}};
        cb.CopyImageToBuffer(*img.image, VK_IMAGE_LAYOUT_GENERAL, *buf, region);
        cb.End();
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = 0,
                        .pWaitSemaphores = nullptr, .pWaitDstStageMask = nullptr, .commandBufferCount = 1,
                        .pCommandBuffers = &util_cmd, .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr};
        CheckVk(queue.Submit(si, *util_fence), "vkQueueSubmit");
        {
            ProfScope ps{prof, PB_WaitDump};
            CheckVk(util_fence.Wait(), "fence wait");
        }
        buf.Invalidate();
        const f32* d = reinterpret_cast<const f32*>(buf.Mapped().data());
        f32 lo = 1e30f, hi = -1e30f;
        for (size_t i = 0; i < size_t{img.width} * img.height; ++i) {
            if (std::isfinite(d[i])) {
                lo = std::min(lo, d[i]);
                hi = std::max(hi, d[i]);
            }
        }
        std::vector<u8> rgba(bytes);
        for (size_t i = 0; i < size_t{img.width} * img.height; ++i) {
            const f32 v = hi > lo ? (d[i] - lo) / (hi - lo) : 0.0f;
            const u8 g = static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
            rgba[i * 4 + 3] = 255;
        }
        WritePng(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name, img.width, img.height, rgba.data());
        Note(fmt::format("dumped {} depth range {} .. {} format {}", name, lo, hi, static_cast<int>(img.format)));
    }

    // Present mode: copy the frame the game just presented into one of three images that suyu's
    // compositor blits to the window. Waits for our own rendering first, so the copy is finished.
    void WaitAllFrames(ProfBucket bucket) {
        ProfScope ps{prof, bucket};
        for (auto& f : frames) {
            if (f.pending) {
                CheckVk(f.fence.Wait(), "fence wait");
                f.pending = false;
            }
        }
    }

    bool util_pending{};
    int pres_prev{-1};

    /// Ring size actually used: [Debug] pres_ring, at least 4 with present_nodrain (one more slot of margin, the drain
    /// no longer holds the next blit back), at least 6 with present_fifo.
    static u32 PresRingEffective() {
        u32 n = static_cast<u32>(Core::GameSettings::PresRing());
        if (Core::GameSettings::PresentNoDrain()) n = std::max<u32>(n, 4);
        if (Core::GameSettings::PresentFifo()) n = std::max<u32>(n, 6);
        return n;
    }

    /// Once at start: the present mode, to the suyu log and the headers of nvn_present.txt / nvn_prof.txt / nvn_perf.txt.
    void LogPresentMode() {
        const bool nodrain = Core::GameSettings::PresentNoDrain();
        const int ini_ring = Core::GameSettings::PresRing();
        const u32 ring = PresRingEffective();
        std::string text = fmt::format("present mode: {} | ring {} (pres_ring {}{}) | fence_hold {} | present_fifo {}",
                                       nodrain ? "nodrain" : "drain", ring, ini_ring,
                                       ring != static_cast<u32>(ini_ring)
                                           ? (nodrain && ring == 4 && !Core::GameSettings::PresentFifo() ? ", raised to 4 for nodrain"
                                                                                                        : ", raised")
                                           : "",
                                       Core::GameSettings::FenceHold() ? "on" : "off",
                                       Core::GameSettings::PresentFifo() ? "on" : "off");
        if (nodrain) text += " | ring slots are reused only after suyu's composite read finished (scheduler tick)";
        LOG_INFO(Render_Vulkan, "NVN {}", text);
        PpLine("# " + text);
        prof.Line("# " + text);
        prof.PerfLine("# " + text);
    }

    // present_nodrain: which ring slot suyu's composite may still read. The composite (suyu's GPU thread) takes a slot in
    // PresentedImage (marked kPresPending), records its blit, and then reports the scheduler tick whose completion ends
    // that read (NvnVkPresentImageUsed). PresentCopy only writes a slot that is neither published nor pending, and whose
    // tick the GPU has reached. Guarded by pres_lock (lock order: pres_lock, then fifo_lock).
    static constexpr u64 kPresPending = ~u64{0};
    std::mutex pres_lock;
    std::vector<u64> pres_read_tick;    // 0 = no read outstanding
    std::vector<double> pres_pending_ms;
    int pres_handed{-1};
    MasterSemaphore* pres_sem{};
    // Per-second nodrain summary (render thread only).
    struct NodrainSec {
        double start{};
        u32 presents{}, skips{}, waits{}, forced{}, stale{};
        double submit_sum{}, submit_max{}, settle_sum{}, settle_max{}, wait_ms{}, wait_max{};
    } nd;

    /// Picks the ring slot for the next blit in nodrain mode. Common case: pres_next, checked without blocking. A slot
    /// still being read is skipped; only when every other slot is busy it polls (bounded at 200 ms, then overwrites).
    u32 PickNodrainSlot() {
        const u32 n = static_cast<u32>(pres_img.size());
        const double t0 = NvnVkNowMs();
        bool waited = false;
        for (;;) {
            {
                std::scoped_lock lk{pres_lock};
                if (pres_sem) pres_sem->Refresh();
                const int latest = pres_latest.load();
                std::vector<int> queued;
                if (Core::GameSettings::PresentFifo()) {
                    std::scoped_lock fl{fifo_lock};
                    queued.assign(fifo.begin(), fifo.end());
                    if (fifo_last >= 0) queued.push_back(fifo_last);
                }
                const double now = NvnVkNowMs();
                for (u32 k = 0; k < n; ++k) {
                    const u32 i = (pres_next + k) % n;
                    if (static_cast<int>(i) == latest || static_cast<int>(i) == pres_prev) continue;
                    if (std::find(queued.begin(), queued.end(), static_cast<int>(i)) != queued.end()) continue;
                    u64& t = pres_read_tick[i];
                    if (t == kPresPending) {
                        if (now - pres_pending_ms[i] < 250.0) continue;
                        ++nd.stale; // handed out but no tick reported (composite skipped its blit): treat as read
                        t = 0;
                    }
                    if (t != 0 && pres_sem && !pres_sem->IsFree(t)) continue;
                    t = 0;
                    if (k != 0) ++nd.skips;
                    if (waited) {
                        const double w = NvnVkNowMs() - t0;
                        ++nd.waits;
                        nd.wait_ms += w;
                        nd.wait_max = std::max(nd.wait_max, w);
                    }
                    return i;
                }
            }
            if (NvnVkNowMs() - t0 > 200.0) {
                // Should not happen (needs the composite >= ring-2 frames behind on the GPU): overwrite the oldest
                // candidate rather than hang the guest.
                ++nd.forced;
                u32 i = pres_next;
                for (u32 k = 0; k < n; ++k) {
                    i = (pres_next + k) % n;
                    if (static_cast<int>(i) != pres_latest.load() && static_cast<int>(i) != pres_prev) break;
                }
                std::scoped_lock lk{pres_lock};
                pres_read_tick[i] = 0;
                return i;
            }
            waited = true;
            ProfScope ps{prof, PB_WaitPresent};
            if (NvnVkNowMs() - t0 < 1.0) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    /// nodrain: one summary line per second in nvn_present.txt (the drain path writes a P line per present instead).
    void NodrainSecond(double now) {
        if (nd.start == 0) nd.start = now;
        if (now - nd.start < 1000.0) return;
        const double c = std::max<u32>(nd.presents, 1);
        PpLine(fmt::format("N t{:.1f} nodrain presents {} | record+submit avg {:.2f} max {:.2f} | settle_util avg {:.2f} max {:.2f} | "
                           "ring {} slot skips {} waits {} ({:.2f} ms, max {:.2f}) forced {} stale {}",
                           now, nd.presents, nd.submit_sum / c, nd.submit_max, nd.settle_sum / c, nd.settle_max, pres_img.size(),
                           nd.skips, nd.waits, nd.wait_ms, nd.wait_max, nd.forced, nd.stale));
        if (nd.forced) {
            LOG_WARNING(Render_Vulkan, "NVN present_nodrain: {} ring slot(s) overwritten while suyu's composite may still read them",
                        nd.forced);
        }
        nd = NodrainSec{};
        nd.start = now;
    }
    /// A util submit may still be in flight (present_nodrain): finish it before its fence or command buffer is reused.
    void SettleUtil() {
        if (util_pending) {
            ProfScope ps{prof, PB_WaitUtil};
            CheckVk(util_fence.Wait(), "fence wait");
            util_pending = false;
        }
    }

    // Order probe ([Debug] order_probe = true, then F8): for 60 presents, one line each with the window
    // texture index, the ring slot published, and a 64-column luminance signature of our finished copy
    // (centroid of brightness and of change vs the previous sample = where the scroll strip / hunter is).
    std::unordered_map<u32, u32> tex_index;
    u32 cur_tex{};
    u32 order_left{};
    u32 order_n{};
    std::vector<u8> prev_sig;

    void OrderSample(u32 idx, std::vector<u8>& cols) {
        constexpr u32 W = 64, H = 36;
        auto img = allocator.CreateImage({
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = nullptr, .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        auto buf = allocator.CreateBuffer(
            {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0, .size = W * H * 4,
             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
             .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr},
            MemoryUsage::Download);
        SettleUtil();
        util_fence.Reset();
        vk::CommandBuffer cb(util_cmd, device.GetDispatchLoader());
        cb.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr});
        const VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0,
                                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *img,
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        cb.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, b);
        const VkImageBlit blit{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               {{0, 0, 0}, {static_cast<s32>(pres_w), static_cast<s32>(pres_h), 1}},
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {W, H, 1}}};
        cb.BlitImage(*pres_img[idx], VK_IMAGE_LAYOUT_GENERAL, *img, VK_IMAGE_LAYOUT_GENERAL, blit, VK_FILTER_LINEAR);
        const VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        cb.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, mb);
        const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
        cb.CopyImageToBuffer(*img, VK_IMAGE_LAYOUT_GENERAL, *buf, region);
        cb.End();
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = 0,
                        .pWaitSemaphores = nullptr, .pWaitDstStageMask = nullptr, .commandBufferCount = 1,
                        .pCommandBuffers = &util_cmd, .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr};
        CheckVk(queue.Submit(si, *util_fence), "vkQueueSubmit");
        CheckVk(util_fence.Wait(), "fence wait");
        buf.Invalidate();
        const u8* p = buf.Mapped().data();
        cols.assign(W, 0);
        for (u32 x = 0; x < W; ++x) {
            u32 sum = 0;
            for (u32 y = 0; y < H; ++y) {
                const u8* px = p + (y * W + x) * 4;
                sum += (px[0] + px[1] + px[2]) / 3;
            }
            cols[x] = static_cast<u8>(sum / H);
        }
    }

    void PresentOrderLine(u64 seq, u32 idx, double t, double total) {
        if (!Core::GameSettings::OrderProbe()) return;
        static bool f8_was = false;
        bool f8 = false;
        f8 = Common::HostKeyDown(0x77);
        if (f8 && !f8_was) {
            order_left = 60;
            order_n = 0;
            prev_sig.clear();
            PpLine(fmt::format("O ---- armed (F8) t{:.1f}: next 60 presents", t));
        }
        f8_was = f8;
        if (order_left == 0) return;
        --order_left;
        std::string text = fmt::format("O{} P{} t{:.1f} tex {} ring {} total {:.1f}", ++order_n, seq, t, cur_tex, idx, total);
        if (Core::GameSettings::PresentNoDrain()) {
            text += " sig n/a (nodrain: copy not finished)";
        } else {
            std::vector<u8> cols;
            OrderSample(idx, cols);
            double sw = 0, sx = 0, dw = 0, dx = 0;
            for (u32 x = 0; x < cols.size(); ++x) {
                sw += cols[x];
                sx += double(cols[x]) * x;
                if (prev_sig.size() == cols.size()) {
                    const double d = std::abs(int(cols[x]) - int(prev_sig[x]));
                    dw += d;
                    dx += d * x;
                }
            }
            text += fmt::format(" cx {:.2f} mx {}", sw > 0 ? sx / sw : -1.0,
                                dw > 0 ? fmt::format("{:.2f}", dx / dw) : std::string("-"));
            text += " sig ";
            for (u8 c : cols) text += fmt::format("{:02x}", c);
            prev_sig = std::move(cols);
        }
        PpLine(text);
    }

    void PresentCopy(Image& src) {
        const double pp_t0 = NvnVkNowMs();
        const bool nodrain = Core::GameSettings::PresentNoDrain();
        if (!nodrain) WaitAllFrames(PB_WaitPresent);
        const double pp_t1 = NvnVkNowMs();
        if (pres_img.empty()) {
            const u32 n = PresRingEffective();
            pres_img.resize(n);
            pres_seq.assign(n, 0);
            pres_done_ms.assign(n, 0.0);
            std::scoped_lock lk{pres_lock};
            pres_read_tick.assign(n, 0);
            pres_pending_ms.assign(n, 0.0);
        }
        const bool first = !pres_img[0];
        if (first) {
            pres_w = src.pw ? src.pw : src.width;
            pres_h = src.ph ? src.ph : src.height;
            if (split_on) {
                // split scale: the copies are at the output (1X) even when the first presented image was still created at the
                // render scale (the blit below scales; the image itself is re-created at 1X on its next use)
                pres_w = src.width;
                pres_h = src.height;
            }
            for (auto& im : pres_img) {
                im = allocator.CreateImage({
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                    .extent = {pres_w, pres_h, 1},
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT,
                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                });
            }
        }
        double settle_ms = 0;
        if (nodrain) {
            // Finish the previous blit first (its fence is reused below); it is published after this submit.
            const double s0 = NvnVkNowMs();
            SettleUtil();
            settle_ms = NvnVkNowMs() - s0;
        }
        const u32 idx = nodrain ? PickNodrainSlot() : pres_next;
        pres_next = (idx + 1) % static_cast<u32>(pres_img.size());
        SettleUtil();
        util_fence.Reset();
        vk::CommandBuffer cb(util_cmd, device.GetDispatchLoader());
        cb.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr});
        if (first) {
            for (auto& im : pres_img) {
                const VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0,
                                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *im,
                                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                cb.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, b);
            }
        }
        if (nodrain) {
            // Our own rendering is no longer waited for on the CPU: order the blit after it on this queue.
            const VkMemoryBarrier pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
            cb.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, pre);
        }
        const VkImageBlit blit{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               {{0, 0, 0}, {static_cast<s32>(src.pw ? src.pw : src.width), static_cast<s32>(src.ph ? src.ph : src.height), 1}},
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               {{0, 0, 0}, {static_cast<s32>(pres_w), static_cast<s32>(pres_h), 1}}};
        cb.BlitImage(*src.image, VK_IMAGE_LAYOUT_GENERAL, *pres_img[idx], VK_IMAGE_LAYOUT_GENERAL, blit, VK_FILTER_LINEAR);
        const VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        cb.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, mb);
        cb.End();
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = 0,
                        .pWaitSemaphores = nullptr, .pWaitDstStageMask = nullptr, .commandBufferCount = 1,
                        .pCommandBuffers = &util_cmd, .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr};
        CheckVk(queue.Submit(si, *util_fence), "vkQueueSubmit");
        const double pp_t2 = NvnVkNowMs();
        if (nodrain) {
            // Publish the previous frame's copy (its fence was settled above): suyu never reads an unfinished image.
            util_pending = true;
            const u64 seq = ++pres_ctr;
            pres_seq[idx] = seq;
            if (pres_prev >= 0) {
                // "done" = when the copy became visible to the composite (as in drain mode, where that is right after
                // the util fence): the previous copy, now, after SettleUtil confirmed it finished.
                pres_done_ms[pres_prev] = NvnVkNowMs();
                PublishCopy(pres_prev);
            }
            PresentOrderLine(seq, idx, pp_t2, pp_t2 - pp_t0);
            pres_prev = static_cast<int>(idx);
            ++nd.presents;
            const double rec_ms = std::max(0.0, pp_t2 - pp_t1 - settle_ms); // includes the slot pick (and its rare waits)
            nd.submit_sum += rec_ms;
            nd.submit_max = std::max(nd.submit_max, rec_ms);
            nd.settle_sum += settle_ms;
            nd.settle_max = std::max(nd.settle_max, settle_ms);
            NodrainSecond(pp_t2);
            return;
        }
        {
            ProfScope ps{prof, PB_WaitUtil};
            CheckVk(util_fence.Wait(), "fence wait");
        }
        const double pp_t3 = NvnVkNowMs();
        const u64 seq = ++pres_ctr;
        pres_seq[idx] = seq;
        pres_done_ms[idx] = pp_t3;
        PublishCopy(static_cast<int>(idx));
        PresentOrderLine(seq, idx, pp_t3, pp_t3 - pp_t0);
        if (Core::GameSettings::GpuDiagnosticsEnabled()) { // per-present P line: nvn_present.txt grew without limit otherwise
            static double last_copy = 0;
            PpLine(fmt::format("P{} t{:.1f} dt {:.1f} | wait_frames {:.1f} record+submit {:.1f} wait_util {:.1f} total {:.1f}{}",
                               seq, pp_t3, last_copy != 0 ? pp_t3 - last_copy : 0.0, pp_t1 - pp_t0, pp_t2 - pp_t1,
                               pp_t3 - pp_t2, pp_t3 - pp_t0, (last_copy != 0 && pp_t3 - last_copy > 25.0) ? " LATE-COPY" : ""));
            last_copy = pp_t3;
        }
    }

    // [Debug] present_fifo: finished copies wait here in order; each composite takes the next one.
    std::mutex fifo_lock;
    std::deque<int> fifo;
    int fifo_last{-1};
    u32 fifo_dropped{}, fifo_repeats{};

    void PublishCopy(int idx) {
        if (!Core::GameSettings::PresentFifo()) {
            pres_latest = idx;
            return;
        }
        std::scoped_lock lk{fifo_lock};
        fifo.push_back(idx);
        while (fifo.size() > 3) { // composite is far behind: keep latency bounded, never reuse a slot suyu may still read
            fifo.pop_front();
            ++fifo_dropped;
        }
        pres_latest = idx; // keeps "something exists" for the first composite
    }

    int NextCopy() {
        if (!Core::GameSettings::PresentFifo()) return pres_latest.load();
        std::scoped_lock lk{fifo_lock};
        if (!fifo.empty()) {
            fifo_last = fifo.front();
            fifo.pop_front();
        } else if (fifo_last >= 0) {
            ++fifo_repeats;
        }
        return fifo_last;
    }

    bool PresentedImage(VkImage& image, unsigned& w, unsigned& h) {
        const bool nodrain = Core::GameSettings::PresentNoDrain();
        std::unique_lock<std::mutex> lk{pres_lock, std::defer_lock};
        if (nodrain) lk.lock();
        const int i = NextCopy();
        if (i < 0) return false;
        if (nodrain && static_cast<size_t>(i) < pres_read_tick.size()) {
            // The composite reads slot i from now on; NvnVkPresentImageUsed replaces this with the tick that ends the read.
            pres_read_tick[i] = kPresPending;
            pres_pending_ms[i] = NvnVkNowMs();
            pres_handed = i;
        }
        shown_idx = i;
        shown_seq = pres_seq[i];
        shown_done_ms = pres_done_ms[i];
        image = *pres_img[i];
        w = pres_w;
        h = pres_h;
        return true;
    }

    void DumpImage(Image& img, const std::string& name) {
        // Wait for all rendering, then copy the image to a host buffer.
        WaitAllFrames(PB_WaitDump);
        const size_t bytes = size_t{img.width} * img.height * 4;
        auto buf = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = bytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
        SettleUtil();
        util_fence.Reset();
        vk::CommandBuffer cb(util_cmd, device.GetDispatchLoader());
        cb.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr});
        const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {img.width, img.height, 1}};
        cb.CopyImageToBuffer(*img.image, VK_IMAGE_LAYOUT_GENERAL, *buf, region);
        cb.End();
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = 0,
                              .pWaitSemaphores = nullptr, .pWaitDstStageMask = nullptr, .commandBufferCount = 1,
                              .pCommandBuffers = &util_cmd, .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr};
        CheckVk(queue.Submit(si, *util_fence), "vkQueueSubmit");
        {
            ProfScope ps{prof, PB_WaitDump};
            CheckVk(util_fence.Wait(), "fence wait");
        }
        buf.Invalidate();
        WritePng(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name, img.width, img.height, buf.Mapped().data());
        Note(fmt::format("dumped {}", name));
    }

    // ---- upscaler probe ([Debug] nvn_upscaler_probe, default off): user/nvn_upscaler_probe.txt ----------------
    // Observation only. Lists every distinct render target configuration with the raw NVN format value (RtFormat
    // folds unknown ones to RGBA8), per-frame draw counts per target, and a guess at the depth / HUD boundary.
    using UpKey = std::tuple<u32, u32, u32, bool>; // width, height, raw nvn format, is depth
    void UpProbeTarget(Core::Memory::Memory& mem, u32 tex, bool depth, u32 draw_idx) {
        if (tex == 0 || !mem.IsValidVirtualAddressRange(tex, 96)) return;
        const UpKey k{mem.Read32(tex + 28), mem.Read32(tex + 32), mem.Read32(tex + 44), depth};
        ++up_frame_counts[k];
        {
            UpImg& im = up_imgs[RtKey(mem, tex)];
            if (im.draws++ == 0) {
                im.w = std::get<0>(k);
                im.h = std::get<1>(k);
                im.fmt = std::get<2>(k);
                im.depth = depth;
                im.first = draw_idx;
            }
            im.last = draw_idx;
        }
        if (up_seen.insert(k).second && up_lines.size() < 4000) {
            up_lines.push_back(fmt::format("new target config {}x{} nvn format {:#x} {} rt {:x}: frame {} draw {} (frame draw {}) mapped to vk {}",
                                           std::get<0>(k), std::get<1>(k), std::get<2>(k), depth ? "depth" : "color", RtKey(mem, tex), frame_no,
                                           stats.draws, draw_idx, depth ? static_cast<int>(kRtDepthFormat) : static_cast<int>(kRtColorFormat)));
        }
    }
    // Frames 600/1200/1800 (probe numbering = the value frame_no has after the present that ends the frame).
    bool UpDumpFrame(u64 f) const { return f == 600 || f == 1200 || f == 1800; }
    // Hex + float dump of the first 128 bytes of the stage 0 (then first two stage 1) uniform buffer bindings.
    void UpDumpUniforms(Core::Memory::Memory& mem, const char* what, u32 draw_idx) {
        for (u32 s = 0; s < 2; ++s) {
            u32 shown = 0;
            for (u32 i = 0; i < 16 && shown < (s == 0 ? 6u : 2u); ++i) {
                if (st.ub_addr[s][i] == 0) continue;
                ++shown;
                u32 ucpu = 0, uav = 0;
                std::array<u32, 32> w{};
                const bool ok = NR::GpuToCpu(mem, st.ub_addr[s][i], ucpu, uav) && uav >= 4;
                if (ok) mem.ReadBlock(ucpu, w.data(), std::min<u32>(128, uav) & ~3u);
                std::string o = fmt::format("uniforms frame {} draw {} ({}) stage {} cb{} va {:x} size {:x}{}", frame_no + 1, draw_idx, what, s, i + 3,
                                            st.ub_addr[s][i], st.ub_size[s][i], ok ? "" : " (unreadable)");
                for (u32 r = 0; ok && r < 8; ++r) {
                    float f[4];
                    std::memcpy(f, &w[r * 4], 16);
                    o += fmt::format("\n  +{:02x}: {:08x} {:08x} {:08x} {:08x} | {:.5g} {:.5g} {:.5g} {:.5g}", r * 16, w[r * 4], w[r * 4 + 1],
                                     w[r * 4 + 2], w[r * 4 + 3], f[0], f[1], f[2], f[3]);
                }
                if (up_lines.size() < 4000) up_lines.push_back(std::move(o));
            }
        }
    }
    void UpProbeDraw(Core::Memory::Memory& mem, const Image* const* sampled) {
        const u32 idx = up_draws++;
        for (u32 i = 0; i < 16; ++i) { // sampled render targets: only counts if the image was drawn into earlier this frame
            if (!sampled[i] || !sampled[i]->render_target) continue;
            const auto it = up_imgs.find(sampled[i]->cpu);
            if (it == up_imgs.end() || it->second.draws == 0) continue;
            if (it->second.sampled++ == 0) it->second.first_sampled = idx;
        }
        if (UpDumpFrame(frame_no + 1) || (up_f7_frame != 0 && frame_no + 1 == up_f7_frame)) {
            const auto big = [&](u32 tex) {
                return tex && mem.IsValidVirtualAddressRange(tex, 96) && mem.Read32(tex + 28) == 1920 && mem.Read32(tex + 32) == 1080;
            };
            if (st.rt_count && big(st.rt_tex[0]) && up_ub_color < 3) UpDumpUniforms(mem, "1920x1080 color", idx), ++up_ub_color;
            if (big(st.depth_tex) && up_ub_depth < 3) UpDumpUniforms(mem, "1920x1080 depth", idx), ++up_ub_depth;
        }
        for (u32 i = 0; i < st.rt_count; ++i) UpProbeTarget(mem, st.rt_tex[i], false, idx);
        if (st.depth_tex) {
            UpProbeTarget(mem, st.depth_tex, true, idx);
            up_last_depth = static_cast<s32>(idx);
        }
        up_rt0.push_back(st.rt_count && st.rt_tex[0] && mem.IsValidVirtualAddressRange(st.rt_tex[0], 96) ? RtKeyFrame(mem, st.rt_tex[0]) : 0);
    }
    // Burst capture only: the depth aspect of a D32_S8 image as raw little-endian float32 (w*h values, row 0 = top, no header),
    // plus a PNG stretched between the smallest and largest depth value below 1.0 (near = dark) for a quick look.
    void DumpDepthRaw(Image& img, const std::string& name) {
        WaitAllFrames(PB_WaitDump);
        const size_t bytes = size_t{img.width} * img.height * 4;
        auto buf = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = bytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);
        SettleUtil();
        util_fence.Reset();
        vk::CommandBuffer cb(util_cmd, device.GetDispatchLoader());
        cb.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr});
        const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {img.width, img.height, 1}};
        cb.CopyImageToBuffer(*img.image, VK_IMAGE_LAYOUT_GENERAL, *buf, region);
        cb.End();
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = 0,
                              .pWaitSemaphores = nullptr, .pWaitDstStageMask = nullptr, .commandBufferCount = 1,
                              .pCommandBuffers = &util_cmd, .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr};
        CheckVk(queue.Submit(si, *util_fence), "vkQueueSubmit");
        {
            ProfScope ps{prof, PB_WaitDump};
            CheckVk(util_fence.Wait(), "fence wait");
        }
        buf.Invalidate();
        const auto path = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name;
        const u8* src = buf.Mapped().data();
        {
            std::ofstream bin(path.string() + ".bin", std::ios::binary | std::ios::trunc);
            bin.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(bytes));
        }
        const size_t n = size_t{img.width} * img.height;
        std::vector<float> d(n);
        std::memcpy(d.data(), src, bytes);
        float lo = 1e30f, hi = -1e30f;
        for (const float v : d) {
            if (v < 1.0f && std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
        std::vector<u8> rgba(n * 4);
        const float range = hi > lo ? hi - lo : 1.0f;
        for (size_t i = 0; i < n; ++i) {
            const float t = (std::isfinite(d[i]) && d[i] < 1.0f && hi >= lo) ? std::clamp((d[i] - lo) / range, 0.0f, 1.0f) : 1.0f;
            const u8 g = static_cast<u8>(t * 255.0f);
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
            rgba[i * 4 + 3] = 255;
        }
        WritePng(path.string() + ".png", img.width, img.height, rgba.data());
        Note(fmt::format("dumped depth {} (min {} max {} below 1.0)", name, lo, hi));
    }

    // F6 burst: called at the end of each of 3 consecutive frames (probe numbering). Only the 1920x1080 targets drawn this frame.
    void UpBurstFrame(const std::string& cam_text) {
        std::error_code ec;
        const auto dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "upscaler_dump";
        std::filesystem::create_directories(dir, ec);
        const std::string tag = fmt::format("burst{}_f{}", up_burst_id, frame_no);
        for (const auto& [key, img] : rt_images) {
            if (img->width != 1920 || img->height != 1080 || up_imgs.find(key) == up_imgs.end()) continue;
            try {
                if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT && img->format == VK_FORMAT_R8G8B8A8_UNORM) {
                    DumpImage(*img, fmt::format("upscaler_dump/{}_color_{:x}_1920x1080.png", tag, key));
                } else if ((img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && img->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
                    DumpDepthRaw(*img, fmt::format("upscaler_dump/{}_depth_{:x}_1920x1080_float32", tag, key));
                }
            } catch (const std::exception& e) {
                Note(fmt::format("burst dump failed: {}", e.what()));
            }
        }
        std::ofstream cam(dir / (tag + "_cam.txt"), std::ios::trunc);
        cam << fmt::format("burst {} frame {}: camera projection and view (look-at) calls since the previous frame ended (first 24 view calls; main view = aspect matches), then the main camera state at this present\n", up_burst_id, frame_no)
            << cam_text
            << "depth .bin: raw little-endian float32, 1920*1080 values, row 0 = top, no header; the PNG is a contrast stretch only\n";
    }

    void UpProbeFrame(Core::Memory::Memory& mem, u32 texture) {
        const std::string cam_text = Core::GameSettings::CamProbeDrain();
        const bool log = frame_no <= 5 || frame_no % 60 == 0;
        if (log && up_lines.size() < 4000) {
            const u32 pk = texture && mem.IsValidVirtualAddressRange(texture, 96) ? RtKey(mem, texture) : 0;
            s32 first_present_rt = -1;
            for (size_t i = 0; pk && i < up_rt0.size(); ++i) {
                if (up_rt0[i] == pk) { first_present_rt = static_cast<s32>(i); break; }
            }
            std::string counts;
            for (const auto& [k, n] : up_frame_counts) {
                counts += fmt::format(" [{}x{} f{:#x} {}: {}]", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k) ? "D" : "C", n);
            }
            up_lines.push_back(fmt::format("frame {}: {} draws; last draw with a depth target bound {}; first draw into the present image {} (rt {:x}); draws per target:{}",
                                           frame_no, up_draws, up_last_depth, first_present_rt, pk, counts));
        }
        if (log && up_lines.size() < 4000) {
            for (const auto& [key, im] : up_imgs) {
                up_lines.push_back(fmt::format("  frame {} image rt {:x} {}x{} nvn format {:#x} {}: {} draws, first draw {}, last draw {}, sampled by {} later draws (first {})",
                                               frame_no, key, im.w, im.h, im.fmt, im.depth ? "depth" : "color", im.draws, im.first, im.last,
                                               im.sampled, im.sampled ? static_cast<s32>(im.first_sampled) : -1));
            }
        }
        // F7 capture: the frame that just ended is the one that was armed (its draws saw up_f7_frame).
        const bool f7_now = up_f7_frame != 0 && frame_no == up_f7_frame;
        if (UpDumpFrame(frame_no) || f7_now) {
            std::error_code ec;
            const auto dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "upscaler_dump";
            std::filesystem::create_directories(dir, ec);
            for (const auto& [key, img] : rt_images) {
                if (up_total_dumped >= 400) break;
                if (f7_now ? up_f7_dumped >= 40 : up_dumped >= 40) break;
                if (img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !(img->draws || img->clears)) continue;
                const bool sz = (img->width == 1920 && img->height == 1080) || (img->width == 960 && img->height == 540) ||
                                (f7_now && img->width == 480 && img->height == 270);
                if (!sz) continue;
                if (img->format != VK_FORMAT_R8G8B8A8_UNORM) continue;
                try {
                    DumpImage(*img, fmt::format("upscaler_dump/f{}{}_{:x}_{}x{}.png", frame_no, f7_now ? "_F7" : "", key, img->width, img->height));
                    ++up_total_dumped;
                    if (f7_now) ++up_f7_dumped; else ++up_dumped;
                } catch (const std::exception& e) {
                    Note(fmt::format("upscaler dump failed: {}", e.what()));
                }
            }
        }
        if (up_burst_id != 0 && frame_no >= up_burst_first && frame_no < up_burst_first + 3) UpBurstFrame(cam_text);
        up_draws = 0;
        up_last_depth = -1;
        up_rt0.clear();
        up_frame_counts.clear();
        up_imgs.clear();
        up_ub_color = up_ub_depth = 0;
        bool f7_hit = false;
        {
            const bool down = Common::HostKeyDown(VK_F7);
            f7_hit = down && !up_f7_down;
            up_f7_down = down;
        }
        bool f6_hit = false;
        {
            const bool down = Common::HostKeyDown(VK_F6);
            f6_hit = down && !up_f6_down;
            up_f6_down = down;
        }
        if (f6_hit && !(up_burst_id != 0 && frame_no < up_burst_first + 3)) { // F6: 3 consecutive frames, ids count up per run
            up_burst_first = frame_no + 1;
            ++up_burst_id;
            up_lines.push_back(fmt::format("F6 burst {} at frames {}..{}", up_burst_id, up_burst_first, up_burst_first + 2));
        }
        if (f7_now) up_f7_frame = 0;
        if (f7_hit) {
            up_f7_frame = frame_no + 1; // draws of the next frame see frame_no + 1 == up_f7_frame
            up_f7_dumped = 0;
            up_lines.push_back(fmt::format("F7 capture at frame {}", up_f7_frame));
        }
        if (f7_hit || f7_now || f6_hit || (log && (frame_no <= 5 || frame_no % 120 == 0))) {
            std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_upscaler_probe.txt", std::ios::trunc);
            for (const auto& l : up_lines) out << l << "\n";
        }
    }

    void WriteStats() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        const std::string memory_line = fmt::format("process private {} MB, peak pagefile {} MB; system commit free {} MB, address space free {} MB\n",
                                                    pmc.PrivateUsage >> 20, pmc.PeakPagefileUsage >> 20, ms.ullAvailPageFile >> 20, ms.ullAvailVirtual >> 20);
#else
        const std::string memory_line = "process memory: not reported on this platform\n";
#endif
        std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_vk.txt", std::ios::trunc);
        out << memory_line;
        // Reads of rasterizer-cached guest pages from non-CPU-core threads (ours: render thread, workers); 0 = never hit.
        out << fmt::format("rasterizer downloads from non-core threads {} (nvn_unsafe_reads {})\n", Core::Memory::NonCoreRasterizerDownloads(),
                           opt_unsafe_reads);
        if (last_mem) {
            std::vector<std::string> lines;
            NR::LogPools(*last_mem, lines);
            for (const auto& l : lines) out << "final" << l << "\n";
        }
        out << fmt::format("frames {} submits {} lists {} recs {} draws {} drawn {} clears {} presents {}\n", frame_no, stats.submits,
                           stats.lists, stats.recs, stats.draws, stats.draws_done, stats.clears, stats.presents);
        out << fmt::format("shaders ok {} fail {} ({:.0f} ms), pipelines ok {} fail {} ({:.0f} ms), images {} fail {} ({:.0f} ms), samplers {}, arena full {}\n",
                           stats.shader_ok, stats.shader_fail, stats.translate_ms, stats.pipeline_ok, stats.pipeline_fail,
                           stats.pipeline_ms, stats.images, stats.image_fail, stats.texture_ms, stats.samplers, stats.arena_full);
        {
            u32 n = 0;
            for (const auto& [k, img] : rt_images) {
                if ((img->draws || img->clears) && n++ < 60) {
                    out << fmt::format("  target {:x} {}x{} {} draws {} clears {}{}\n", k, img->width, img->height,
                                       img->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? "color" : "depth", img->draws, img->clears,
                                       img.get() == last_present ? " PRESENTED" : "");
                }
            }
        }
        for (const auto& [k, n] : vp_probe) {
            stats.notes[fmt::format("draw vp {} [{:.0f} {:.0f} {:.0f} {:.0f}] sc {} [{} {} {} {}] pass {}x{}", k[0] != 0, std::bit_cast<f32>(k[1]),
                                      std::bit_cast<f32>(k[2]), std::bit_cast<f32>(k[3]), std::bit_cast<f32>(k[4]), k[5] != 0,
                                      static_cast<s32>(k[6]), static_cast<s32>(k[7]), static_cast<s32>(k[8]), static_cast<s32>(k[9]), k[10], k[11])] += n;
        }
        vp_probe.clear();
        for (const auto& [k, v] : stats.notes) out << fmt::format("  {}: {}\n", k, v);
        Kernel::StallRing::TimedGuard lk{note_lock, Kernel::StallRing::LockNote};
        for (const auto& l : log_lines) out << l << "\n";
    }

    // ---- [Debug] nvn_mv_pass: camera-reprojection motion vectors (default off; nothing below runs when off) ------
    // Where: in the replay, right before the first draw of a frame into an image the game has presented before (the
    // game's own final upsample draw, the last draw of the frame); the scene depth is complete by then. Depth = the
    // depth target of the last draw this frame whose colour target 0 has the depth's size (the scene pass; at 1x this
    // is the 1920x1080 D32_S8 image), read through its depth-only sampled view in GENERAL layout.
    // Output: mv_img, R32G32_SFLOAT, the depth's allocated size (render size incl. render scale), GENERAL layout.
    // motion = previous pixel position - current pixel position, in those pixels (FSR: cur + mv = prev). Full formulae
    // and conventions: src/video_core/host_shaders/vulkan_nvn_motion_vectors.comp.
    // Camera pairing: frame F (= frame_no + 1 while F's submits replay, = the F-th guest present, since Present()
    // counts the guest's presents in the same order) uses Core::GameSettings::MvCamFor(F - lag) as current and
    // MvCamFor(F - 1 - lag) as previous; a record tagged N is the main camera computed between guest presents N-1 and
    // N. lag = [Debug] nvn_mv_lag (default 1). Which lag is right is measured by the F2 dump (colour warp error per lag).
    struct MvFrame {
        u64 frame{};
        int lag{};
        bool paired{}, reset{true};
        Core::GameSettings::MvCam cur{}, prev{};
        float reproj[16]{}, dir[16]{};
        u32 w{}, h{};   // mv / depth image pixels
        u32 gw{}, gh{}; // the depth target's game size
        u32 depth_key{}, color_key{};
        VkFormat color_format{VK_FORMAT_UNDEFINED};
    };

    void MvLog(const std::string& line) {
        if (!mv_log.is_open()) {
            mv_log.open(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_mv_pass.txt", std::ios::trunc);
        }
        mv_log << Common::Log::RedactText(line) << std::endl;
    }

    static void MvMul(const double* a, const double* b, double* o) {
        double t[16];
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                double s = 0.0;
                for (u32 k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
                t[r * 4 + c] = s;
            }
        }
        std::copy(t, t + 16, o);
    }

    // Gauss-Jordan with partial pivoting; false if singular.
    static bool MvInv(const double* m, double* o) {
        double a[4][8];
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                a[r][c] = m[r * 4 + c];
                a[r][c + 4] = r == c ? 1.0 : 0.0;
            }
        }
        for (u32 c = 0; c < 4; ++c) {
            u32 piv = c;
            for (u32 r = c + 1; r < 4; ++r) {
                if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
            }
            if (!(std::fabs(a[piv][c]) > 1e-300)) return false;
            if (piv != c) std::swap(a[piv], a[c]);
            const double inv = 1.0 / a[c][c];
            for (u32 k = 0; k < 8; ++k) a[c][k] *= inv;
            for (u32 r = 0; r < 4; ++r) {
                if (r == c) continue;
                const double f = a[r][c];
                if (f == 0.0) continue;
                for (u32 k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
        }
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 4; ++c) o[r * 4 + c] = a[r][c + 4];
        }
        return true;
    }

    static void MvIdentity(float* m) {
        for (u32 i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }

    // reproj = inv(P_cur) inv(V_cur) V_prev P_prev; dir = inv(P_cur) diag(1,1,1,0) Rot P_prev (Rot = inv(V_cur) V_prev
    // without its translation row). Row-major, row-vector products, computed in double. flip_y: the renderer's
    // 'noflipy' option (NDC +y = bottom row): the shader's NDC is mirrored, so both become F M F with F = diag(1,-1,1,1).
    static bool MvBuild(const Core::GameSettings::MvCam& cur, const Core::GameSettings::MvCam& prev, bool flip_y, float* reproj, float* dir) {
        double v[16], p[16], vp[16], pp[16];
        for (u32 i = 0; i < 16; ++i) {
            v[i] = cur.view[i];
            p[i] = cur.proj[i];
            vp[i] = prev.view[i];
            pp[i] = prev.proj[i];
        }
        double ip[16], iv[16];
        if (!MvInv(p, ip) || !MvInv(v, iv)) {
            MvIdentity(reproj);
            MvIdentity(dir);
            return false;
        }
        double r[16], rel[16], d[16];
        MvMul(ip, iv, r);
        MvMul(r, vp, r);
        MvMul(r, pp, r);
        MvMul(iv, vp, rel);
        rel[12] = rel[13] = rel[14] = 0.0;
        const double z[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
        MvMul(ip, z, d);
        MvMul(d, rel, d);
        MvMul(d, pp, d);
        if (flip_y) {
            const double f[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            MvMul(f, r, r);
            MvMul(r, f, r);
            MvMul(f, d, d);
            MvMul(d, f, d);
        }
        for (u32 i = 0; i < 16; ++i) {
            reproj[i] = static_cast<float>(r[i]);
            dir[i] = static_cast<float>(d[i]);
        }
        return true;
    }

    // The shader's formula on the CPU (self-checks): motion of the pixel centred at (cx, cy) with depth dz.
    static void MvCpu(const float* reproj, const float* dir, float dz, double cx, double cy, u32 w, u32 h, double& mx, double& my) {
        const double c[4] = {2.0 * cx / w - 1.0, 1.0 - 2.0 * cy / h, dz, 1.0};
        const float* m = dz >= 0.99999f ? dir : reproj;
        double q[4]{};
        for (u32 j = 0; j < 4; ++j) {
            for (u32 i = 0; i < 4; ++i) q[j] += c[i] * m[i * 4 + j];
        }
        double wq = q[3];
        if (std::fabs(wq) < 1e-12) wq = wq < 0 ? -1e-12 : 1e-12;
        mx = (q[0] / wq + 1.0) * 0.5 * w - cx;
        my = (1.0 - q[1] / wq) * 0.5 * h - cy;
        if (!std::isfinite(mx)) mx = 0.0;
        if (!std::isfinite(my)) my = 0.0;
        mx = std::clamp(mx, -16384.0, 16384.0);
        my = std::clamp(my, -16384.0, 16384.0);
    }

    static double MvEyeStep(const Core::GameSettings::MvCam& a, const Core::GameSettings::MvCam& b) {
        const double x = a.eye[0] - b.eye[0], y = a.eye[1] - b.eye[1], z = a.eye[2] - b.eye[2];
        return std::sqrt(x * x + y * y + z * z);
    }

    // Angle (degrees) of the relative rotation between two view matrices (upper 3x3).
    static double MvRotDeg(const Core::GameSettings::MvCam& a, const Core::GameSettings::MvCam& b) {
        double tr = 0.0; // trace(Ra^T Rb)
        for (u32 i = 0; i < 3; ++i) {
            for (u32 k = 0; k < 3; ++k) tr += static_cast<double>(a.view[k * 4 + i]) * b.view[k * 4 + i];
        }
        return std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
    }

    static bool MvResetFor(const MvFrame& i) {
        if (!i.paired) return true;
        if (i.cur.present == i.prev.present) return false; // no camera update between the two frames
        return i.cur.ptr != i.prev.ptr || i.cur.reset || MvEyeStep(i.cur, i.prev) > 100.0;
    }

    void MvPair(MvFrame& info, u64 f, int lag) {
        info.frame = f;
        info.lag = lag;
        info.paired = f > static_cast<u64>(lag) + 1 && Core::GameSettings::MvCamFor(f - lag, &info.cur) &&
                      Core::GameSettings::MvCamFor(f - 1 - lag, &info.prev);
        if (info.paired && !MvBuild(info.cur, info.prev, no_flip_y, info.reproj, info.dir)) info.paired = false;
        if (!info.paired) {
            MvIdentity(info.reproj);
            MvIdentity(info.dir);
        }
        info.reset = MvResetFor(info);
    }

    bool MvEnsurePipeline() {
        if (mv_ready) return true;
        if (mv_failed) return false;
        try {
            mv_sampler = dev.CreateSampler({
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .magFilter = VK_FILTER_NEAREST,
                .minFilter = VK_FILTER_NEAREST,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .mipLodBias = 0.0f,
                .anisotropyEnable = VK_FALSE,
                .maxAnisotropy = 1.0f,
                .compareEnable = VK_FALSE,
                .compareOp = VK_COMPARE_OP_ALWAYS,
                .minLod = 0.0f,
                .maxLod = 0.0f,
                .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                .unnormalizedCoordinates = VK_FALSE,
            });
            const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            }};
            mv_set_layout = dev.CreateDescriptorSetLayout({
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings = bindings.data(),
            });
            const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
            mv_layout = dev.CreatePipelineLayout({
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = 1,
                .pSetLayouts = mv_set_layout.address(),
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &pcr,
            });
            const std::array<VkDescriptorUpdateTemplateEntry, 2> entries{{
                {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, sizeof(VkDescriptorImageInfo)},
                {1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sizeof(VkDescriptorImageInfo), sizeof(VkDescriptorImageInfo)},
            }};
            mv_template = dev.CreateDescriptorUpdateTemplate({
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
                .pDescriptorUpdateEntries = entries.data(),
                .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
                .descriptorSetLayout = *mv_set_layout,
                .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE,
                .pipelineLayout = *mv_layout,
                .set = 0,
            });
            mv_module = dev.CreateShaderModule({
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = sizeof(VULKAN_NVN_MOTION_VECTORS_COMP_SPV),
                .pCode = VULKAN_NVN_MOTION_VECTORS_COMP_SPV,
            });
            mv_pipeline = dev.CreateComputePipeline({
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = *mv_module,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
                .layout = *mv_layout,
                .basePipelineHandle = {},
                .basePipelineIndex = 0,
            });
            mv_ready = true;
            MvLog("compute pipeline built (host shader vulkan_nvn_motion_vectors.comp, SPIR-V compiled at build time)");
            return true;
        } catch (const std::exception& e) {
            mv_failed = true;
            MvLog(fmt::format("compute pipeline failed: {} (pass off for this run)", e.what()));
            return false;
        }
    }

    void MvEnsureImage(u32 w, u32 h) {
        if (mv_img && mv_img->width == w && mv_img->height == h) return;
        if (mv_img) graveyard.push_back({serial, std::move(mv_img)}); // earlier submits may still write it
        auto img = std::make_unique<Image>();
        img->format = VK_FORMAT_R32G32_SFLOAT;
        img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        img->width = w;
        img->height = h;
        img->image = allocator.CreateImage({
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = img->format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        img->view = dev.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *img->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = img->format,
            .components = {},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        });
        mv_img = std::move(img);
        mv_img_fresh = true;
        MvLog(fmt::format("motion vector image {}x{} R32G32_SFLOAT created at frame {}", w, h, frame_no + 1));
    }

    // Every draw while the pass is on (cheap: a set lookup, and for scene draws two map lookups).
    void MvBeforeDraw(Core::Memory::Memory& mem) {
        if (st.rt_count == 0 || st.rt_tex[0] == 0) return;
        const u64 f = frame_no + 1;
        if (mv_present_keys.contains(RtKeyFrame(mem, st.rt_tex[0]))) {
            if (mv_done_frame != f) {
                mv_done_frame = f;
                MvDispatch(f);
            }
            return;
        }
        if (st.depth_tex == 0) return;
        Image* d = RenderTarget(mem, st.depth_tex, true);
        Image* c = RenderTarget(mem, st.rt_tex[0], false);
        if (d && c && d->width == c->width && d->height == c->height && d->width >= 320 && d->sampled_view &&
            (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
            mv_depth = d;
            mv_color = c;
            mv_depth_frame = f; // a scene depth pass was drawn this frame (nvn_fsr menu skip)
        }
    }

    void MvDispatch(u64 f) {
        MvFrame info{};
        info.frame = f;
        if (!mv_depth) {
            ++mv_no_depth;
            mv_last = info;
            return;
        }
        if (!MvEnsurePipeline()) return;
        Image& d = *mv_depth;
        const u32 w = d.pw ? d.pw : d.width, h = d.ph ? d.ph : d.height;
        MvEnsureImage(w, h);
        MvPair(info, f, Core::GameSettings::NvnMvLag());
        info.w = w;
        info.h = h;
        info.gw = d.width;
        info.gh = d.height;
        info.depth_key = d.cpu;
        info.color_key = mv_color ? mv_color->cpu : 0;
        info.color_format = mv_color ? mv_color->format : VK_FORMAT_UNDEFINED;
        EndPass();
        // depth: attachment writes -> compute reads (EndPass only adds a barrier when a pass was open)
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        const VkImageMemoryBarrier to_write{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = mv_img_fresh ? VkAccessFlags{0} : VkAccessFlags{VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT},
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = mv_img_fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *mv_img->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, to_write);
        mv_img_fresh = false;
        const std::array<VkDescriptorImageInfo, 2> desc{{
            {*mv_sampler, *d.sampled_view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, *mv_img->view, VK_IMAGE_LAYOUT_GENERAL},
        }};
        std::array<float, 32> pcs{};
        std::copy(info.reproj, info.reproj + 16, pcs.begin());
        std::copy(info.dir, info.dir + 16, pcs.begin() + 16);
        vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *mv_pipeline);
        vkcmd.PushConstants(*mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, pcs.data());
        vkcmd.PushDescriptorSetWithTemplateKHR(*mv_template, *mv_layout, 0, desc.data());
        vkcmd.Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        // mv written -> any later reader (sampling, copies); also orders the compute read of the depth before later depth writes
        Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        GpMarkNamed("camera_mv");
        ++mv_dispatched;
        if (info.paired) ++mv_paired_count;
        if (info.reset) ++mv_reset_count;
        // once a second: copy the result to a host buffer, read it a frame or two later (MvPoll) for the log
        const double now_ms = NvnVkNowMs();
        if (mv_chatty && !mv_stat_pending && now_ms - mv_stat_last_ms >= 1000.0) {
            const size_t bytes = size_t{w} * h * 8;
            mv_stat_texels = opt_texel_stats; // [Debug] nvn_fsr_texel_stats: the 16 MB image copy + CPU loop only when asked for
            if (mv_stat_texels && (!mv_stat_buf || mv_stat_bytes != bytes)) {
                mv_stat_buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                      .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                      .pQueueFamilyIndices = nullptr},
                                                     MemoryUsage::Download);
                mv_stat_bytes = bytes;
            }
            const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
            if (mv_stat_texels) {
                vkcmd.CopyImageToBuffer(*mv_img->image, VK_IMAGE_LAYOUT_GENERAL, *mv_stat_buf, region);
                Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            }
            mv_stat_pending = true;
            mv_stat_slot = cur;
            mv_stat_info = info;
            mv_stat_last_ms = now_ms;
        }
        mv_last = info;
    }

    // Present (render thread): the stats copy of an earlier frame, once its submit has finished.
    void MvPoll() {
        if (!mv_stat_pending || frames[mv_stat_slot].fence.GetStatus() != VK_SUCCESS) return;
        mv_stat_pending = false;
        const MvFrame& i = mv_stat_info;
        const float* v = nullptr;
        if (mv_stat_texels) {
            mv_stat_buf.Invalidate();
            v = reinterpret_cast<const float*>(mv_stat_buf.Mapped().data());
        }
        double sum = 0.0, mx = 0.0;
        u64 n = 0, bad = 0;
        for (u32 y = 0; v && y < i.h; y += 2) {
            for (u32 x = 0; x < i.w; x += 2) {
                const float a = v[(size_t{y} * i.w + x) * 2], b = v[(size_t{y} * i.w + x) * 2 + 1];
                if (!std::isfinite(a) || !std::isfinite(b)) {
                    ++bad;
                    continue;
                }
                const double m = std::hypot(static_cast<double>(a), static_cast<double>(b));
                sum += m;
                mx = std::max(mx, m);
                ++n;
            }
        }
        u32 rw = 0, rh = 0;
        Core::GameSettings::MvRenderSize(&rw, &rh);
        MvLog(fmt::format("frame {} mv {}x{} paired {} (records cur {} prev {}{}, lag {}) reset {} mean|mv| {:.3f} px max {:.1f} nan/inf {} "
                          "eye step {:.3f} rot {:.4f} deg jitter {:.4f} {:.4f} px (of {}x{}) | since last line: passes {} paired {} resets {} no-depth {}",
                          i.frame, i.w, i.h, i.paired ? "yes" : "no", i.cur.present, i.prev.present,
                          i.paired && i.cur.present == i.prev.present ? " same record" : "", i.lag, i.reset ? "yes" : "no",
                          n ? sum / n : 0.0, mx, bad, i.paired ? MvEyeStep(i.cur, i.prev) : 0.0, i.paired ? MvRotDeg(i.cur, i.prev) : 0.0,
                          i.cur.jitter_px[0], i.cur.jitter_px[1], rw, rh, mv_dispatched, mv_paired_count, mv_reset_count, mv_no_depth));
        mv_dispatched = mv_paired_count = mv_reset_count = mv_no_depth = 0;
        if (++mv_stat_lines % 10 == 1) MvLog("  capture: " + Core::GameSettings::MvCaptureStats());
    }

    // Copy a whole image (one aspect) to host memory. Waits for the GPU (debug dumps only).
    std::vector<u8> MvReadback(Image& img, VkImageAspectFlags aspect, u32 bpp) {
        const u32 w = img.pw ? img.pw : img.width, h = img.ph ? img.ph : img.height;
        WaitAllFrames(PB_WaitDump);
        const size_t bytes = size_t{w} * h * bpp;
        auto buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0, .size = bytes,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .queueFamilyIndexCount = 0, .pQueueFamilyIndices = nullptr},
                                          MemoryUsage::Download);
        SettleUtil();
        util_fence.Reset();
        vk::CommandBuffer cb(util_cmd, device.GetDispatchLoader());
        cb.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr,
                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr});
        const VkBufferImageCopy region{0, 0, 0, {aspect, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        cb.CopyImageToBuffer(*img.image, VK_IMAGE_LAYOUT_GENERAL, *buf, region);
        cb.End();
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreCount = 0,
                        .pWaitSemaphores = nullptr, .pWaitDstStageMask = nullptr, .commandBufferCount = 1,
                        .pCommandBuffers = &util_cmd, .signalSemaphoreCount = 0, .pSignalSemaphores = nullptr};
        CheckVk(queue.Submit(si, *util_fence), "vkQueueSubmit");
        {
            ProfScope ps{prof, PB_WaitDump};
            CheckVk(util_fence.Wait(), "fence wait");
        }
        buf.Invalidate();
        std::vector<u8> out(bytes);
        std::memcpy(out.data(), buf.Mapped().data(), bytes);
        return out;
    }

    static bool MvColor4(VkFormat f) {
        return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM;
    }

    static std::string MvMatText(const char* name, const float* m) {
        std::string s = fmt::format("{}:\n", name);
        for (u32 r = 0; r < 4; ++r) {
            s += fmt::format("  [{}] {:.9g} {:.9g} {:.9g} {:.9g}\n", r, m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
        }
        return s;
    }

    // Present of frame frame_no (render thread). The pass for this frame ran during its replay (mv_done_frame).
    void MvPresent(Core::Memory::Memory& mem, u32 texture) {
        if (texture && mem.IsValidVirtualAddressRange(texture, 96)) {
            if (const u32 k = RtKey(mem, texture); k != 0 && mv_present_keys.insert(k).second) {
                MvLog(fmt::format("frame {}: image {:x} is presented; the first draw into it each frame triggers the pass", frame_no, k));
            }
        }
        MvPoll();
        bool f2_hit = false;
        if (mv_keys) { // nvn_fsr without the debug hooks: no F2 poll
            const bool down = Common::HostKeyDown(VK_F2);
            f2_hit = down && !mv_f2_down;
            mv_f2_down = down;
        }
        const bool ran = mv_done_frame == frame_no && mv_last.frame == frame_no && mv_depth != nullptr;
        if (mv_dump_stage == 1 && frame_no == mv_dump_first) {
            if (ran && mv_color && MvColor4(mv_color->format)) {
                try {
                    mv_dump_prev_color = MvReadback(*mv_color, VK_IMAGE_ASPECT_COLOR_BIT, 4);
                    mv_dump_prev = mv_last;
                    mv_dump_stage = 2;
                } catch (const std::exception& e) {
                    MvLog(fmt::format("F2 dump: readback failed: {}", e.what()));
                    mv_dump_stage = 0;
                }
            } else {
                MvLog(fmt::format("F2 dump: no pass (or no RGBA8 scene colour) in frame {}; cancelled", frame_no));
                mv_dump_stage = 0;
            }
        } else if (mv_dump_stage == 2 && frame_no == mv_dump_first + 1) {
            if (ran) {
                try {
                    MvWriteDump();
                } catch (const std::exception& e) {
                    MvLog(fmt::format("F2 dump failed: {}", e.what()));
                }
            } else {
                MvLog(fmt::format("F2 dump: no pass in frame {}; cancelled", frame_no));
            }
            mv_dump_stage = 0;
            mv_dump_prev_color.clear();
        }
        if (f2_hit && mv_dump_stage == 0) {
            mv_dump_stage = 1;
            mv_dump_first = frame_no + 1;
            MvLog(fmt::format("F2: dumping frames {} (previous colour) and {} (motion vectors)", mv_dump_first, mv_dump_first + 1));
        }
        mv_depth = nullptr;
        mv_color = nullptr;
    }

    // F2 dump of frame frame_no (its pass ran; mv_dump_prev_color holds frame_no - 1's scene colour).
    void MvWriteDump() {
        const MvFrame& i = mv_last;
        const u32 w = i.w, h = i.h;
        const size_t n = size_t{w} * h;
        const std::vector<u8> mv_raw = MvReadback(*mv_img, VK_IMAGE_ASPECT_COLOR_BIT, 8);
        const std::vector<u8> depth_raw = MvReadback(*mv_depth, VK_IMAGE_ASPECT_DEPTH_BIT, 4);
        std::vector<u8> color;
        if (mv_color && MvColor4(mv_color->format) && (mv_color->pw ? mv_color->pw : mv_color->width) == w &&
            (mv_color->ph ? mv_color->ph : mv_color->height) == h) {
            color = MvReadback(*mv_color, VK_IMAGE_ASPECT_COLOR_BIT, 4);
        }
        std::vector<float> mv(n * 2), depth(n);
        std::memcpy(mv.data(), mv_raw.data(), n * 8);
        std::memcpy(depth.data(), depth_raw.data(), n * 4);
        std::error_code ec;
        const auto dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "upscaler_dump";
        std::filesystem::create_directories(dir, ec);
        const std::string base = fmt::format("mv_{}_{}x{}", frame_no, w, h);
        {
            std::ofstream bin(dir / (base + "_float32.bin"), std::ios::binary | std::ios::trunc);
            bin.write(reinterpret_cast<const char*>(mv.data()), static_cast<std::streamsize>(n * 8));
            std::ofstream dbin(dir / (base + "_depth_float32.bin"), std::ios::binary | std::ios::trunc);
            dbin.write(reinterpret_cast<const char*>(depth.data()), static_cast<std::streamsize>(n * 4));
        }
        // statistics of the GPU result
        constexpr double kScale = 16.0; // px of motion for full brightness in the PNG
        double sum = 0.0, sx = 0.0, sy = 0.0, mn = 1e30, mxv = 0.0;
        u64 nan = 0, big = 0, far_px = 0;
        std::vector<u8> png(n * 4);
        for (size_t k = 0; k < n; ++k) {
            const float a = mv[k * 2], b = mv[k * 2 + 1];
            if (depth[k] >= 0.99999f) ++far_px;
            if (!std::isfinite(a) || !std::isfinite(b)) {
                ++nan;
                png[k * 4] = 255, png[k * 4 + 1] = 0, png[k * 4 + 2] = 255, png[k * 4 + 3] = 255;
                continue;
            }
            const double m = std::hypot(static_cast<double>(a), static_cast<double>(b));
            sum += m;
            sx += a;
            sy += b;
            mn = std::min(mn, m);
            mxv = std::max(mxv, m);
            if (m > 1000.0) ++big;
            // hue = direction on screen (0 deg = right, 90 = up, 180 = left, 270 = down), value = |mv| / kScale
            double hue = std::atan2(-static_cast<double>(b), static_cast<double>(a)) * 180.0 / 3.14159265358979323846;
            if (hue < 0) hue += 360.0;
            const double val = std::clamp(m / kScale, 0.0, 1.0);
            const double c = val, hp = hue / 60.0, x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
            double r = 0, g = 0, bl = 0;
            if (hp < 1) r = c, g = x;
            else if (hp < 2) r = x, g = c;
            else if (hp < 3) g = c, bl = x;
            else if (hp < 4) g = x, bl = c;
            else if (hp < 5) r = x, bl = c;
            else r = c, bl = x;
            png[k * 4] = static_cast<u8>(r * 255.0 + 0.5);
            png[k * 4 + 1] = static_cast<u8>(g * 255.0 + 0.5);
            png[k * 4 + 2] = static_cast<u8>(bl * 255.0 + 0.5);
            png[k * 4 + 3] = 255;
        }
        WritePng(dir / (base + ".png"), w, h, png.data());
        if (!color.empty()) WritePng(dir / (base + "_color.png"), w, h, color.data());
        if (mv_dump_prev_color.size() == n * 4) WritePng(dir / (base + "_color_prev.png"), w, h, mv_dump_prev_color.data());
        const u64 good = n - nan;
        // smoothness: mean |mv(x+1,y) - mv(x,y)| and |mv(x,y+1) - mv(x,y)|
        double grad = 0.0;
        u64 gn = 0;
        for (u32 y = 0; y + 1 < h; y += 2) {
            for (u32 x = 0; x + 1 < w; x += 2) {
                const size_t k = size_t{y} * w + x;
                grad += std::hypot(mv[(k + 1) * 2] - mv[k * 2], mv[(k + 1) * 2 + 1] - mv[k * 2 + 1]);
                grad += std::hypot(mv[(k + w) * 2] - mv[k * 2], mv[(k + w) * 2 + 1] - mv[k * 2 + 1]);
                gn += 2;
            }
        }
        // shader check (CPU formula, same matrices) and parallax (full minus rotation-only motion), every 4th pixel
        double max_diff = 0.0, parallax = 0.0;
        u64 pn_ = 0;
        for (u32 y = 0; y < h; y += 4) {
            for (u32 x = 0; x < w; x += 4) {
                const size_t k = size_t{y} * w + x;
                double cx, cy, rx, ry;
                MvCpu(i.reproj, i.dir, depth[k], x + 0.5, y + 0.5, w, h, cx, cy);
                MvCpu(i.dir, i.dir, depth[k], x + 0.5, y + 0.5, w, h, rx, ry);
                max_diff = std::max({max_diff, std::fabs(cx - mv[k * 2]), std::fabs(cy - mv[k * 2 + 1])});
                parallax += std::hypot(cx - rx, cy - ry);
                ++pn_;
            }
        }
        // lag test: warp the previous frame's colour with the motion each lag predicts; the right lag has the lowest error
        std::string lag_text;
        if (!color.empty() && mv_dump_prev_color.size() == n * 4) {
            const auto err_for = [&](const float* rep, const float* dr, bool zero, double& valid_frac) {
                double e = 0.0;
                u64 cnt = 0, tot = 0;
                for (u32 y = 0; y < h; y += 4) {
                    for (u32 x = 0; x < w; x += 4) {
                        ++tot;
                        const size_t k = size_t{y} * w + x;
                        double mx = 0, my = 0;
                        if (!zero) MvCpu(rep, dr, depth[k], x + 0.5, y + 0.5, w, h, mx, my);
                        const long px = std::lround(x + mx), py = std::lround(y + my);
                        if (px < 0 || py < 0 || px >= static_cast<long>(w) || py >= static_cast<long>(h)) continue;
                        const size_t q = static_cast<size_t>(py) * w + static_cast<size_t>(px);
                        e += (std::abs(int(color[k * 4]) - int(mv_dump_prev_color[q * 4])) + std::abs(int(color[k * 4 + 1]) - int(mv_dump_prev_color[q * 4 + 1])) +
                              std::abs(int(color[k * 4 + 2]) - int(mv_dump_prev_color[q * 4 + 2]))) / 3.0;
                        ++cnt;
                    }
                }
                valid_frac = tot ? static_cast<double>(cnt) / tot : 0.0;
                return cnt ? e / cnt : -1.0;
            };
            double vf = 0.0;
            const double e0 = err_for(nullptr, nullptr, true, vf);
            lag_text += fmt::format("  no motion (mv = 0): mean colour error {:.3f} (0-255 scale)\n", e0);
            for (int L = 0; L <= 3; ++L) {
                MvFrame t{};
                MvPair(t, frame_no, L);
                if (!t.paired) {
                    lag_text += fmt::format("  lag {}: records not available\n", L);
                    continue;
                }
                const double e = err_for(t.reproj, t.dir, false, vf);
                lag_text += fmt::format("  lag {}: records cur {} prev {}{} eye step {:.3f} rot {:.4f} deg -> mean colour error {:.3f} over {:.1f}% of samples{}\n",
                                        L, t.cur.present, t.prev.present, t.cur.present == t.prev.present ? " (same record: mv = 0)" : "",
                                        MvEyeStep(t.cur, t.prev), MvRotDeg(t.cur, t.prev), e, vf * 100.0, L == i.lag ? "  <- used by the pass" : "");
            }
            lag_text += "  (the right lag has clearly the lowest error while the camera turns; equal errors = camera still)\n";
        } else {
            lag_text = "  not run: no RGBA8 scene colour of both frames at the mv size\n";
        }
        u32 rw = 0, rh = 0;
        Core::GameSettings::MvRenderSize(&rw, &rh);
        const bool still = i.paired && MvEyeStep(i.cur, i.prev) < 1e-3 && MvRotDeg(i.cur, i.prev) < 1e-4;
        std::string t;
        t += fmt::format("nvn_mv_pass dump of frame {} (F2 pressed before frame {}).\n", frame_no, mv_dump_first);
        t += fmt::format("files: {0}.png (hue = direction on screen: 0 deg right, 90 up, 180 left, 270 down; brightness = |mv| / {1} px, "
                         "full at >= {1} px; magenta = NaN), {0}_float32.bin (raw little-endian float32 pairs mv.x, mv.y per pixel, "
                         "row 0 = top, {2}x{3}, no header), {0}_depth_float32.bin (the depth used), {0}_color.png / _color_prev.png "
                         "(scene colour rt {4:x} of this and the previous frame)\n",
                         base, kScale, w, h, i.color_key);
        t += "convention: mv = previous position - current position in render pixels of this image (FSR: cur + mv = prev); "
             "x right, y down (image rows); camera motion only (no object motion); unjittered projections.\n";
        t += fmt::format("render size {}x{} (depth rt {:x}, game size {}x{}); jitter grid {}x{}\n", w, h, i.depth_key, i.gw, i.gh, rw, rh);
        t += fmt::format("pairing: lag {} -> current record = guest present {} , previous = {}; paired {}; reset {}; camera ptr {:#x} / {:#x}; "
                         "projection calls in the interval {} / {}; capture thread {:08x}\n",
                         i.lag, i.cur.present, i.prev.present, i.paired ? "yes" : "no", i.reset ? "yes" : "no", i.cur.ptr, i.prev.ptr,
                         i.cur.calls, i.prev.calls, i.cur.thread);
        t += "  " + Core::GameSettings::MvCaptureStats() + "\n";
        t += fmt::format("jitter (current record, render px of {}x{}, +x right, +y up): {:.5f} {:.5f}; in mv pixels: {:.5f} {:.5f}\n", rw, rh,
                         i.cur.jitter_px[0], i.cur.jitter_px[1], i.cur.jitter_px[0] * (rw ? double(w) / rw : 1.0),
                         i.cur.jitter_px[1] * (rh ? double(h) / rh : 1.0));
        t += fmt::format("camera: eye cur {:.4f} {:.4f} {:.4f} prev {:.4f} {:.4f} {:.4f}; eye step {:.4f} units; rotation {:.5f} deg\n",
                         i.cur.eye[0], i.cur.eye[1], i.cur.eye[2], i.prev.eye[0], i.prev.eye[1], i.prev.eye[2],
                         i.paired ? MvEyeStep(i.cur, i.prev) : 0.0, i.paired ? MvRotDeg(i.cur, i.prev) : 0.0);
        t += MvMatText("view (current)", i.cur.view) + MvMatText("projection (current, unjittered)", i.cur.proj) +
             MvMatText("view (previous)", i.prev.view) + MvMatText("projection (previous, unjittered)", i.prev.proj) +
             MvMatText("reproj = inv(P) inv(V) V_prev P_prev (row-vector)", i.reproj) + MvMatText("reproj_dir (depth >= 0.99999)", i.dir);
        t += fmt::format("|mv| over {} finite pixels: min {:.4f} max {:.4f} mean {:.4f} px; mean mv ({:.4f}, {:.4f}); NaN/Inf {}; |mv| > 1000: {}; "
                         "far pixels (depth >= 0.99999) {}\n",
                         good, good ? mn : 0.0, mxv, good ? sum / good : 0.0, good ? sx / good : 0.0, good ? sy / good : 0.0, nan, big, far_px);
        t += fmt::format("self-check: GPU vs CPU formula (same matrices, every 4th pixel): max |diff| {:.6f} px\n", max_diff);
        t += fmt::format("self-check: smoothness mean |neighbour difference| {:.5f} px; parallax (full minus rotation-only motion) mean {:.5f} px "
                         "(pure rotation -> ~0 and a smooth field)\n",
                         gn ? grad / gn : 0.0, pn_ ? parallax / pn_ : 0.0);
        t += fmt::format("self-check: camera {} -> mean |mv| {:.5f} px{}\n", still ? "STATIONARY" : "moving", good ? sum / good : 0.0,
                         still ? " (expected ~0)" : "");
        t += "lag test (colour of this frame vs the previous frame warped by each lag's motion, every 4th pixel):\n" + lag_text;
        std::ofstream(dir / (base + ".txt"), std::ios::trunc) << t;
        MvLog(fmt::format("F2 dump written: upscaler_dump/{}.* (mean |mv| {:.4f} px, NaN {}, GPU-CPU max diff {:.6f})", base,
                          good ? sum / good : 0.0, nan, max_diff));
    }

#ifdef SUYU_HAS_FSR_VULKAN
    // ---- [Debug] nvn_fsr: FSR 3.1 temporal upscaler (default off; nothing below runs when off) ------------------
    // Frame structure (upscaler probe, quest, frame 10380): the scene ends in image S ('743b' in the probe); draws 399/400
    // write the post-processed scene (tone, bloom) into image H ('7327'); draw 401 samples H while writing S; draws
    // 402..466 are the HUD into H; draw 467 (the game's final draw) samples H and scales it to the present image.
    // S is NOT "H without HUD" (no bloom/grade: it differs from H on ~100% of pixels), so FSR does not take S:
    //  - H = the render target the final draw samples, learnt every frame (fsr_src_key).
    //  - snapshot: right after the last full-screen post pass into H and before the first HUD draw into H (rule and
    //    measurements at FsrOnDraw; [Debug] nvn_fsr_snapdraw = N forces "after draw N"), H is copied to fsr_snap = the
    //    finished, post-processed scene WITHOUT HUD -> FSR colour input. (Until 2026-10-18 the copy was taken at the first
    //    draw that reads H (draw 401 here), which is before a full-screen pass: the HUD mask covered most of the screen.)
    //  - at the final draw, right after the motion vector pass: prep (scene depth -> R32F, optional reactive mask from the
    //    dynamic-object mask), FSR (snap, depth, motion vectors, reactive) -> fsr_out at the present image's size, then
    //    the composite (vulkan_nvn_fsr_composite.comp: HUD = where H differs from the snapshot, shown as the game's
    //    bilinear H; FSR everywhere else) -> fsr_comp, and the final draw samples fsr_comp in place of H (same UVs).
    // Every image stays in GENERAL (the renderer's convention); FSR receives them as UNORDERED_ACCESS (= GENERAL in
    // its backend) and transitions them back to that state at the end of its dispatch.
    // Keys: Numpad1 FSR on/off, Numpad2 reactive mask, Numpad3 jitter sign convention, Numpad4 view, Numpad5 sharpening, Numpad6 dump,
    // Numpad7 HUD layer, Numpad8 motion mask,
    // Page Down dump. Log: user/nvn_fsr.txt.
    struct FsrPipe {
        vk::DescriptorSetLayout set_layout;
        vk::PipelineLayout layout;
        vk::DescriptorUpdateTemplate tmpl;
        vk::ShaderModule module;
        vk::Pipeline pipeline;
    };

    void FsrLog(const std::string& line) {
        if (!fsr_log.is_open()) {
            fsr_log.open(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_fsr.txt", std::ios::trunc);
        }
        fsr_log << Common::Log::RedactText(line) << std::endl;
    }

    /// The periodic / per-frame lines: only with the diagnostics gates (fsr_chatty); errors, warnings, init and event lines use FsrLog.
    void FsrLogSec(const std::string& line) {
        if (fsr_chatty) FsrLog(line);
    }

    static const char* FsrSignText(int s) {
        switch (s) {
        case 0:
            return "0: jitterOffset = -(picture shift) in render px, +y down (AMD FSR sample convention; default)";
        case 1:
            return "1: jitterOffset = +(picture shift) (AMD header comment: the projection offsets themselves)";
        case 2:
            return "2: jitterOffset = (-x, +y) of the picture shift (y flipped vs default)";
        default:
            return "3: jitterOffset = (+x, -y) of the picture shift (x flipped vs default)";
        }
    }

    static const char* FsrViewText(u32 v) {
        switch (v) {
        case 0:
            return "0 composite (FSR scene + game HUD)";
        case 1:
            return "1 FSR output only (no HUD)";
        case 2:
            return "2 composite, HUD layer alpha (layer path) / HUD mask (diff path) tinted red";
        default:
            return "3 game bilinear of the HUD image (no FSR, jittered input; no HUD layer redirect in this view)";
        }
    }

    static std::string FsrResultText(FfxVkPortableResult r) {
        switch (r) {
        case FFX_VK_PORTABLE_OK:
            return "ok";
        case FFX_VK_PORTABLE_ERROR_INVALID_POINTER:
            return "invalid pointer";
        case FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE:
            return "invalid struct size";
        case FFX_VK_PORTABLE_ERROR_VULKAN:
            return "Vulkan error";
        case FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY:
            return "out of memory";
        case FFX_VK_PORTABLE_ERROR_UNSUPPORTED:
            return "unsupported";
        case FFX_VK_PORTABLE_ERROR_BACKEND:
            return "backend error";
        }
        return fmt::format("result {}", static_cast<int>(r));
    }

    static std::string FsrIssuesText(u64 issues) {
        std::string s;
        for (u32 b = 0; b < 64; ++b) {
            if (issues & (1ull << b)) {
                const char* n = ffxVkPortableValidationIssueName(1ull << b);
                s += fmt::format("{}{}", s.empty() ? "" : ", ", n ? n : "?");
            }
        }
        return s;
    }

    FfxVkPortableDeviceInfo FsrDeviceInfo() const {
        FfxVkPortableDeviceInfo di{};
        di.structSize = sizeof(di);
        di.physicalDevice = device.GetPhysical();
        di.device = *dev;
        di.getDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr;
        di.queueFamilyIndex = device.GetGraphicsFamily();
        // only what suyu's device really enabled (Vulkan cannot be asked after device creation)
        di.shaderFloat16Enabled = device.IsFloat16Supported() ? VK_TRUE : VK_FALSE;
        di.subgroupSizeControlEnabled =
            device.IsExtSubgroupSizeControlSupported() && device.IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT) ? VK_TRUE : VK_FALSE;
        // a mandatory feature of suyu's device (vulkan_device.h FOR_EACH_VK_MANDATORY_FEATURE): always enabled
        di.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;
        return di;
    }

    void FsrInit() {
        fsr_on = true;
        fsr_reactive = Core::GameSettings::NvnFsrReactive();
        fsr_sharpness = Core::GameSettings::NvnFsrSharpness();
        fsr_sharpness_ini = fsr_sharpness;
        fsr_sharpen = fsr_sharpness > 0.0f;
        fsr_sharp_idx = fsr_sharpen ? 1 : 0;
        fsr_react_strength = Core::GameSettings::NvnFsrReactiveStrength();
        const auto& dld = device.GetDispatchLoader();
        FfxVkSuyuLoader l{};
        l.device = *dev;
        l.getDeviceProcAddr = dld.vkGetDeviceProcAddr;
        l.getPhysicalDeviceProperties = dld.vkGetPhysicalDeviceProperties;
        l.getPhysicalDeviceProperties2 = dld.vkGetPhysicalDeviceProperties2;
        l.getPhysicalDeviceFeatures2 = dld.vkGetPhysicalDeviceFeatures2;
        l.getPhysicalDeviceMemoryProperties = dld.vkGetPhysicalDeviceMemoryProperties;
        l.getPhysicalDeviceFormatProperties = dld.vkGetPhysicalDeviceFormatProperties;
        l.getPhysicalDeviceQueueFamilyProperties = dld.vkGetPhysicalDeviceQueueFamilyProperties;
        l.enumerateDeviceExtensionProperties = dld.vkEnumerateDeviceExtensionProperties;
        const int missing = ffxVkSuyuLoaderInit(&l);
        FfxVkPortableDeviceCapabilities caps{};
        caps.structSize = sizeof(caps);
        const FfxVkPortableResult cr =
            missing == 0 ? ffxVkPortableQueryDeviceCapabilities(device.GetPhysical(), &caps) : FFX_VK_PORTABLE_ERROR_UNSUPPORTED;
        const FfxVkPortableDeviceInfo di = FsrDeviceInfo();
        FsrLog("nvn_fsr: FSR 3.1.4 temporal upscaler (externals/fsr-vulkan, FireBurn/FSR-Vulkan c64f093, AMD FidelityFX SDK v1.1.4)");
        fsr_dbg = Core::GameSettings::NvnFsrDebugHooks();
        fsr_dbg_checking = DebugOption("nvn_fsr_debug_checking", 0) != 0;
        fsr_chatty = fsr_dbg || Core::GameSettings::GpuDiagnosticsEnabled();
        fsr_hud_lo = Core::GameSettings::NvnFsrHudLo();
        fsr_hud_hi = Core::GameSettings::NvnFsrHudHi();
        FsrLog(fmt::format("FSR debug hooks {}: {}; ffx debug checking {}. {}", fsr_dbg ? "ACTIVE" : "off", Core::GameSettings::NvnFsrDebugHooksReason(),
                           fsr_dbg_checking ? "on" : "off",
                           fsr_dbg ? "Numpad keys, F2, ini re-reads once a second (nvn_fsr_snapdraw, nvn_fsr_hud_lo/_hi), per-draw draw lists / "
                                     "menu-check target summaries, FSR stage timestamps and the FSR jitter log are on"
                                   : "Startup values are used; no Numpad / F2 keys, no ini re-reads, no per-draw draw lists; FSR stage "
                                     "timestamps only with gpu_diagnostics ([Debug] nvn_fsr_debug = true or nvn_bench = 1 restores them)"));
        LOG_INFO(Render_Vulkan, "nvn_fsr: FSR debug hooks {} ({}), ffx debug checking {}", fsr_dbg ? "active" : "off", Core::GameSettings::NvnFsrDebugHooksReason(),
                 fsr_dbg_checking ? "on" : "off");
        FsrLog(fmt::format("ini: nvn_fsr_reactive {} nvn_fsr_sharpness {:.2f} (sharpening {}); motion vector lag {}", fsr_reactive ? "on" : "off",
                           fsr_sharpness, fsr_sharpen ? "on" : "off", Core::GameSettings::NvnMvLag()));
        FsrLog("keys: Numpad1 FSR on/off | Numpad2 reactive mask on/off | Numpad3 jitter sign convention | Numpad4 view | Numpad5 sharpening (off / ini / 0.6 / 1.0) | "
               "Numpad6 dump one frame to upscaler_dump/fsr_<frame>_*.png | Numpad7 HUD layer on/off | Numpad8 motion mask on/off | "
               "Numpad9 object motion (geometry + block -> geometry -> block -> off)");
        fsr_hudlayer = Core::GameSettings::NvnFsrHudLayer();
        FsrLog(fmt::format("HUD: {} ([Debug] nvn_fsr_hudlayer; Numpad7 toggles); alphaToOne {} (needed for opaque HUD draws in the layer)",
                           fsr_hudlayer ? "HUD LAYER (HUD draws after the snapshot go to a premultiplied layer composited over FSR)"
                                        : "old diff composite (HUD mask = where H differs from the snapshot)",
                           device.SupportsAlphaToOne() ? "supported"
                                                        : "NOT supported (opaque HUD draws get a second, alpha-only logic-op SET pass instead)"));
        FsrLog(fmt::format("device: {} ({}), vendor {:04x} device {:04x}, Vulkan {}.{}.{}, driver {:#x}", device.GetModelName(), device.GetDriverName(),
                           caps.vendorId, caps.deviceId, VK_API_VERSION_MAJOR(caps.apiVersion), VK_API_VERSION_MINOR(caps.apiVersion),
                           VK_API_VERSION_PATCH(caps.apiVersion), caps.driverVersion));
        FsrLog(fmt::format("support: loader shim {} (missing {}), capability query {}; compute queue {} subgroup ops {} (size {} min {} max {}) "
                           "FSR3 storage formats {} -> FSR3 prerequisites {}; physical fp16 {} subgroup size control {} sync2 {}",
                           missing == 0 ? "ok" : "FAILED", missing, FsrResultText(cr), caps.hasComputeQueue, caps.subgroupOperations,
                           caps.subgroupSize, caps.minSubgroupSize, caps.maxSubgroupSize, caps.requiredFsr3StorageFormats,
                           caps.fsr3ComputePrerequisites, caps.shaderFloat16, caps.subgroupSizeControl, caps.synchronization2));
        FsrLog(fmt::format("enabled on suyu's device (passed to FSR): shaderFloat16 {} subgroupSizeControl {} "
                           "shaderStorageImageWriteWithoutFormat {} (no device feature was added for FSR)",
                           di.shaderFloat16Enabled, di.subgroupSizeControlEnabled, di.shaderStorageImageWriteWithoutFormatEnabled));
        if (missing != 0 || cr != FFX_VK_PORTABLE_OK || !caps.fsr3ComputePrerequisites) {
            FsrFail("this device does not meet FSR 3's requirements (see the support line)");
            return;
        }
        fsr_active = true;
        fsr_need_reset = true;
        fsr_sign = Core::GameSettings::NvnFsrJitterConv();
        fsr_snapdraw = Core::GameSettings::NvnFsrSnapDraw();
        FsrLog(fmt::format("snapshot point: {} ([Debug] nvn_fsr_snapdraw, re-read from the ini once a second)",
                           fsr_snapdraw > 0 ? fmt::format("forced, right after draw {}", fsr_snapdraw) : std::string("automatic (0)")));
        FsrLog("jitter: the camera projection is always jittered the same way (game_settings: AMD Halton offset (ox, oy), "
               "m[8] -= 2 ox / w, m[9] -= 2 oy / h); the sign convention (Numpad3) only changes how that jitter is described to FSR "
               "(jitterOffset), it never changes the projection");
        FsrLog(fmt::format("jitter sign convention {} ([Debug] nvn_fsr_jitter_conv, default 1)", FsrSignText(fsr_sign)));
        FsrLog(fmt::format("view {}", FsrViewText(fsr_view)));
        fsr_motion = Core::GameSettings::NvnFsrMotionMask();
        fsr_objmv = Core::GameSettings::NvnFsrObjMv();
        fsr_objmv_near = Core::GameSettings::NvnFsrObjMvNear();
        FsrLog(fmt::format("object motion vectors ([Debug] nvn_fsr_objmv, now {}; Numpad9 toggles): 8x8 block matching (luma SAD, 12x12 "
                           "window) of the FSR input against the previous one, centred on the camera vector, for silhouette blocks "
                           "(dynamic-object mask now or last frame, +-12 px) and low-rate blocks (cloth: >= 8 of 64 texels changed "
                           "against the previous input, linear depth < {:.0f} ([Debug] nvn_fsr_objmv_near), +-20 px in the jump "
                           "frame, reactive held 2 frames); confident vectors replace the camera vector per texel where a 3x3 SAD "
                           "says they fit better; silhouette reactive lowered by 65% x history fit (3x3 SAD of the vector FSR gets) in "
                           "searched blocks",
                           fsr_objmv ? "on" : "off", fsr_objmv_near));
        objv_mode = Core::GameSettings::NvnFsrObjVel();
        objv_react_cap = Core::GameSettings::NvnFsrObjVelReactive();
        objv_max_px = Core::GameSettings::NvnFsrObjVelMaxPx();
        objv_max_verts = Core::GameSettings::NvnFsrObjVelMaxVerts();
        fsr_anim = Core::GameSettings::NvnFsrAnimSurf();
        fsr_anim_bypass = Core::GameSettings::NvnFsrAnimSurfBypass();
        fsr_anim_gate = Core::GameSettings::NvnFsrAnimSurfEdgeGate();
        fsr_menuskip = Core::GameSettings::NvnFsrMenuSkip();
        fsr_menu3d_on = Core::GameSettings::NvnFsrMenu3d();
        FsrLog(fmt::format("frames without a scene pass ([Debug] nvn_fsr_menuskip {}): {}", fsr_menuskip ? 1 : 0,
                           fsr_menuskip ? "old menu skip (no FSR, no jitter after 3 such frames)"
                                        : "FSR keeps running with still-picture inputs (zero motion, the picture's real jitter, last depth)"));
        objv_signals = Core::GameSettings::NvnFsrSignals();
        objv_static_on = Core::GameSettings::NvnFsrObjVelStatic();
        objv_static_frames = Core::GameSettings::NvnFsrStaticFrames();
        objv_static_rearm = objv_static_frames > 40 ? objv_static_frames - 40 : 0;
        FsrLog(fmt::format("static-draw exclusion: after {} static checked frames ([Debug] nvn_fsr_static_frames, default 240), for 600 "
                           "frames, then {} frames of credit", objv_static_frames, objv_static_rearm));
        objv_cap_ts = DebugOption("nvn_fsr_capture_ts", 0) != 0;
        fsr_bm_compact = DebugOption("nvn_fsr_bm_compact", 0) != 0;
        FsrLog(fmt::format("block matching layout: {} ([Debug] nvn_fsr_bm_compact = {})",
                           fsr_bm_compact ? "classify pass + indirect search of the listed blocks" : "one pass over every 8x8 block (default)",
                           fsr_bm_compact ? 1 : 0));
        if (objv_cap_ts) {
            FsrLog(fmt::format("[Debug] nvn_fsr_capture_ts on: GPU timestamps around every object-velocity capture (nvn_perf.txt "
                               "g.objv_capture_xfb / g.objv_capture_only{}); they serialise the GPU around those draws, A/B only",
                               Core::GameSettings::GpuDiagnosticsEnabled() ? "" : "; NOT active: needs gpu_diagnostics = true"));
        }
        FsrLog(fmt::format("cost options: [Debug] nvn_fsr_signals {} (pose-signal diagnostics), nvn_fsr_objvel_static {} (mode 4 stops "
                           "capturing draws that never differ from the camera vector), nvn_fsr_animsurf_bypass {}",
                           objv_signals ? "on" : "off", objv_static_on ? "on" : "off", fsr_anim_bypass ? "on" : "off"));
        fsr_anim_thr = Core::GameSettings::NvnFsrAnimSurfThr();
        FsrLog(fmt::format("animated surfaces ([Debug] nvn_fsr_animsurf, now {}; Numpad0 toggles; threshold [Debug] nvn_fsr_animsurf_thr "
                           "{:.3f}): 8x8 tiles where the object vector equals the camera vector and the 4x4 low-pass colour differs from "
                           "the reprojected previous input (jitter change removed) get reactive 0.95 in that frame and the next",
                           fsr_anim ? "on" : "off", fsr_anim_thr));
        objv_ok = objv_mode > 0 && ObjvCaptureBuildable();
        objv_on = objv_ok;
        if (objv_ok && (fsr_dbg || Core::GameSettings::GpuDiagnosticsEnabled())) {
            // GPU time per FSR stage (FsrTs 0..6 per frame slot, the FSR cost line): debug hooks or gpu_diagnostics only
            try {
                objv_write_ts = reinterpret_cast<PFN_vkCmdWriteTimestamp>(dld.vkGetDeviceProcAddr(*dev, "vkCmdWriteTimestamp"));
                const u32 nq = static_cast<u32>(frames.size()) * 8;
                objv_ts_pool = dev.CreateQueryPool({VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, nq, 0});
                objv_ts_period = device.GetPhysical().GetProperties().limits.timestampPeriod;
                objv_ts_written.assign(frames.size(), 0);
            } catch (const std::exception& e) {
                objv_write_ts = nullptr;
                FsrLog(fmt::format("object velocity: no GPU timestamps ({})", e.what()));
            }
        }
        FsrLog(fmt::format("geometry object motion vectors ([Debug] nvn_fsr_objvel = {}; Numpad9 cycles): transform feedback of the "
                           "tracked draws' clip positions (vertex shader variant), paired with the previous frame's capture of the same "
                           "draw, rasterized at this frame's position and validated against the scene depth; device transform feedback "
                           "{} (draw {}) -> {} | reactive cap on valid texels {:.2f} ([Debug] nvn_fsr_objvel_reactive), max motion "
                           "{:.0f} px ([Debug] nvn_fsr_objvel_maxpx), reactive strength elsewhere {:.2f} ([Debug] nvn_fsr_reactive_strength)",
                           ObjvModeText(objv_mode), device.IsExtTransformFeedbackSupported() ? "yes" : "NO",
                           device.IsTransformFeedbackDrawSupported() ? "yes" : "no", objv_ok ? "ON" : "OFF", objv_react_cap, objv_max_px,
                           fsr_react_strength));
        fsr_indicator = Core::GameSettings::NvnFsrIndicator();
        FsrLog(fmt::format("reactive mask: Numpad2 = silhouette (dynamic-object mask target, current frame + previous frame's at the "
                           "reprojected texel), Numpad8 = motion mask ([Debug] nvn_fsr_motionmask, now {}): current input outside the "
                           "previous input's 3x3 colour box at the camera-reprojected texel; reactive = {:.2f} x max of them (capped where a "
                           "geometry object vector is valid)",
                           fsr_motion ? "on" : "off", fsr_react_strength));
        FsrLog(fmt::format("state indicator ([Debug] nvn_fsr_indicator, now {}): 6x6 px squares (black border) in the output's top-left "
                           "corner, left to right: 1 HUD path (green HUD layer, orange old diff composite, grey menu skip: a small grey "
                           "square in H instead, no composite runs), 2 reactive silhouette (green on, dark off), 3 jitter convention "
                           "(red 0, yellow 1, cyan 2, magenta 3), 4 view (white 0 composite, blue 1 FSR only, pink 2 mask tint, black 3 "
                           "game picture), 5 motion mask (green on, dark off), 6 object motion (blue block matching only, white geometry vectors only, cyan both, dark off), 7 animated-surface reactive (magenta on, dark off), 8 FSR backend that ran (green FSR 4, orange FSR 3.1). Not drawn in the frame a Numpad6 dump is taken of",
                           fsr_indicator ? "on" : "off"));
        Fsr4Init();
        FsrModeLine();
    }

    // ---- [Debug] nvn_fsr_backend = 4: FSR 4 via the AMD driver's DX12 ffx loader (see nvn_fsr4.h) -----------------------
    // Only step "2. FSR" of FsrDispatch changes: the inputs FSR 3.1 would get (snapshot, depth, motion vectors, reactive) are
    // copied into the shared images, the frame's command buffer is submitted (signal N), D3D12 runs ffxDispatch (wait N,
    // signal N + 1), the frame continues in the slot's second command buffer, which EndFrame submits waiting on N + 1, and
    // whose first commands copy the FSR 4 output into fsr_out. The composite and everything else are unchanged. Any failure
    // -> FSR 3.1 in the same frame (history reset), FSR 4 retried after 300 frames, off for the run after 3 failures.
    void Fsr4Init() {
        fsr_backend_ini = Core::GameSettings::NvnFsrBackend();
        if (fsr_backend_ini != 4) {
            FsrLog("backend: FSR 3.1.4 ([Debug] nvn_fsr_backend = 3, the default; 4 = FSR 4 through the AMD driver's DX12 loader)");
            return;
        }
#ifndef SUYU_HAS_FSR4_BRIDGE
        fsr4_reason = "this build has no FSR 4 bridge (Windows only)";
        FsrLog("backend: nvn_fsr_backend = 4, but " + fsr4_reason + " -> FSR 3.1.4");
#else
        if (!device.HasFsr4Interop()) {
            fsr4_reason = "the Vulkan device lacks VK_KHR_external_memory_win32 / VK_KHR_external_semaphore_win32";
            FsrLog("backend: nvn_fsr_backend = 4, but " + fsr4_reason + " -> FSR 3.1.4 for this run");
            return;
        }
        const auto& dld = device.GetDispatchLoader();
        NvnFsr4::VkContext c{};
        c.device = *dev;
        c.physical = device.GetPhysical();
        c.get_device_proc_addr = dld.vkGetDeviceProcAddr;
        c.get_physical_properties2 = dld.vkGetPhysicalDeviceProperties2;
        c.get_physical_memory_properties = dld.vkGetPhysicalDeviceMemoryProperties;
        c.ring = kSlots + 1;
        c.debug_checking = fsr_dbg_checking;
        fsr4 = std::make_unique<NvnFsr4::Bridge>();
        if (!fsr4->Init(c, [this](const std::string& s) { FsrLog(s); })) {
            fsr4_reason = "FSR 4 init failed: " + fsr4->LastError();
            FsrLog("backend: " + fsr4_reason + " -> FSR 3.1.4 for this run");
            fsr4.reset();
            return;
        }
        try {
            fsr4_cmd_buffers = pool.Allocate(kSlots);
            for (u32 i = 0; i < kSlots; ++i) fsr4_cmd[i] = fsr4_cmd_buffers[i];
        } catch (const std::exception& e) {
            fsr4_reason = fmt::format("second command buffers: {}", e.what());
            FsrLog("backend: " + fsr4_reason + " -> FSR 3.1.4 for this run");
            fsr4->Shutdown();
            fsr4.reset();
            return;
        }
        fsr4_ok = fsr4_want = true;
        FsrLog(fmt::format("backend: {} through the driver's DX12 loader (Ctrl+Numpad1 switches FSR 4 <-> FSR 3.1.4 at run time, with a "
                           "history reset; ffx debug checking {} ([Debug] nvn_fsr_debug)); FSR 3.1.4 stays loaded as the fallback",
                           fsr4->VersionName(), c.debug_checking ? "on" : "off"));
        FsrLog(fmt::format("FSR 4 jitter: jitterOffset = the same value FSR 3.1 gets (sign convention {}, Numpad3 still cycles it): ffx-api "
                           "defines jitterOffset identically for every upscaler provider ('the subpixel jitter offset applied to the "
                           "camera', render px), and convention 1 is the one verified on FSR 3.1; the camera keeps game_settings' AMD "
                           "Halton sequence (FSR 4's own phase count / offsets are queried and compared when the context is created). "
                           "Colour: create flag NON_LINEAR_COLORSPACE + dispatch flag NON_LINEAR_COLOR_SRGB (post-tonemap gamma bytes)",
                           fsr_sign));
#endif
    }

#ifdef SUYU_HAS_FSR4_BRIDGE
    void Fsr4Failure(const std::string& why) {
        ++fsr4_failures;
        fsr_need_reset = true;
        fsr4_drop_ctx = true;
        fsr4_in_ours = fsr4_out_ours = false;
        if (!fsr4->WaitIdle(1000)) fsr4->ReleaseWaiters(); // no Vulkan submit may hang on a D3D12 value that never comes
        if (fsr4_failures >= 3) {
            fsr4_ok = false;
            fsr4_reason = "3 failures, last: " + why;
        } else {
            fsr4_retry_frame = frame_no + 300;
            fsr4_reason = "failed (retry in 300 frames): " + why;
        }
        FsrLog(fmt::format("frame {}: FSR 4 failure {} of 3: {} -> FSR 3.1.4 with a history reset{}", frame_no, fsr4_failures, why,
                           fsr4_failures >= 3 ? " for the rest of the run" : ", FSR 4 retried in 300 frames"));
        FsrModeLine();
    }

    // The FSR 4 step of FsrDispatch. True = fsr_out holds FSR 4's output and vkcmd is the frame's second command buffer;
    // false = nothing changed for FSR 3.1 (vkcmd may be a new command buffer: everything recorded before was submitted).
    bool Fsr4Run(Image* mv_in, bool react_on, NvnFsr4::DispatchParams p) {
        if (!fsr4 || !fsr4_ok || !fsr4_want) return false;
        // (the caller counts the frames that fall back)
        if (fsr4_retry_frame != 0 && frame_no < fsr4_retry_frame) return false;
        if (end_cmd_override || !fsr_snap || !fsr_depth || !mv_in || !fsr_react || !fsr_out || frames.size() != fsr4_cmd.size()) {
            fsr4_reason = "an input image is missing / the frame was already split";
            return false;
        }
        const u32 rw = p.render_w, rh = p.render_h, ow = p.output_w, oh = p.output_h;
        if (mv_in->width < rw || mv_in->height < rh || fsr_depth->width < rw || fsr_react->width < rw || fsr_out->width != ow ||
            fsr_out->height != oh) {
            fsr4_reason = "input image sizes do not match the render size";
            return false;
        }
        if (fsr4_drop_ctx || fsr4->NeedsRecreate(rw, rh, ow, oh, fsr_snap->format, mv_in->format)) {
            // nothing in flight may still use the old shared images
            WaitAllFrames(PB_WaitDump);
            if (!fsr4->WaitIdle(1000)) fsr4->ReleaseWaiters();
            fsr4->DestroyContext();
            fsr4_drop_ctx = false;
            fsr4_in_ours = fsr4_out_ours = false;
            if (!fsr4->Ensure(rw, rh, ow, oh, fsr_snap->format, mv_in->format)) {
                Fsr4Failure("context / shared images: " + fsr4->LastError());
                return false;
            }
            const int32_t n4 = fsr4->JitterPhaseCount(rw, ow);
            const int32_t n3 = ffxFsr3UpscalerGetJitterPhaseCount(static_cast<int32_t>(rw), static_cast<int32_t>(ow));
            u32 same = 0;
            std::string table;
            for (int32_t i = 0; i < n4; ++i) {
                float x4 = 0.0f, y4 = 0.0f, x3 = 0.0f, y3 = 0.0f;
                fsr4->JitterOffset(i, n4, &x4, &y4);
                ffxFsr3UpscalerGetJitterOffset(&x3, &y3, i, n3 > 0 ? n3 : n4);
                same += std::fabs(x4 - x3) < 1e-5f && std::fabs(y4 - y3) < 1e-5f;
                if (i < 4) table += fmt::format(" [{}] ({:.4f}, {:.4f})", i, x4, y4);
            }
            u32 gi = 0, gn = 0;
            Core::GameSettings::FsrJitterPhase(&gi, &gn);
            FsrLog(fmt::format("frame {}: FSR 4 jitter query: phase count {} (FSR 3.1 helper {}, game_settings camera {}); offsets equal to "
                               "FSR 3.1's table {}/{}:{} ... -> the camera sequence is kept as is",
                               frame_no + 1, n4, n3, gn, same, n4, table));
        }
        const auto& dld = device.GetDispatchLoader();
        const u32 gfx = device.GetGraphicsFamily();
        const bool fresh = fsr4->TakeFreshImages();
        const auto bar = [&](NvnFsr4::Slot s, bool ours, bool to_external, VkAccessFlags sa, VkAccessFlags da) {
            const bool undef = fresh;
            return VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = sa,
                .dstAccessMask = da,
                .oldLayout = undef && !ours ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = undef && !ours ? VK_QUEUE_FAMILY_IGNORED : to_external ? gfx : VK_QUEUE_FAMILY_EXTERNAL,
                .dstQueueFamilyIndex = undef && !ours ? VK_QUEUE_FAMILY_IGNORED : to_external ? VK_QUEUE_FAMILY_EXTERNAL : gfx,
                .image = fsr4->Image(s).image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
        };
        // (a) inputs: acquire from D3D12 (or first use), copy the FSR 3.1 inputs in
        {
            std::array<VkImageMemoryBarrier, 4> acq{};
            u32 na = 0;
            if (!fsr4_in_ours) {
                for (const NvnFsr4::Slot s : {NvnFsr4::kColor, NvnFsr4::kDepth, NvnFsr4::kMotion, NvnFsr4::kReactive}) {
                    acq[na++] = bar(s, false, false, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                }
            }
            Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            if (na) vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, vk::Span<VkImageMemoryBarrier>(acq.data(), na));
            const auto copy = [&](const Image& src, NvnFsr4::Slot s, u32 w, u32 h) {
                const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
                vkcmd.CopyImage(*src.image, VK_IMAGE_LAYOUT_GENERAL, fsr4->Image(s).image, VK_IMAGE_LAYOUT_GENERAL, region);
            };
            copy(*fsr_snap, NvnFsr4::kColor, rw, rh);
            copy(*fsr_depth, NvnFsr4::kDepth, rw, rh);
            copy(*mv_in, NvnFsr4::kMotion, rw, rh);
            if (react_on) copy(*fsr_react, NvnFsr4::kReactive, rw, rh);
            fsr4_in_ours = true;
        }
        // (b) release all five to D3D12 (the output: first use, or owned since the last copy back)
        {
            std::array<VkImageMemoryBarrier, 5> rel{};
            u32 nr = 0;
            for (const NvnFsr4::Slot s : {NvnFsr4::kColor, NvnFsr4::kDepth, NvnFsr4::kMotion, NvnFsr4::kReactive}) {
                rel[nr++] = bar(s, true, true, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            }
            if (fresh) { // UNDEFINED -> GENERAL first (contents do not matter: FSR 4 writes every output texel)
                const VkImageMemoryBarrier init = bar(NvnFsr4::kOutput, false, false, 0, 0);
                vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, init);
                fsr4_out_ours = true;
            }
            if (fsr4_out_ours) rel[nr++] = bar(NvnFsr4::kOutput, true, true, VK_ACCESS_TRANSFER_READ_BIT, 0);
            vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, {}, {}, vk::Span<VkImageMemoryBarrier>(rel.data(), nr));
            fsr4_in_ours = fsr4_out_ours = false;
        }
        // (c) split: submit what the frame recorded so far, signalling v1
        GpMarkNamed("fsr4_input_copies_and_release");
        vkcmd.End();
        const VkCommandBuffer first = *vkcmd;
        const VkSemaphore tl = fsr4->Timeline();
        const u64 v1 = fsr4->NextValue(), v2 = fsr4->NextValue();
        const VkTimelineSemaphoreSubmitInfo tsi{.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .pNext = nullptr, .waitSemaphoreValueCount = 0,
                                                .pWaitSemaphoreValues = nullptr, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v1};
        const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .waitSemaphoreCount = 0, .pWaitSemaphores = nullptr,
                              .pWaitDstStageMask = nullptr, .commandBufferCount = 1, .pCommandBuffers = &first, .signalSemaphoreCount = 1,
                              .pSignalSemaphores = &tl};
        VkResult sr;
        {
            ProfScope ps{prof, PB_Submit};
            sr = queue.Submit(si);
        }
        // the frame continues in the slot's second command buffer (EndFrame submits it)
        vkcmd = vk::CommandBuffer(fsr4_cmd[cur], dld);
        vkcmd.Begin({.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                     .pInheritanceInfo = nullptr});
        end_cmd_override = fsr4_cmd[cur];
        GpMarkNamed("fsr4_dx12_gap_incl_queue_wait");
        if (sr != VK_SUCCESS) {
            Fsr4Failure(fmt::format("vkQueueSubmit of the first part failed ({})", static_cast<int>(sr)));
            return false;
        }
        // (d) D3D12: wait v1, ffxDispatch, signal v2
        p.reset = p.reset || !fsr4_ran_last;
        if (!fsr4->Dispatch(p, v1, v2)) {
            Fsr4Failure(fsr4->LastError());
            return false;
        }
        end_wait_sem = tl;
        end_wait_value = v2;
        // (e) acquire everything back (output read now; inputs stay ours until the next frame's copies) and copy the output
        {
            std::array<VkImageMemoryBarrier, 5> acq{};
            u32 na = 0;
            for (const NvnFsr4::Slot s : {NvnFsr4::kColor, NvnFsr4::kDepth, NvnFsr4::kMotion, NvnFsr4::kReactive, NvnFsr4::kOutput}) {
                acq[na++] = bar(s, true, false, 0, s == NvnFsr4::kOutput ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT);
            }
            vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, vk::Span<VkImageMemoryBarrier>(acq.data(), na));
            fsr4_in_ours = fsr4_out_ours = true;
            Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {ow, oh, 1}};
            vkcmd.CopyImage(fsr4->Image(NvnFsr4::kOutput).image, VK_IMAGE_LAYOUT_GENERAL, *fsr_out->image, VK_IMAGE_LAYOUT_GENERAL, region);
            Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        }
        ++fsr4_sec_frames;
        fsr4_reason.clear();
        return true;
    }
#endif

    // backend name for the mode line / logs
    std::string FsrBackendText() const {
#ifdef SUYU_HAS_FSR4_BRIDGE
        if (fsr4 && fsr4_ok && fsr4_want && fsr4_ran_last) return fsr4->VersionName() + " (DX12 bridge)";
        if (fsr4 && fsr4_ok && !fsr4_want) return "FSR 3.1.4 (FSR 4 available: Ctrl+Numpad1)";
#endif
        if (fsr_backend_ini == 4) return "FSR 3.1.4 (FSR 4 not running: " + (fsr4_reason.empty() ? std::string("no FSR frame yet") : fsr4_reason) + ")";
        return "FSR 3.1.4";
    }

    void FsrFail(const std::string& why) {
        fsr_failed = true;
        fsr_active = false;
        Core::GameSettings::FsrSetActive(false);
        FsrLog("ERROR: " + why + " -> FSR off for this run (the game's own picture is shown)");
        Note("nvn_fsr: " + why);
    }

    void FsrDestroyContext(bool wait) {
        if (!fsr_ctx) return;
        if (wait) WaitAllFrames(PB_WaitDump);
        ffxVkPortableUpscaleContextDestroy(fsr_ctx);
        fsr_ctx = nullptr;
    }

    bool FsrBuildPipe(FsrPipe& p, const std::vector<VkDescriptorType>& types, const u32* code, size_t code_bytes, u32 pc_bytes) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorUpdateTemplateEntry> entries;
        for (u32 i = 0; i < types.size(); ++i) {
            bindings.push_back({i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
            entries.push_back({i, 0, 1, types[i], i * sizeof(VkDescriptorImageInfo), sizeof(VkDescriptorImageInfo)});
        }
        p.set_layout = dev.CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
        const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_bytes};
        p.layout = dev.CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = p.set_layout.address(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pcr,
        });
        p.tmpl = dev.CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
            .descriptorSetLayout = *p.set_layout,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE,
            .pipelineLayout = *p.layout,
            .set = 0,
        });
        p.module = dev.CreateShaderModule({
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = code_bytes,
            .pCode = code,
        });
        p.pipeline = dev.CreateComputePipeline({
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *p.module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            .layout = *p.layout,
            .basePipelineHandle = {},
            .basePipelineIndex = 0,
        });
        return true;
    }

    bool FsrEnsurePipes() {
        if (fsr_pipes_ready) return true;
        try {
            const auto sampler = [&](VkFilter filter) {
                return dev.CreateSampler({
                    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .magFilter = filter,
                    .minFilter = filter,
                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                    .mipLodBias = 0.0f,
                    .anisotropyEnable = VK_FALSE,
                    .maxAnisotropy = 1.0f,
                    .compareEnable = VK_FALSE,
                    .compareOp = VK_COMPARE_OP_ALWAYS,
                    .minLod = 0.0f,
                    .maxLod = 0.0f,
                    .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                    .unnormalizedCoordinates = VK_FALSE,
                });
            };
            fsr_near = sampler(VK_FILTER_NEAREST);
            fsr_linear = sampler(VK_FILTER_LINEAR);
            constexpr VkDescriptorType kTex = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            FsrBuildPipe(fsr_prep, {kTex, kTex, kImg, kImg, kTex, kImg, kTex, kTex, kImg, kTex, kTex, kTex, kTex}, VULKAN_NVN_FSR_PREP_COMP_SPV,
                         sizeof(VULKAN_NVN_FSR_PREP_COMP_SPV), 32);
            FsrBuildPipe(fsr_objs, {kTex, kTex, kTex, kTex, kTex, kTex, kTex, kImg}, VULKAN_NVN_FSR_OBJMV_SEARCH_COMP_SPV,
                         sizeof(VULKAN_NVN_FSR_OBJMV_SEARCH_COMP_SPV), 32);
            FsrBuildPipe(fsr_obja, {kTex, kTex, kTex, kTex, kImg, kTex, kImg, kTex}, VULKAN_NVN_FSR_OBJMV_APPLY_COMP_SPV,
                         sizeof(VULKAN_NVN_FSR_OBJMV_APPLY_COMP_SPV), 32);
            if (fsr_bm_compact) {
                // [Debug] nvn_fsr_bm_compact: a failure here only keeps the one-pass search
                try {
                    constexpr VkDescriptorType kBuf = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    FsrBuildPipe(fsr_objc, {kTex, kTex, kTex, kTex, kTex, kTex, kTex, kImg, kBuf}, VULKAN_NVN_FSR_OBJMV_CLASSIFY_COMP_SPV,
                                 sizeof(VULKAN_NVN_FSR_OBJMV_CLASSIFY_COMP_SPV), 32);
                    FsrBuildPipe(fsr_objl, {kTex, kTex, kTex, kTex, kTex, kTex, kTex, kImg, kBuf}, VULKAN_NVN_FSR_OBJMV_SEARCH_LIST_COMP_SPV,
                                 sizeof(VULKAN_NVN_FSR_OBJMV_SEARCH_LIST_COMP_SPV), 32);
                    fsr_bm_compact_ok = true;
                    FsrLog("block matching: compacted (classify pass + indirect search over the searched blocks only, "
                           "[Debug] nvn_fsr_bm_compact = 1; vulkan_nvn_fsr_objmv_classify.comp / _search_list.comp)");
                } catch (const std::exception& e) {
                    fsr_bm_compact_ok = false;
                    FsrLog(fmt::format("block matching: nvn_fsr_bm_compact pipelines failed ({}): the one-pass search is used", e.what()));
                }
            }
            FsrBuildPipe(fsr_anim_pipe, {kTex, kTex, kTex, kTex, kTex, kImg, kImg, kTex}, VULKAN_NVN_FSR_ANIMSURF_COMP_SPV,
                         sizeof(VULKAN_NVN_FSR_ANIMSURF_COMP_SPV), 32);
            FsrBuildPipe(fsr_compose, {kTex, kTex, kTex, kImg, kTex, kTex, kTex}, VULKAN_NVN_FSR_COMPOSITE_COMP_SPV,
                         sizeof(VULKAN_NVN_FSR_COMPOSITE_COMP_SPV), 32);
            fsr_pipes_ready = true;
            FsrLog("prep, composite and object-motion compute pipelines built (host shaders vulkan_nvn_fsr_prep.comp / "
                   "vulkan_nvn_fsr_composite.comp / vulkan_nvn_fsr_objmv_search.comp / vulkan_nvn_fsr_objmv_apply.comp)");
            return true;
        } catch (const std::exception& e) {
            FsrFail(fmt::format("compute pipelines failed: {}", e.what()));
            return false;
        }
    }

    bool FsrEnsureContext(u32 rw, u32 rh, u32 ow, u32 oh) {
        if (fsr_ctx && fsr_ci.maxRenderSize.width == rw && fsr_ci.maxRenderSize.height == rh && fsr_ci.maxOutputSize.width == ow &&
            fsr_ci.maxOutputSize.height == oh) {
            return true;
        }
        if (fsr_ctx) FsrLog(fmt::format("frame {}: sizes changed, recreating the FSR context", frame_no + 1));
        FsrDestroyContext(true);
        const FfxVkPortableDeviceInfo di = FsrDeviceInfo();
        fsr_ci = {};
        fsr_ci.structSize = sizeof(fsr_ci);
        fsr_ci.flags = 0; // LDR colour (post-tonemap RGBA8), standard depth, render-size unjittered motion vectors
        fsr_ci.maxRenderSize = {rw, rh};
        fsr_ci.maxOutputSize = {ow, oh};
        const double t0 = NvnVkNowMs();
        const FfxVkPortableResult r = ffxVkPortableUpscaleContextCreate(&di, &fsr_ci, &fsr_ctx);
        if (r != FFX_VK_PORTABLE_OK || !fsr_ctx) {
            fsr_ctx = nullptr;
            FsrFail(fmt::format("FSR context {}x{} -> {}x{} could not be created: {}", rw, rh, ow, oh, FsrResultText(r)));
            return false;
        }
        FfxVkPortableMemoryUsage mu{};
        mu.structSize = sizeof(mu);
        ffxVkPortableUpscaleContextGetMemoryUsage(fsr_ctx, &mu);
        fsr_need_reset = true;
        fsr_amd_jitter.clear();
        const int32_t n = ffxFsr3UpscalerGetJitterPhaseCount(static_cast<int32_t>(rw), static_cast<int32_t>(ow));
        std::string table;
        for (int32_t i = 0; i < n; ++i) {
            float x = 0.0f, y = 0.0f;
            ffxFsr3UpscalerGetJitterOffset(&x, &y, i, n);
            fsr_amd_jitter.push_back({x, y});
            if (i < 8) table += fmt::format(" [{}] ({:.4f}, {:.4f})", i, x, y);
        }
        FsrLog(fmt::format("frame {}: FSR context created, render {}x{} -> output {}x{} (scale {:.3f} x {:.3f}), effect memory {:.1f} MB, {:.1f} ms; "
                           "AMD jitter phase count {}:{} ...",
                           frame_no + 1, rw, rh, ow, oh, double(ow) / rw, double(oh) / rh, mu.totalUsageInBytes / 1048576.0, NvnVkNowMs() - t0, n,
                           table));
        return true;
    }

    void FsrEnsureImage(std::unique_ptr<Image>& slot, VkFormat format, u32 w, u32 h, VkImageUsageFlags usage, const char* what) {
        if (slot && slot->width == w && slot->height == h && slot->format == format) return;
        if (slot) graveyard.push_back({serial, std::move(slot)}); // earlier submits may still use it
        auto img = std::make_unique<Image>();
        img->format = format;
        img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        img->width = w;
        img->height = h;
        img->image = allocator.CreateImage({
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        img->view = dev.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *img->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components = {},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        });
        // the callers ended the render pass: UNDEFINED -> GENERAL right here, before the first use
        const VkImageMemoryBarrier to_general{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *img->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkcmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, to_general);
        slot = std::move(img);
        FsrLog(fmt::format("frame {}: {} image {}x{} (VkFormat {}) created", frame_no + 1, what, w, h, static_cast<int>(format)));
    }

    // FsrBuildPipe's templates read one 24-byte record per binding: an image or (storage buffer bindings) a buffer info
    union FsrDesc {
        VkDescriptorImageInfo img;
        VkDescriptorBufferInfo buf;
    };
    static_assert(sizeof(VkDescriptorImageInfo) == sizeof(VkDescriptorBufferInfo) && sizeof(FsrDesc) == sizeof(VkDescriptorImageInfo));

    // nvn_fsr_bm_compact: the block list (dispatch args + one entry per block). Grows only (resolution change: waits for the
    // frames in flight, which may still read the old one).
    bool FsrEnsureBmList(u32 blocks) {
        const VkDeviceSize bytes = 16 + VkDeviceSize{blocks} * 4;
        if (fsr_bm_list && fsr_bm_list_bytes >= bytes) return true;
        try {
            if (fsr_bm_list) WaitAllFrames(PB_WaitDump);
            fsr_bm_list = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0, .size = bytes,
                                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                  .pQueueFamilyIndices = nullptr},
                                                 MemoryUsage::DeviceLocal);
            fsr_bm_list_bytes = bytes;
            FsrLog(fmt::format("frame {}: block list buffer for {} blocks created (nvn_fsr_bm_compact)", frame_no + 1, blocks));
            return true;
        } catch (const std::exception& e) {
            fsr_bm_compact_ok = false;
            FsrLog(fmt::format("block matching: block list buffer failed ({}): the one-pass search is used", e.what()));
            return false;
        }
    }

    static FfxVkPortableImage FsrImg(const Image* img) {
        FfxVkPortableImage o{};
        o.structSize = sizeof(o);
        if (!img) return o;
        o.image = *img->image;
        o.format = img->format;
        o.extent = {img->pw ? img->pw : img->width, img->ph ? img->ph : img->height};
        o.mipCount = 1;
        o.arrayLayers = 1;
        // what the images were created with (FsrEnsureImage / MvEnsureImage)
        o.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        o.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        o.state = FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS; // = VK_IMAGE_LAYOUT_GENERAL in the backend
        return o;
    }

    // Draw facts FsrOnDraw classifies with (DoDraw fills them).
    struct FsrDrawInfo {
        bool indexed;
        u32 topology;
        u32 count;      // vertex count (plain draws) / index count (indexed draws)
        u32 blend;      // RT0 blending enabled
        u64 fs;         // fragment program (logs)
        const PipelineKey* key; // the draw's pipeline key (HUD layer blend classification)
    };
    // One draw that writes H or reads H, for the per-frame draw list in nvn_fsr.txt.
    struct FsrDrawRec {
        u32 idx, rt, count, topology;
        bool indexed, blend, into_h, post, reads_h, layer;
        u64 fs;
        std::array<u32, 3> rt_keys; // render targets it reads (RtKey), 0 = none
        std::array<u16, 3> rt_w, rt_h;
        char snap; // 0, or 'S' when the snapshot was copied right before this draw
    };
    static constexpr u32 kFsrDrawRecs = 48;

    static bool FsrIsColorRt(const Image* s) {
        return s && s->render_target && s->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    }

    // Every draw while nvn_fsr is on (DoDraw, after the textures are known, before the descriptors are written).
    // sampled = every set texture slot (stale bindings included), used = the slots the draw's program reads.
    //
    // Where the snapshot goes (the finished scene without HUD). Measured (upscaler probe, 149 gameplay frames, always
    // the same shape; H = the image the final draw samples, S = 743b): S <- draw H0-1; H <- draw H0; S <- draw H0+1
    // (reads H); H <- H0+2 ... (HUD). The old rule (copy at H0+1, the first draw that reads H) gave a snapshot that
    // differs from the final H on 99.9% of pixels (48% by >= 6/255: bloom-like glow + a global tone change, i.e. a
    // full-screen pass into H after H0+1), so the HUD mask covered most of the screen. Viewport/scissor cannot tell
    // the passes apart: 21.2M of the run's 1920x1080 draws (HUD included) use the full viewport and scissor.
    // Rule now: a draw into H is a "post pass" when it is a small plain/indexed draw (<= 6 vertices: a full-screen
    // quad/triangle) that reads a colour render target other than H (the scene, S, the bloom chain); HUD sprites read
    // ordinary textures. The post section is the run of post passes into H from H's first draw of the frame; the
    // snapshot is copied right before the first non-post draw into H after it (or before the final draw when no
    // HUD draw follows). If H's first draw is not a post pass, the old rule is the fallback. [Debug]
    // nvn_fsr_snapdraw = N overrides all of this: copy right after draw N (before draw N + 1 / the final draw).
    void FsrOnDraw(Core::Memory::Memory& mem, const std::array<Image*, 16>& sampled, const std::array<Image*, 16>& used,
                   const FsrDrawInfo& di) {
        const u64 f = frame_no + 1;
        if (fsr_frame != f) {
            fsr_frame = f;
            fsr_snap_ok = false;
            fsr_draw_idx = 0;
            fsr_src_draws = 0;
            fsr_mask_img = nullptr;
            fsr_final_src = nullptr;
            fsr_final_seen = false;
            fsr_h_img = nullptr;
            fsr_h_first = fsr_h_last_post = ~0u;
            fsr_h_posts = 0;
            fsr_h_run_open = true;
            fsr_snap_pending = false;
            fsr_snap_why = 0;
            fsr_recs_n = 0;
            fsr_tsum_n = 0;
            fsr_mesh_last = 0;
            fsr_frame_dodraws0 = stats.draws;
            if (fsr_layer_on) {
                // the last frame had no final draw: its HUD layer is dropped
                EndPass();
                pass_dirty = true;
            }
            fsr_layer_on = fsr_layer_used = fsr_layer_aborted = false;
            fsr_tm_used = false;
            fsr_layer_h = nullptr;
            fsr_layer_draws = 0;
        }
        const u32 idx = fsr_draw_idx++;
        // per-frame target summary (menu-skip diagnostics): draws per colour / depth target of this frame. Log only
        // ("menu-check targets"): debug hooks only
        if (fsr_dbg) {
            const u32 ck = st.rt_count > 0 && st.rt_tex[0] ? RtKeyFrame(mem, st.rt_tex[0]) : 0;
            const u32 dk = st.depth_tex ? RtKeyFrame(mem, st.depth_tex) : 0;
            bool found = false;
            for (u32 i = 0; i < fsr_tsum_n; ++i) {
                if (fsr_tsum[i].ck == ck && fsr_tsum[i].dk == dk) {
                    ++fsr_tsum[i].n;
                    found = true;
                    break;
                }
            }
            if (!found && fsr_tsum_n < fsr_tsum.size()) {
                FsrTargetSum& t = fsr_tsum[fsr_tsum_n++];
                t = {};
                t.ck = ck;
                t.dk = dk;
                t.n = 1;
                t.first = idx;
                if (Image* c = ck ? RenderTarget(mem, st.rt_tex[0], false) : nullptr) t.cw = c->width, t.ch = c->height;
                if (Image* d = dk ? RenderTarget(mem, st.depth_tex, true) : nullptr) t.dw = d->width, t.dh = d->height;
            }
        }
        fsr_sub_now = false;
        fsr_redirect_now = false;
        fsr_final_now = false;
        fsr_hud_pipe = fsr_hud_pipe2 = VK_NULL_HANDLE;
        const bool live = fsr_active && !fsr_failed;
        // forced point: right after draw N, i.e. before the first later draw of the frame (any target)
        if (live && fsr_snapdraw > 0 && !fsr_snap_ok && idx > static_cast<u32>(fsr_snapdraw) && fsr_h_img) {
            FsrTakeSnapshot(*fsr_h_img, idx, 4);
            FsrLayerBegin(*fsr_h_img, idx);
        }
        // HUD layer: a draw (other than the final draw) that reads H, or binds H next to other targets, would see /
        // write H without the HUD drawn so far: put the layer back into H and use the old path for the rest of the frame
        if (fsr_layer_on) {
            bool rd = false;
            for (Image* s : used) rd |= s && fsr_src_key != 0 && s->cpu == fsr_src_key;
            const bool final_draw = st.rt_count > 0 && st.rt_tex[0] != 0 && !fsr_final_seen && mv_present_keys.contains(RtKeyFrame(mem, st.rt_tex[0]));
            if (rd && !final_draw) {
                FsrLayerAbort(fmt::format("draw {} reads H while the HUD goes to the layer (target {:x})", idx,
                                          st.rt_count > 0 && st.rt_tex[0] ? RtKeyFrame(mem, st.rt_tex[0]) : 0u));
            }
            for (u32 i = 1; fsr_layer_on && i < st.rt_count; ++i) {
                if (st.rt_tex[i] && RtKeyFrame(mem, st.rt_tex[i]) == fsr_src_key) {
                    FsrLayerAbort(fmt::format("draw {} binds H as colour target {} of {}", idx, i, st.rt_count));
                }
            }
        }
        if (st.rt_count == 0 || st.rt_tex[0] == 0) return;
        const u32 rt0 = RtKeyFrame(mem, st.rt_tex[0]);
        if (rt0 == 0) return;
        bool reads_h = false;
        for (Image* s : used) reads_h |= s && fsr_src_key != 0 && s->cpu == fsr_src_key;
        if (mv_present_keys.contains(rt0)) {
            if (fsr_final_seen) return; // only the first draw into the present image is the final draw
            fsr_final_seen = true;
            fsr_final_now = true;
            if (fsr_layer_on) {
                fsr_layer_on = false; // redirect ends here; the layer is composited (FsrDispatch) or merged (FsrFinalLayer)
                pass_dirty = true;
            }
            // the image the final draw upscales: the LARGEST colour render target it samples (the camp's final draw also
            // binds a 256x1 colour-grading target first; taking the first one made that 256x1 target 'H' and FSR stopped)
            Image* src = nullptr;
            for (Image* s : sampled) {
                if (FsrIsColorRt(s) && s->cpu != rt0 && s->width >= 64 && s->height >= 64 &&
                    (!src || u64{s->width} * s->height > u64{src->width} * src->height)) {
                    src = s;
                }
            }
            if (src && src->cpu != fsr_src_key) {
                FsrLog(fmt::format("frame {}: the final draw samples render target {:x} ({}x{}, was {:x}): FSR source / HUD image", f, src->cpu,
                                   src->width, src->height, fsr_src_key));
                fsr_src_key = src->cpu;
                fsr_src_w = src->width;
                fsr_src_h = src->height;
                fsr_need_reset = true;
            }
            fsr_final_src = src;
            FsrMenuUpdate(f);
            if (live && src && !fsr_snap_ok && src->cpu == fsr_src_key) {
                if (fsr_snapdraw > 0) {
                    FsrTakeSnapshot(*src, idx, 5); // N is at or past the final draw: copy here
                } else if (fsr_snap_pending) {
                    FsrTakeSnapshot(*src, idx, 2); // post section ran to the end: no HUD this frame
                }
            }
            FsrRecord(idx, rt0, di, used, false, false, true);
            if (live) {
                if (!src) {
                    ++fsr_skip_nosrc;
                } else if (!fsr_snap_ok) {
                    ++fsr_skip_nosnap;
                } else if (!fsr_menu) {
                    fsr_sub_now = true;
                }
            }
            return;
        }
        if (rt0 == fsr_src_key) {
            Image* h = RenderTarget(mem, st.rt_tex[0], false);
            if (h) fsr_h_img = h;
            ++fsr_src_draws;
            if (fsr_h_first == ~0u) fsr_h_first = idx;
            bool post = false;
            if (di.count <= 6) {
                for (Image* s : used) post |= FsrIsColorRt(s) && s->cpu != fsr_src_key;
            }
            if (!post && di.indexed && di.count >= 64 && !di.blend) fsr_mesh_last = idx; // opaque 3D mesh drawn into H
            // menu 3D (still frames, [Debug] nvn_fsr_menu3d): the last frame had no scene pass and drew opaque meshes into
            // H: keep them in the FSR input, snapshot right after the last one (learned from that frame)
            const bool menu3d = live && fsr_snapdraw == 0 && fsr_menu3d_on && fsr_menu3d_draw != 0 && !fsr_snap_ok;
            if (menu3d && idx > fsr_menu3d_draw && h) {
                FsrTakeSnapshot(*h, idx, 6);
                fsr_snap_pending = false;
                fsr_h_run_open = false;
                FsrLayerBegin(*h, idx);
            }
            if (live && fsr_snapdraw == 0 && fsr_h_run_open && !menu3d) {
                if (post) {
                    ++fsr_h_posts;
                    fsr_h_last_post = idx;
                    fsr_snap_pending = true;
                } else {
                    fsr_h_run_open = false;
                    if (fsr_snap_pending && h) {
                        FsrTakeSnapshot(*h, idx, 1);
                        fsr_snap_pending = false;
                        FsrLayerBegin(*h, idx);
                    }
                }
            }
            if (fsr_layer_on && h == fsr_layer_h && di.key && di.key->depth_format && fsr_layer &&
                fsr_layer->width != (h->pw ? h->pw : h->width)) {
                // split scale: the 1X layer cannot share a framebuffer with H's render-scale depth buffer
                SplitNote("layer depth", "a HUD draw into H uses a depth buffer: HUD layer abandoned for such frames (HUD at the render "
                                         "size, upscaled)");
                FsrLayerAbort(fmt::format("draw {} uses a depth buffer (render scale) while the HUD layer is 1X (split scale)", idx));
            }
            if (fsr_layer_on && h == fsr_layer_h) {
                const u32 cls = di.key ? FsrHudBlendClass(*di.key) : 0;
                if (cls == 0) {
                    FsrLayerAbort(fmt::format("draw {}: blend {} factors colour {}/{} alpha {}/{} mask {:x} cannot be put into the layer", idx,
                                              di.key && (di.key->blend_enable & 1) ? "on" : "off", di.key ? di.key->blend[0][0] : 0,
                                              di.key ? di.key->blend[0][1] : 0, di.key ? di.key->blend[0][2] : 0, di.key ? di.key->blend[0][3] : 0,
                                              di.key ? FsrRt0Mask(*di.key) : 0));
                } else {
                    fsr_redirect_now = true;
                    fsr_redirect_class = cls;
                    bool rt = false;
                    for (Image* s : used) rt |= FsrIsColorRt(s);
                    if (rt) ++fsr_sec_rt_reads; // a HUD draw that samples another render target (just a texture for the layer)
                }
            }
            FsrRecord(idx, rt0, di, used, true, post, reads_h);
            return;
        }
        if (reads_h) FsrRecord(idx, rt0, di, used, false, false, true);
        if (st.depth_tex) {
            // scene pass size (menu detection): render-size colour + depth draws outside H and the present image
            Image* sc = RenderTarget(mem, st.rt_tex[0], false);
            Image* sd = RenderTarget(mem, st.depth_tex, true);
            if (sc && sd && sc->width == sd->width && sc->height == sd->height && sd->width >= 320) {
                if (fsr_scene_draws_frame != f) {
                    fsr_scene_draws_frame = f;
                    fsr_scene_draws = 0;
                }
                ++fsr_scene_draws;
            }
        }
        if (!fsr_mask_img && st.depth_tex) {
            // reactive mask source: the first render-size colour target drawn with the scene depth this frame (probe:
            // the dynamic-object mask, drawn right after the depth-only pass). Same size as H when H is known: the
            // half-size 960x540 target drawn with its own 960x540 depth (bloom/DOF chain, town frames) was picked
            // before in frames without the mask pass.
            Image* c = RenderTarget(mem, st.rt_tex[0], false);
            Image* d = RenderTarget(mem, st.depth_tex, true);
            if (c && d && c->width == d->width && c->height == d->height && d->width >= 320 && c->format == kRtColorFormat &&
                c->cpu != fsr_src_key && (fsr_src_w == 0 || (c->width == fsr_src_w && c->height == fsr_src_h))) {
                fsr_mask_img = c;
                fsr_mask_key = c->cpu;
                fsr_mask_draw = idx;
            }
        }
        // fallback (the old rule): H's first draw of the frame was not a post pass -> copy before the first draw that
        // reads H after H was drawn into
        if (!live || fsr_snapdraw > 0 || fsr_snap_ok || fsr_snap_pending || fsr_h_posts > 0 || fsr_src_key == 0 || fsr_src_draws == 0) return;
        for (Image* s : sampled) {
            if (s && s->cpu == fsr_src_key) {
                FsrTakeSnapshot(*s, idx, 3);
                return;
            }
        }
    }

    // why: 1 auto (before the first non-post draw into H), 2 auto (at the final draw, no HUD draw after the post
    // section), 3 fallback (old rule), 4 forced by nvn_fsr_snapdraw, 5 forced N at/after the final draw.
    static const char* FsrSnapWhyText(u32 why) {
        switch (why) {
        case 1: return "auto: right after the last post pass into H, before the first HUD (non-post) draw into H";
        case 2: return "auto: after the last post pass into H, at the final draw (no HUD draw into H this frame)";
        case 3: return "fallback (old rule): H's first draw is not a post pass; before the first draw that reads H";
        case 4: return "forced by [Debug] nvn_fsr_snapdraw";
        case 5: return "forced by [Debug] nvn_fsr_snapdraw (N at/after the final draw: copied at the final draw)";
        case 6: return "menu 3D: right after the last opaque mesh drawn into H (still frame, [Debug] nvn_fsr_menu3d)";
        default: return "none";
        }
    }

    void FsrTakeSnapshot(Image& h, u32 idx, u32 why) {
        FsrSnapshot(h);
        fsr_snap_ok = true;
        fsr_snap_draw = idx;
        fsr_snap_src_draws = fsr_src_draws;
        fsr_snap_why = why;
        fsr_snap_post = fsr_h_last_post;
        fsr_snap_posts = fsr_h_posts;
        fsr_snap_hfirst = fsr_h_first;
        fsr_snap_mark = idx; // the copy is recorded before this draw; FsrRecord marks it in the draw list
    }

    // Menu skip. Signal (no readback): a frame is a SCENE frame only when (a) a depth+colour scene draw of the same size
    // ran in it (mv_depth_frame, set by MvBeforeDraw; mv_depth itself persists across frames, so it is not enough) AND
    // (b) the main-camera projection hook produced the camera record for this present (MvCamFor(f - lag)). Menu screens
    // (e.g. Character Select, drawn entirely in the HUD pass) lack at least one of them. >= 3 menu frames in a row start
    // the skip (no FSR dispatch/composite, final draw samples the game image, jitter off); the first scene frame ends it
    // with a history reset. Called once per frame at the final draw.
    void FsrMenuUpdate(u64 f) {
        Core::GameSettings::MvCam cam{};
        const int lag = Core::GameSettings::NvnMvLag();
        // a scene frame also needs a real scene pass: >= kMinSceneDraws colour+depth draws of render size outside H (the
        // save / loading screens draw 1 such draw per frame with the camera hook running: FSR + jitter there shivered)
        constexpr u32 kMinSceneDraws = 4;
        const u32 scene_draws = fsr_scene_draws_frame == f ? fsr_scene_draws : 0;
        const bool depth = mv_depth_frame == f;
        const bool enough = scene_draws >= kMinSceneDraws;
        const bool hook = f > static_cast<u64>(lag) && Core::GameSettings::MvCamFor(f - lag, &cam);
        if (fsr_dbg && !(depth && hook && enough)) { // the "menu-check targets" log line: debug hooks only
            std::string t = fmt::format("frame {} menu-check targets (draws reaching the renderer {}, draws recorded incl. skipped {}; "
                                        "scene depth {} camera hook {} scene draws outside H {}):",
                                        f, fsr_draw_idx, stats.draws - fsr_frame_dodraws0, depth ? "yes" : "no", hook ? "yes" : "no", scene_draws);
            for (u32 i = 0; i < fsr_tsum_n; ++i) {
                const FsrTargetSum& e = fsr_tsum[i];
                t += fmt::format(" [colour {:x} {}x{} depth {:x} {}x{}: {} draws from #{}{}]", e.ck, e.cw, e.ch, e.dk, e.dw, e.dh, e.n, e.first,
                                 e.ck != 0 && e.ck == fsr_src_key ? " = H" : "");
            }
            fsr_menu_targets = std::move(t);
        }
        const bool is_scene = depth && hook && enough;
        fsr_scene_now = is_scene;
        fsr_still_run = is_scene ? 0 : std::min<u32>(fsr_still_run + 1, 1000000);
        // menu 3D: next frame's snapshot point when this frame had no scene pass and drew opaque meshes into H
        const u32 m3d = !is_scene && fsr_menu3d_on ? fsr_mesh_last : 0;
        if ((m3d != 0) != (fsr_menu3d_draw != 0) && NvnVkNowMs() - fsr_m3d_log_ms >= 2000.0) {
            fsr_m3d_log_ms = NvnVkNowMs();
            FsrLog(fmt::format("frame {}: menu 3D {} (opaque meshes into H up to draw {}: FSR input with camera jitter)", f, m3d ? "on" : "off", m3d));
        }
        fsr_menu3d_draw = m3d;
        // the jitter the camera hook really applied to this frame's projection (0 when the jitter was off), for still frames
        // whose live draws used the main camera
        fsr_still_hook = hook;
        fsr_still_jit[0] = hook ? cam.jitter_px[0] : 0.0f;
        fsr_still_jit[1] = hook ? cam.jitter_px[1] : 0.0f;
        fsr_still_has_scene = depth; // a still frame that still shows a scene image (with the jitter of its last scene frame)
        if (!fsr_menuskip) {
            // one state: FSR runs on every frame; frames without a scene pass get still-picture inputs (FsrDispatch). A
            // change between pure 2D (no scene depth) and scene pictures resets the history (loading screens, cuts).
            const u32 kind = is_scene ? 2u : depth ? 1u : 0u;
            if (kind != fsr_still_kind) {
                if ((kind == 0) != (fsr_still_kind == 0)) fsr_need_reset = true;
                if (NvnVkNowMs() - fsr_still_log_ms >= 2000.0) {
                    fsr_still_log_ms = NvnVkNowMs();
                    FsrLog(fmt::format("frame {}: frame kind {} -> {} ({}; scene depth drawn {}, camera hook ran {}, scene draws outside H {}, "
                                       "draws {}){}",
                                       f, fsr_still_kind, kind,
                                       kind == 2 ? "scene: camera vectors, live jitter"
                                       : kind == 1 ? "still scene picture: zero motion, jitter of the last scene frame"
                                                   : "pure 2D: zero motion, no jitter",
                                       depth ? "yes" : "no", hook ? "yes" : "no", scene_draws, fsr_draw_idx,
                                       (kind == 0) != (fsr_still_kind == 0) ? ", history reset" : ""));
                }
                fsr_still_kind = kind;
            }
            fsr_menu = false;
            fsr_menu_run = 0;
            return;
        }
        if (is_scene) {
            fsr_menu_run = 0;
            if (fsr_menu) {
                fsr_menu = false;
                fsr_need_reset = true;
                FsrLog(fmt::format("frame {}: scene frame again (scene depth drawn, camera hook ran) -> FSR resumes with reset, jitter back on", f));
            }
        } else if (++fsr_menu_run >= 3 && !fsr_menu) {
            fsr_menu = true;
            FsrLog(fmt::format("frame {}: menu frame #{} (reason: scene depth drawn {}, camera hook ran {}, scene draws outside H {} (need >= {})) "
                               "-> FSR skipped, game picture, jitter off",
                               f, fsr_menu_run, depth ? "yes" : "no", hook ? "yes" : "no", scene_draws, kMinSceneDraws)
                   + fmt::format(" [draws in this frame so far {}: the game re-draws no 3D scene (paused / menu over a still "
                                 "image); FSR + jitter on a still image is what shivered]", fsr_draw_idx));
        }
    }

    void FsrRecord(u32 idx, u32 rt, const FsrDrawInfo& di, const std::array<Image*, 16>& used, bool into_h, bool post, bool reads_h) {
        if (!fsr_dbg || fsr_recs_n >= kFsrDrawRecs) return; // the draw list is a log / dump aid: debug hooks only
        FsrDrawRec& r = fsr_recs[fsr_recs_n++];
        r = {};
        r.idx = idx;
        r.rt = rt;
        r.count = di.count;
        r.topology = di.topology;
        r.indexed = di.indexed;
        r.blend = di.blend != 0;
        r.into_h = into_h;
        r.post = post;
        r.reads_h = reads_h;
        r.layer = fsr_redirect_now;
        r.fs = di.fs;
        u32 n = 0;
        for (Image* s : used) {
            if (!FsrIsColorRt(s) || n >= 3) continue;
            bool dup = false;
            for (u32 i = 0; i < n; ++i) dup |= r.rt_keys[i] == s->cpu;
            if (dup) continue;
            r.rt_keys[n] = s->cpu;
            r.rt_w[n] = static_cast<u16>(s->width);
            r.rt_h[n] = static_cast<u16>(s->height);
            ++n;
        }
        r.snap = fsr_snap_ok && fsr_snap_mark == idx ? 'S' : 0;
    }

    // The draw list of the last frame that took a snapshot (draws into H and draws that read H).
    std::string FsrDrawListText() const {
        std::string t = fmt::format("draws writing/reading H {:x} in frame {} (S = snapshot copied right before this draw; P = post pass; "
                                    "n = vertex/index count; rt = render targets the draw reads):",
                                    fsr_src_key, fsr_list_frame);
        for (u32 i = 0; i < fsr_list_n; ++i) {
            const FsrDrawRec& r = fsr_list[i];
            std::string rts;
            for (u32 k = 0; k < 3; ++k) {
                if (r.rt_keys[k]) rts += fmt::format("{}{:x}({}x{})", rts.empty() ? "" : ",", r.rt_keys[k], r.rt_w[k], r.rt_h[k]);
            }
            t += fmt::format("\n  {}{} {} -> {}{} n {}{} topo {} blend {} fs {:x} rt [{}]", r.snap ? "S " : "  ", r.idx,
                             r.into_h ? (r.post ? "P" : "-") : " ",
                             r.into_h ? (r.layer ? "HUD layer" : "H") : fmt::format("{:x}", r.rt), r.reads_h ? " (reads H)" : "",
                             r.count, r.indexed ? " idx" : "", r.topology, r.blend ? "on" : "off", r.fs, rts);
        }
        if (fsr_list_n >= kFsrDrawRecs) t += "\n  ... (list full)";
        return t;
    }

    // Copies H at the snapshot point (see FsrOnDraw): the finished scene without HUD (FSR colour input).
    void FsrSnapshot(Image& src) {
        EndPass();
        const u32 w = src.pw ? src.pw : src.width, h = src.ph ? src.ph : src.height;
        FsrEnsureImage(fsr_snap, src.format, w, h,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       "snapshot (FSR colour input)");
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkcmd.CopyImage(*src.image, VK_IMAGE_LAYOUT_GENERAL, *fsr_snap->image, VK_IMAGE_LAYOUT_GENERAL, region);
        Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }

    // ---- HUD layer ([Debug] nvn_fsr_hudlayer, Numpad7) ----------------------------------------------------------------
    // From the snapshot point (auto rule 1 or the forced nvn_fsr_snapdraw point) to the final draw, every draw into H goes
    // to fsr_layer instead (BeginPass swaps H's attachment view; same format and size, so the game's render pass and
    // pipelines stay compatible), cleared to 0 when the redirect starts. Each such draw uses a variant of its pipeline
    // (fsr_hud_pipes, keyed by the game's pipeline key): colour blend unchanged (SRC_ALPHA / ONE_MINUS_SRC_ALPHA against
    // a 0 destination accumulates premultiplied colour), alpha = coverage. Then H holds the scene WITHOUT HUD at the
    // final draw, and the composite is layer.rgb + (1 - layer.a) * fsr. Anything the layer cannot represent (a draw that
    // reads H, H in an MRT set, destination-dependent / per-channel blend, a partial colour mask) ends the redirect for
    // the frame: the layer is merged back into H (FsrHudMerge) and the frame takes the old diff path.

    static u32 FsrRt0Mask(const PipelineKey& k) {
        u32 m = 0;
        for (u32 c = 0; c < 4; ++c) {
            if ((k.chan_mask >> (c * 8)) & 1) m |= 1u << c;
        }
        return m;
    }

    // 0 = cannot be put into the layer; 1 = over (dst colour factor ONE_MINUS_SRC_ALPHA: A' = a + A(1 - a));
    // 2 = additive (dst ONE: A unchanged) or no colour write; 3 = opaque / replace (blend off or dst ZERO: A' = 1 via
    // alphaToOne; not when the source factor uses the source alpha, alphaToOne would change the colour too).
    u32 FsrHudBlendClass(const PipelineKey& k) const {
        const u32 rgb = FsrRt0Mask(k) & 7;
        if (rgb == 0) return 2;
        if (rgb != 7) return 0; // per-channel colour mask: one coverage value cannot describe it
        // without alphaToOne (AMD on Windows) opaque draws are class 4: the draw, then an alpha-only logic-op pass
        // (class 5 variant, DoDraw) that sets the coverage to 1 where it drew
        const bool a1 = device.SupportsAlphaToOne();
        if (!(k.blend_enable & 1)) return a1 ? 3 : 4;
        const VkBlendFactor sf = Factor(k.blend[0][0]), df = Factor(k.blend[0][1]);
        const auto dst_dep = [](VkBlendFactor f) {
            return f == VK_BLEND_FACTOR_DST_ALPHA || f == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA || f == VK_BLEND_FACTOR_DST_COLOR ||
                   f == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR || f == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        };
        // 6 = multiply: out = src * (DST_COLOR or 0) + dst * df with df independent of dst, i.e. linear in dst without a
        // constant term (a tint / darkening of what is below). Drawn into the layer with the game's colour blend (scales
        // the HUD drawn so far, alpha unchanged) and once more into the transmittance image (scales the scene).
        if ((sf == VK_BLEND_FACTOR_DST_COLOR || sf == VK_BLEND_FACTOR_ZERO) && !dst_dep(df) &&
            !(sf == VK_BLEND_FACTOR_ZERO && df == VK_BLEND_FACTOR_ZERO)) {
            return 6;
        }
        if (dst_dep(sf) || dst_dep(df)) return 0;
        if (df == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) return 1;
        if (df == VK_BLEND_FACTOR_ONE) return 2;
        if (df == VK_BLEND_FACTOR_ZERO) {
            if (!a1) return 4; // the alpha pass does not touch the colour, so source-alpha factors are fine here
            if (sf == VK_BLEND_FACTOR_SRC_ALPHA || sf == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) return 0;
            return 3;
        }
        return 0; // per-channel (SRC_COLOR) or constant destination factors
    }

    VkPipeline FsrHudPipeline(const PipelineKey& key, const Pipeline& pl, u32 cls) {
        if (const auto it = fsr_hud_pipes.find(key); it != fsr_hud_pipes.end()) return *it->second;
        vk::Pipeline p;
        std::string err;
        try {
            auto in = std::make_unique<PipeInputs>(); // heap, as in GetPipeline
            PipelineInputs(key, *in);
            p = CreatePipelineObject(key, *pl.program, in->attributes, RenderPass(key), cls);
        } catch (const std::exception& e) {
            err = e.what();
        }
        const VkPipeline raw = *p;
        ++fsr_hud_variants;
        const std::string combo = fmt::format("class {} ({}) blend {} colour {}/{} game alpha {}/{} mask {:x}", cls,
                                              cls == 1   ? "over"
                                              : cls == 2 ? "additive/no colour"
                                              : cls == 3 ? "opaque, alphaToOne"
                                              : cls == 6 ? "multiply (+ transmittance pass)"
                                                         : "opaque, + alpha logic-op pass",
                                              (key.blend_enable & 1) ? "on" : "off", key.blend[0][0], key.blend[0][1], key.blend[0][2],
                                              key.blend[0][3], FsrRt0Mask(key));
        // the first use of a variant is logged; a failure is logged once per distinct error text, not on every retry
        if (fsr_hud_combos[combo]++ == 0 || (!err.empty() && fsr_hud_errs.insert(err).second)) {
            FsrLog(fmt::format("frame {}: HUD layer pipeline variant #{} (fs {:x}): {} (NVN factor codes: 1 zero 2 one 5 src_alpha 6 "
                               "one_minus_src_alpha){}",
                               frame_no + 1, fsr_hud_variants, key.prog[1], combo, err.empty() ? "" : " FAILED: " + err));
        }
        fsr_hud_pipes.emplace(key, std::move(p));
        return raw;
    }

    // Multiply HUD draws (class 6): the transmittance image (H size, RGBA8), cleared to 1 at the frame's first one.
    bool FsrTmPrepare() {
        if (!fsr_layer_h || !fsr_layer) return false;
        if (fsr_tm_used && fsr_tm) return true;
        EndPass();
        const u32 w = fsr_layer->width, h = fsr_layer->height;
        FsrEnsureImage(fsr_tm, fsr_layer->format, w, h,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                       "HUD multiply transmittance (RGBA8)");
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue one{};
        one.float32[0] = one.float32[1] = one.float32[2] = one.float32[3] = 1.0f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkcmd.ClearColorImage(*fsr_tm->image, VK_IMAGE_LAYOUT_GENERAL, one, range);
        Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        fsr_tm_used = true;
        pass_dirty = true;
        return true;
    }

    // Class 4 (opaque without alphaToOne): the second pipeline of the pair, alpha-only logic op SET.
    VkPipeline FsrHudAlphaPipeline(const PipelineKey& key, const Pipeline& pl) {
        if (const auto it = fsr_hud_pipes_a.find(key); it != fsr_hud_pipes_a.end()) return *it->second;
        vk::Pipeline p;
        try {
            auto in = std::make_unique<PipeInputs>();
            PipelineInputs(key, *in);
            p = CreatePipelineObject(key, *pl.program, in->attributes, RenderPass(key), 5);
        } catch (const std::exception& e) {
            FsrLog(fmt::format("frame {}: HUD layer alpha-pass pipeline (fs {:x}) FAILED: {}", frame_no + 1, key.prog[1], e.what()));
        }
        const VkPipeline raw = *p;
        fsr_hud_pipes_a.emplace(key, std::move(p));
        return raw;
    }

    void FsrLayerBegin(Image& h, u32 idx) {
        if (!fsr_hudlayer || fsr_view == 3 || fsr_menu || fsr_layer_on || fsr_layer_aborted) return;
        if (h.format != kRtColorFormat || h.aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            FsrLayerNote(fmt::format("H has VkFormat {} (expected RGBA8): no HUD layer", static_cast<int>(h.format)));
            return;
        }
        EndPass();
        u32 w = h.pw ? h.pw : h.width, hh = h.ph ? h.ph : h.height;
        if (SplitNow()) {
            // split scale: the HUD is drawn at the output (1X = H's size in the game's units), above FSR's 1X picture
            w = h.width;
            hh = h.height;
        }
        const Image* before = fsr_layer.get();
        FsrEnsureImage(fsr_layer, h.format, w, hh,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       "HUD layer (premultiplied RGBA8)");
        if (before && fsr_layer.get() != before) {
            // a framebuffer of the old layer view must not be found again under a reused handle value
            fb_graveyard.emplace_back(serial, std::make_shared<std::map<std::string, std::unique_ptr<vk::Framebuffer>>>(std::move(framebuffers)));
            framebuffers.clear();
        }
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkcmd.ClearColorImage(*fsr_layer->image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{}, range);
        Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        fsr_layer_on = fsr_layer_used = true;
        fsr_layer_h = &h;
        fsr_layer_begin_draw = idx;
        pass_dirty = true;
    }

    // at most one line a second for repeated layer conditions
    void FsrLayerNote(const std::string& why) {
        const double now = NvnVkNowMs();
        if (now - fsr_layer_note_ms < 1000.0) return;
        fsr_layer_note_ms = now;
        FsrLog(fmt::format("frame {}: {}", frame_no + 1, why));
    }

    void FsrLayerAbort(const std::string& why) {
        if (!fsr_layer_on && !fsr_layer_used) return;
        if (fsr_layer_used && fsr_layer_h) FsrHudMerge(*fsr_layer_h);
        fsr_layer_on = fsr_layer_used = false;
        fsr_layer_aborted = true;
        fsr_redirect_now = false;
        fsr_hud_pipe = fsr_hud_pipe2 = VK_NULL_HANDLE;
        pass_dirty = true;
        ++fsr_sec_aborts;
        FsrLayerNote("HUD layer abandoned for this frame, layer merged into H, old diff composite: " + why);
    }

    // H = layer + (1 - layer.a) * H (render size), through fsr_merge and a copy (H is not a storage image).
    void FsrHudMerge(Image& h) {
        if (!fsr_layer || !FsrEnsurePipes()) {
            FsrLayerNote("HUD layer could not be merged into H (no layer image / pipelines): the HUD is missing this frame");
            return;
        }
        EndPass();
        const u32 w = h.pw ? h.pw : h.width, hh = h.ph ? h.ph : h.height;
        // split scale: the layer (and the transmittance image) are 1X, H is at the render scale: the merge samples them
        // bilinearly at H's texel centres (vulkan_nvn_fsr_composite.comp, merge path); without the split the sizes match
        const bool resample = fsr_layer->width != w || fsr_layer->height != hh;
        if (resample && !split_on) return;
        if (resample) {
            SplitNote("layer merge", "a HUD layer was merged back into H (layer abandoned or FSR skipped in that frame): the HUD of "
                                     "such frames is at the render size, upscaled");
        }
        FsrEnsureImage(fsr_merge, h.format, w, hh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       "HUD merge (H + layer)");
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        // all 6 bindings and the full 32-byte push block of the composite layout (bindings 4/5 unused in merge mode): a
        // shorter array here made the push-descriptor template read past it (driver access violation, 902faf9)
        const bool tm = fsr_tm_used && fsr_tm && fsr_tm->width == fsr_layer->width && fsr_tm->height == fsr_layer->height;
        const VkSampler layer_sampler = resample ? *fsr_linear : *fsr_near;
        const std::array<VkDescriptorImageInfo, 7> desc{{
            {*fsr_near, *h.view, VK_IMAGE_LAYOUT_GENERAL},
            {*fsr_near, *h.view, VK_IMAGE_LAYOUT_GENERAL},
            {layer_sampler, *fsr_layer->view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, *fsr_merge->view, VK_IMAGE_LAYOUT_GENERAL},
            {*fsr_linear, *h.view, VK_IMAGE_LAYOUT_GENERAL},
            {*fsr_linear, *h.view, VK_IMAGE_LAYOUT_GENERAL},
            {layer_sampler, tm ? *fsr_tm->view : *h.view, VK_IMAGE_LAYOUT_GENERAL},
        }};
        static_assert(sizeof(desc) == 7 * sizeof(VkDescriptorImageInfo)); // = the composite layout's bindings
        const std::array<u32, 8> pc{0, 0, 0, 2u | (tm ? 16384u : 0u), 0, 0, 0, 0};
        vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_compose.pipeline);
        vkcmd.PushConstants(*fsr_compose.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc.data());
        vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_compose.tmpl, *fsr_compose.layout, 0, desc.data());
        vkcmd.Dispatch((w + 7) / 8, (hh + 7) / 8, 1);
        Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, hh, 1}};
        vkcmd.CopyImage(*fsr_merge->image, VK_IMAGE_LAYOUT_GENERAL, *h.image, VK_IMAGE_LAYOUT_GENERAL, region);
        Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        ++fsr_sec_merges;
    }

    // At the final draw (after FsrDispatch): which path the frame took; a layer that was not composited goes back into H
    // so the final draw shows the game's picture with its HUD.
    void FsrFinalLayer(bool composited) {
        fsr_final_now = false;
        if (fsr_layer_used && !composited) {
            if (fsr_layer_h) FsrHudMerge(*fsr_layer_h);
            fsr_layer_used = false;
        }
        u32 path = 0;
        if (fsr_layer_used) {
            path = 1;
            ++fsr_sec_layer;
            fsr_sec_redirected += fsr_layer_draws;
        } else if (composited) {
            path = 2;
            ++fsr_sec_diff;
        } else if (fsr_menu) {
            path = 3;
            ++fsr_sec_skip;
        } else {
            ++fsr_sec_none;
        }
        fsr_frame_path = path;
        fsr_frame_path_draws = fsr_layer_draws;
        if (SplitNow() && path == 2) {
            SplitNote("diff path", "a frame used the diff composite (no HUD layer): its HUD is at the render size, upscaled");
        } else if (SplitNow() && path != 1) {
            SplitNote("no fsr", fmt::format("frame {} was not upscaled by FSR (menu, still picture or FSR skipped): the render-size "
                                            "picture is scaled to the output by the game's final draw (bilinear)", frame_no + 1));
        }
        // menu skip: no composite runs, so the state indicator is a grey square copied into H's top-left corner
        // (render px; the final draw scales it with the rest of H)
        if (path == 3 && fsr_indicator && !fsr_dump_req && fsr_final_src && fsr_final_src->width >= 16 && fsr_final_src->height >= 16 &&
            fsr_final_src->format == VK_FORMAT_R8G8B8A8_UNORM) {
            EndPass();
            FsrEnsureImage(fsr_ind, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           "menu-skip indicator square");
            Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkClearColorValue grey{{0.5f, 0.5f, 0.5f, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkcmd.ClearColorImage(*fsr_ind->image, VK_IMAGE_LAYOUT_GENERAL, grey, range);
            Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const VkImageCopy region{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {1, 1, 0}, {4, 4, 1}};
            vkcmd.CopyImage(*fsr_ind->image, VK_IMAGE_LAYOUT_GENERAL, *fsr_final_src->image, VK_IMAGE_LAYOUT_GENERAL, region);
            Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        }
    }

    static const char* FsrPathText(u32 p) {
        switch (p) {
        case 1: return "HUD layer";
        case 2: return "diff (old HUD mask)";
        case 3: return "menu skip";
        default: return "no FSR";
        }
    }

    // ---- geometry object motion vectors ([Debug] nvn_fsr_objvel, Numpad9) -------------------------------------------
    // The game has no velocity buffer and skins on the GPU (its vertex shaders index constant buffers dynamically: bone
    // palettes). Instead of keeping bone palettes, the tracked draws run a variant of their own pipeline whose vertex
    // shader also writes its clip position to transform feedback (PatchXfbPosition): the capture IS the posed mesh of
    // this frame, so the previous frame's capture of the same draw is the previous pose, with the previous camera and
    // jitter already applied. Tracked draws: draws into the dynamic-object mask target (players, NPCs, small monsters:
    // what the game itself flags as moving) and, mode 2/3, opaque depth-tested scene draws whose vertex shader indexes a
    // constant buffer dynamically (skinned / animated: cloth, banners). A draw is identified across frames by
    // (class, vertex program, vertex buffer addresses, index buffer address, count, base vertex, topology); repeats of
    // the same key pair by order; a key without its vertex buffer addresses (streamed vertex data) pairs when unique.
    // At FSR time a velocity pass rasterizes each paired draw at this frame's positions (same floats as the game's
    // draw) into an RGBA16F image, validated against the final scene depth, and the merge puts those vectors over the
    // camera (or block-matched) vectors. Cloth that only re-poses at its simulation rate gives zero object motion in the
    // frames between and the exact jump in the frame it moves.
    struct ObjvEnt {
        u64 key{}, loose{}, geo{};
        VkDeviceSize offset{};
        u32 verts{};
        u32 depth_key{};
        u8 cls{}; // 1 dynamic-object mask target, 2 skinned opaque scene draw
        VkViewport vp{};
        bool expanded{};   // strip/fan with primitive restart: captured by an extra triangle-list draw
        bool jump{};       // CPU-animated vertices changed this frame after >= 2 unchanged frames (low-rate update)
        u64 dkey{};        // identity without vertex buffer addresses (static-draw exclusion)
        size_t list_at{};  // its expanded u32 indices in this frame's arena
    };
    static constexpr u32 kObjvMaxEnts = 1024;
    static constexpr VkDeviceSize kObjvSlotBytes = VkDeviceSize{96} << 20; // capture bytes per frame (2 slots; 6M vertices)
    static constexpr u32 kObjvMaxVerts = 1u << 20;

    bool ObjvCaptureBuildable() const {
        static const bool on = Core::GameSettings::NvnFsr() && Core::GameSettings::NvnFsrObjVel() > 0 && device.IsExtTransformFeedbackSupported();
        return on;
    }

    static const char* ObjvModeText(int m) {
        switch (m) {
        case 1: return "1 (dynamic-object mask draws)";
        case 2: return "2 (mask draws + skinned opaque scene draws)";
        case 3: return "3 (skinned opaque scene draws only)";
        case 4: return "4 (every depth-tested scene draw)";
        default: return "0 (off)";
        }
    }

    bool ObjvEnsureBuffers() {
        if (objv_xfb[0] && objv_xfb[1]) return true;
        if (objv_buf_failed) return false;
        try {
            for (u32 i = 0; i < 2; ++i) {
                objv_xfb[i] = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                      .size = kObjvSlotBytes,
                                                      .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                      .pQueueFamilyIndices = nullptr},
                                                     MemoryUsage::DeviceLocal);
            }
            FsrLog(fmt::format("object velocity: capture buffers 2 x {} MB created", kObjvSlotBytes >> 20));
            return true;
        } catch (const std::exception& e) {
            objv_buf_failed = true;
            objv_ok = false;
            FsrLog(fmt::format("object velocity: capture buffers could not be created ({}): geometry object vectors off", e.what()));
            return false;
        }
    }

    VkPipeline ObjvCapturePipeline(const PipelineKey& key, const Pipeline& pl, bool discard) {
        auto& cache = discard ? objv_pipes_rd : objv_pipes;
        if (const auto it = cache.find(key); it != cache.end()) return *it->second;
        vk::Pipeline p;
        if (pl.program && pl.program->vs_xfb) {
            const double t0 = NvnVkNowMs();
            try {
                auto in = std::make_unique<PipeInputs>(); // heap, as in GetPipeline
                PipelineInputs(key, *in);
                p = CreatePipelineObject(key, *pl.program, in->attributes, RenderPass(key), 0, *pl.program->vs_xfb, discard);
            } catch (const std::exception& e) {
                FsrLog(fmt::format("frame {}: object velocity capture pipeline (vs {:x} fs {:x}) FAILED: {}", frame_no + 1, key.prog[0],
                                   key.prog[1], e.what()));
            }
            const double ms = NvnVkNowMs() - t0;
            ++objv_pipes_built;
            objv_pipes_ms += ms;
            objv_sec_pipe_ms += ms;
        }
        const VkPipeline raw = *p;
        cache.emplace(key, std::move(p));
        return raw;
    }

    // Triangle-list indices of a strip (5) / fan (6) with primitive restart (index all ones), cached by (address, count,
    // width) like the index range cache (index data assumed immutable at an address). Odd strip triangles keep the
    // strip's vertex order swapped as the rasterizer does; degenerate triangles are kept (the count only depends on
    // the index data, so the same draw gives the same capture size every frame). nullptr = unreadable / no triangle.
    const std::vector<u32>* ObjvExpand(Core::Memory::Memory& mem, u64 iva, u32 count, bool wide, u32 topology) {
        const auto ck = std::make_tuple(iva, count, wide, topology);
        if (const auto it = objv_expand_cache.find(ck); it != objv_expand_cache.end()) return it->second.empty() ? nullptr : &it->second;
        if (objv_expand_cache.size() > 65536) objv_expand_cache.clear();
        std::vector<u32>& out = objv_expand_cache[ck];
        std::vector<u8> raw(size_t{count} * (wide ? 4 : 2));
        if (!ReadGpu(mem, iva, raw.data(), static_cast<u32>(raw.size()))) return nullptr;
        const u32 restart_value = wide ? 0xffffffffu : 0xffffu;
        u32 run = 0, a = 0, b = 0;
        out.reserve(size_t{count} * 3);
        for (u32 k = 0; k < count; ++k) {
            u32 v;
            if (wide) {
                std::memcpy(&v, raw.data() + size_t{k} * 4, 4);
            } else {
                u16 w;
                std::memcpy(&w, raw.data() + size_t{k} * 2, 2);
                v = w;
            }
            if (v == restart_value) {
                run = 0;
                continue;
            }
            if (run == 0) {
                a = v;
            } else if (run == 1) {
                b = v;
            } else if (topology == 6) {
                out.insert(out.end(), {a, b, v}); // fan: first, previous, current
                b = v;
            } else {
                if ((run & 1) == 0) {
                    out.insert(out.end(), {a, b, v});
                } else {
                    out.insert(out.end(), {b, a, v});
                }
                a = b;
                b = v;
            }
            ++run;
        }
        return out.empty() ? nullptr : &out;
    }

    void ObjvNewFrame(u64 f) {
        if (objv_cur_frame != 0 && objv_cur_frame + 1 == f) {
            objv_prev.swap(objv_cur);
            objv_prev_frame = objv_cur_frame;
        } else {
            objv_prev.clear();
            objv_prev_frame = 0;
        }
        objv_cur.clear();
        objv_cur_frame = f;
        objv_slot = static_cast<u32>(f & 1);
        objv_used = 0;
        objv_geo_mask.clear();
        objv_geo_mask_all.clear();
        objv_barrier_done = false;
        objv_frame_verts = 0;
        objv_ub_global.clear();
        for (const auto& [a, n] : objv_ub_count_cur) {
            if (n > 32) objv_ub_global.insert(a);
        }
        objv_ub_count_cur.clear();
        // the mask target only counts as a separate dynamic-object pass when it is not the target most scene draws went
        // to (frames without the mask pass would otherwise pick the scene colour target as "mask")
        u32 best_n = 0, best_rt = 0, mask_n = 0;
        for (const auto& [rt, n] : objv_rt_counts) {
            if (n > best_n) best_n = n, best_rt = rt;
            if (rt == objv_rt_mask) mask_n = n;
        }
        objv_mask_ok = objv_rt_mask != 0 && mask_n > 0 && objv_rt_mask != best_rt;
        objv_rt_counts.clear();
        objv_rt_mask = fsr_mask_key;
    }

    // Pose updates (baked low-rate animations: banners play at ~24-30 fps whatever the frame rate; the pose snaps and holds).
    // Per draw identity, compared with the previous frame:
    //  - vertex data: vertex buffer 0 address + FNV of up to 1 KB from the draw's first / base vertex;
    //  - bound textures by MEMORY (TIC address + size, not the handle: handles are ring-allocated) and the first 64 bytes of
    //    texel data of each bound texture (a flipbook that rewrites one texture in place);
    //  - diagnostics only (draws with <= 4096 indices): every constant buffer slot of both stages separately (up to 256
    //    bytes), and per 16-byte chunk of the slots that change, to find the one vec4 a baked animation steps.
    // "Update" = a change >= 3 frames after the previous change of the same thing (low rate); "continuous" = 1-2 frames.
    // Vertex data or texture memory/content updates mark the draw ("jump"): its depth-validated texels get a high
    // reactive in that frame and the next. Constant slots are logged only (camera / time constants change every frame).
    // vdyn = the vertex data changed within objv_ttl frames (tracking criterion of modes 2/3).
    static constexpr u32 kSigSlotBytes = 256;
    struct ObjvSigCounter {
        u64 changes{}, updates{}, period_sum{}, last{};
        void Hit(u64 f) {
            ++changes;
            if (last != 0 && f - last >= 3) {
                ++updates;
                period_sum += f - last;
            }
            last = f;
        }
    };
    void ObjvSignals(Core::Memory::Memory& mem, const Program& prog, u64 dkey, s32 first_or_base, u64 f, u64 vs, u64 fs, u32 count,
                     bool& vdyn, bool& jump) {
        if (objv_sig.size() > 6000) objv_sig.clear();
        ObjvSigRec& r = objv_sig[dkey];
        if (r.frame == f) { // repeated identity in this frame: same verdict as its first draw
            vdyn = r.vchange != 0 && f - r.vchange <= objv_ttl;
            jump = r.jump_frame != 0 && (r.jump_frame == f || r.jump_frame + 1 == f);
            return;
        }
        const bool consecutive = r.frame != 0 && r.frame + 1 == f;
        const u64 cls_key = vs ^ (fs * 1099511628211ull);
        ObjvSigStat& ss = objv_sigstat[cls_key];
        ss.vs = vs;
        ss.fs = fs;
        ++ss.draws;
        // vertex data
        u64 vh = 0;
        const u64 va = st.vb[0].addr;
        if (va != 0 && st.vb[0].size != 0) {
            const u64 start = first_or_base > 0 ? u64(first_or_base) * st.stride[0] : 0;
            if (start < st.vb[0].size) {
                const u32 n = static_cast<u32>(std::min<u64>(1024, st.vb[0].size - start));
                objv_vscratch.resize(n);
                if (ReadGpu(mem, va + start, objv_vscratch.data(), n)) {
                    vh = 1469598103934665603ull;
                    Fnv(vh, objv_vscratch.data(), n);
                }
            }
            vh ^= va * 0x9E3779B97F4A7C15ull;
        }
        // texture memory and first texels (diagnostics)
        u64 th = 1469598103934665603ull, tc = 1469598103934665603ull;
        for (u32 s2 = 0; s2 < 2 && objv_signals; ++s2) {
            for (u32 i = 0; i < 8; ++i) {
                const u32 hnd = st.tex_handle[s2][i];
                if (hnd == 0) continue;
                const Tegra::Texture::TICEntry tic = TicFor(mem, st.tex_pool, hnd);
                const u64 ta = tic.Address();
                FnvV(th, ta);
                FnvV(th, tic.Width());
                FnvV(th, tic.Height());
                u8 texels[64];
                if (ta != 0 && ReadGpu(mem, ta, texels, sizeof(texels))) Fnv(tc, texels, sizeof(texels));
            }
        }
        bool upd = false;
        if (consecutive) {
            const auto bump = [&](ObjvSigCounter& c) {
                const bool low = c.last != 0 && f - c.last >= 3;
                c.Hit(f);
                return low;
            };
            if (r.vhash != vh) {
                upd |= bump(r.vtx);
                ss.vtx.Hit(f);
                r.vchange = f;
            }
            if (r.thash != th) {
                upd |= bump(r.tex);
                ss.tex.Hit(f);
            }
            if (r.tchash != tc) {
                upd |= bump(r.texel);
                ss.texel.Hit(f);
            }
        } else if (r.frame != 0) {
            r.vtx.last = r.tex.last = r.texel.last = 0; // a gap in the draw's frames: start over
        }
        // constant slots (diagnostics)
        if (objv_signals && count <= 4096) {
            for (u32 s2 = 0; s2 < 2; ++s2) {
                for (u32 i = 0; i < 14; ++i) {
                    const u64 a = st.ub_addr[s2][i];
                    const u32 used = prog.info[s2].constant_buffer_used_sizes[i + 3];
                    if (a == 0 || used == 0) continue;
                    const u32 n = std::min<u32>(std::min<u32>(st.ub_size[s2][i], used), kSigSlotBytes);
                    if (n == 0) continue;
                    u8 cur_bytes[kSigSlotBytes]{};
                    if (!ReadGpu(mem, a, cur_bytes, n)) continue;
                    ObjvSlotRec& sr = r.slot[s2][i];
                    if (consecutive && sr.n == n && std::memcmp(sr.bytes, cur_bytes, n) != 0) {
                        ObjvSlotStat& st2 = ss.slot[s2][i];
                        st2.all.Hit(f);
                        for (u32 c = 0; c < n / 16; ++c) {
                            if (std::memcmp(sr.bytes + c * 16, cur_bytes + c * 16, 16) != 0) st2.chunk[c].Hit(f);
                        }
                    }
                    std::memcpy(sr.bytes, cur_bytes, n);
                    sr.n = n;
                }
            }
        }
        if (upd) {
            r.jump_frame = f;
            ++objv_sec_updates;
        }
        r.frame = f;
        r.vhash = vh;
        r.thash = th;
        r.tchash = tc;
        vdyn = r.vchange != 0 && f - r.vchange <= objv_ttl;
        jump = r.jump_frame != 0 && (r.jump_frame == f || r.jump_frame + 1 == f);
    }

    // Every 5 s: the signals ranked by low-rate behaviour (updates whose mean period is 3-8 frames first: a 24-30 fps
    // baked animation at 120 fps steps every 4-5 frames), then by update count.
    std::string ObjvSigText() const {
        struct Row {
            double score;
            std::string text;
        };
        std::vector<Row> rows;
        const auto add = [&](const ObjvSigStat& e, const char* what, const ObjvSigCounter& c) {
            if (c.changes == 0) return;
            const double period = c.updates ? double(c.period_sum) / c.updates : 0.0;
            const u64 cont = c.changes - c.updates;
            const bool slow = c.updates > 0 && period >= 3.0 && period <= 8.0;
            const double score = (slow ? 1e9 : 0.0) + double(c.updates) * 1000.0 - double(cont);
            rows.push_back({score, fmt::format("vs {:x} fs {:x} {}: changes {} low-rate updates {} mean period {:.1f} frames, continuous {} "
                                               "(draws {})",
                                               e.vs, e.fs, what, c.changes, c.updates, period, cont, e.draws)});
        };
        for (const auto& kv : objv_sigstat) {
            const ObjvSigStat& e = kv.second;
            add(e, "vertex data", e.vtx);
            add(e, "texture memory", e.tex);
            add(e, "texel bytes", e.texel);
            for (u32 s2 = 0; s2 < 2; ++s2) {
                for (u32 i = 0; i < 14; ++i) {
                    const ObjvSlotStat& sl = e.slot[s2][i];
                    if (sl.all.changes == 0) continue;
                    add(e, fmt::format("{} cbuf slot {}", s2 ? "fragment" : "vertex", i).c_str(), sl.all);
                    for (u32 c = 0; c < kSigSlotBytes / 16; ++c) {
                        if (sl.chunk[c].updates > 0) {
                            add(e, fmt::format("{} cbuf slot {} bytes {:#x}-{:#x}", s2 ? "fragment" : "vertex", i, c * 16, c * 16 + 15).c_str(),
                                sl.chunk[c]);
                        }
                    }
                }
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });
        std::string t = fmt::format("pose-change signals ({} rows; ranked: low-rate period 3-8 frames first, then updates; 'updates' = "
                                    "change >= 3 frames after the previous one), top 30:",
                                    rows.size());
        for (size_t i = 0; i < rows.size() && i < 30; ++i) t += "\n    " + rows[i].text;
        return t;
    }

    // Histogram of skipped opaque / blended scene draws (logged once a second, top entries): which draws the player
    // and the cloth are.
    void ObjvSkipHist(const PipelineKey& key, const Pipeline& pl, u32 count, u32 topology, bool blended, u64 dkey) {
        u64 hk = 1469598103934665603ull;
        FnvV(hk, key.prog[0]);
        FnvV(hk, key.prog[1]);
        FnvV(hk, count);
        FnvV(hk, blended);
        if (objv_hist.size() > 4096 && !objv_hist.contains(hk)) return;
        ObjvHistEnt& h = objv_hist[hk];
        if (h.n == 0) {
            h.vs = key.prog[0];
            h.fs = key.prog[1];
            h.count = count;
            h.topology = topology;
            h.blended = blended;
            if (pl.program) {
                h.dyn = pl.program->vs_dyn_cbuf;
                h.vstex = pl.program->vs_tex;
                for (u32 c = 3; c < Shader::Info::MAX_CBUFS; ++c) {
                    const u32 sz = pl.program->info[0].constant_buffer_used_sizes[c];
                    if (sz != 0) h.cbufs += fmt::format("{}{}:{:x}", h.cbufs.empty() ? "" : ",", c - 3, sz);
                }
            }
        }
        ++h.n;
        h.in_mask = h.in_mask || objv_geo_mask_all.contains(dkey);
        if (const auto it = objv_sig.find(dkey); it != objv_sig.end() && it->second.vchange != 0) h.vchanged = true;
    }

    std::string ObjvSkipHistText() const {
        std::vector<const ObjvHistEnt*> v;
        for (const auto& kv : objv_hist) v.push_back(&kv.second);
        std::sort(v.begin(), v.end(), [](const ObjvHistEnt* a, const ObjvHistEnt* b) { return a->n > b->n; });
        std::string t = fmt::format("skipped scene draws, {} distinct (vs, fs, index/vertex count, topology, blend, VS dynamic cbuf, VS "
                                    "samples textures, VS cbufs NVN slot:used size, also drawn into the mask target, vertex data changed "
                                    "since tracked, draws this second), top 24:",
                                    v.size());
        for (size_t i = 0; i < v.size() && i < 24; ++i) {
            const ObjvHistEnt& e = *v[i];
            t += fmt::format("\n    vs {:x} fs {:x} n {} topo {} {} dyncbuf {} vstex {} cbufs [{}] mask {} vchg {} x{}", e.vs, e.fs, e.count,
                             e.topology, e.blended ? "blend" : "opaque", e.dyn, e.vstex, e.cbufs, e.in_mask, e.vchanged, e.n);
        }
        return t;
    }

    // DoDraw, before the render pass is begun: decide whether this draw is captured (sets objv_cap_now / objv_cap_pipe
    // and appends this frame's entry).
    void ObjvOnDraw(Core::Memory::Memory& mem, const PipelineKey& key, const Pipeline& pl, bool indexed, u32 topology, u32 count,
                    s32 first_or_base, bool restart, u64 index_va, bool wide) {
        if (!objv_on || !objv_ok || !fsr_active || fsr_failed || fsr_menu || fsr_redirect_now) return;
        if (st.rt_count == 0 || st.rt_tex[0] == 0 || st.depth_tex == 0) return;
        const u64 f = frame_no + 1;
        if (objv_cur_frame != f) ObjvNewFrame(f);
        const u32 rt0 = RtKeyFrame(mem, st.rt_tex[0]);
        // menu 3D (still frame, before the snapshot): the opaque meshes drawn into H are tracked too
        const bool menu_mesh = rt0 != 0 && rt0 == fsr_src_key && fsr_menu3d_on && fsr_menu3d_draw != 0 && !fsr_snap_ok && !fsr_scene_now &&
                               indexed && count >= 64 && !((key.blend_enable & 1) && key.blend[0][1] != 1);
        if (rt0 == 0 || (rt0 == fsr_src_key && !menu_mesh) || mv_present_keys.contains(rt0)) return;
        Image* dimg = RenderTarget(mem, st.depth_tex, true);
        Image* cimg = RenderTarget(mem, st.rt_tex[0], false);
        if (!dimg || !cimg || dimg->width < 320 || dimg->width != cimg->width || dimg->height != cimg->height) {
            ++objv_sec_sk_target;
            return;
        }
        bool counted = false;
        for (auto& [rt, n] : objv_rt_counts) {
            if (rt == rt0) {
                ++n;
                counted = true;
                break;
            }
        }
        if (!counted && objv_rt_counts.size() < 16) objv_rt_counts.push_back({rt0, 1});
        const bool into_mask = fsr_mask_key != 0 && rt0 == fsr_mask_key;
        const bool mask = objv_mask_ok && into_mask;
        // blending that replaces the destination (dst colour factor ZERO, e.g. ONE/ZERO) counts as opaque here
        const bool blend_on = (key.blend_enable & 1) != 0 && key.blend[0][1] != 1;
        const bool opaque = !blend_on && (key.ds & 1) != 0;
        // draw identity without vertex buffer addresses (also the key of the dynamic-vertex record)
        u64 dkey = 1469598103934665603ull;
        FnvV(dkey, key.prog[0]);
        FnvV(dkey, index_va);
        FnvV(dkey, count);
        FnvV(dkey, first_or_base);
        FnvV(dkey, topology);
        if (into_mask) objv_geo_mask_all.insert(dkey);
        // CPU-animated vertices (CPU skinning, cloth): the vertex data of this draw changed (address or a content sample)
        // within the last objv_ttl frames -> tracked every frame, so frames without a pose change give 0 object motion and
        // the update frame gives the jump; jump = changed now after >= 2 unchanged frames (low-rate update)
        bool vdyn = false, jump = false;
        const bool skinned = pl.program && (pl.program->vs_dyn_cbuf || pl.program->vs_tex);
        // pose-update signals of this draw (vertex data, vertex-stage constants, bound textures); every depth-tested scene
        // draw, so the baked low-rate animations (banners) show which signal they use
        // pose-update signals: vertex data (needed for modes 2/3 and cheap); texture / constant diagnostics only with
        // [Debug] nvn_fsr_signals = 1
        if (pl.program && (key.ds & 1) != 0 && (objv_signals || ((objv_mode == 2 || objv_mode == 3) && opaque && !skinned))) {
            const double sig_t0 = NvnVkNowMs();
            ObjvSignals(mem, *pl.program, dkey, first_or_base, f, key.prog[0], key.prog[1], count, vdyn, jump);
            objv_sec_sig_ms += NvnVkNowMs() - sig_t0;
        }
        u8 cls = 0;
        if (menu_mesh) {
            cls = 2;
            objv_menu_frame = f;
            objv_menu_depth = dimg;
        }
        // mode 4: every depth-tested scene draw (blended ones only when they also write depth): animation evaluated in
        // the vertex shader from constants (wind / time: banners, flags) is in the captured positions; static draws get
        // camera-equivalent vectors
        const bool all4 = objv_mode == 4 && (key.ds & 1) != 0 && (!blend_on || (key.ds & 2) != 0);
        if (cls != 0) {
        } else if (mask && objv_mode != 3) {
            cls = 1;
        } else if (!mask && objv_mode >= 2 && ((opaque && (skinned || vdyn)) || all4)) {
            cls = 2;
        }
        if (cls == 2 && objv_mode == 4 && objv_static_on && !skinned && !vdyn) {
            // a draw whose object vectors matched the camera's for 240 checked frames: camera vectors are exact for it
            if (const auto it = objv_static.find(dkey); it != objv_static.end() && f < it->second.excl_until) {
                ++objv_sec_sk_static;
                return;
            }
        }
        if (cls == 0) {
            // reason histogram (once a second in the log)
            if (into_mask) {
                ++(objv_mode == 3 ? objv_sec_sk_mode : objv_sec_sk_maskoff);
            } else if (objv_mode < 2) {
                ++objv_sec_sk_mode;
            } else if (blend_on) {
                ++objv_sec_sk_blend;
                if (objv_signals) ObjvSkipHist(key, pl, count, topology, true, dkey);
            } else if (!(key.ds & 1)) {
                ++objv_sec_sk_nodepth;
            } else {
                ++objv_sec_sk_noskin;
                if (objv_signals) ObjvSkipHist(key, pl, count, topology, false, dkey);
            }
            return;
        }
        u32 verts = 0;
        const std::vector<u32>* list = nullptr; // strips / fans with primitive restart: expanded triangle list indices
        if (topology == 4) {
            verts = count - count % 3;
        } else if ((topology == 5 || topology == 6) && count >= 3) {
            if (key.restart && restart) {
                if (!indexed) {
                    ++objv_sec_sk_restart;
                    return;
                }
                list = ObjvExpand(mem, index_va, count, wide, topology);
                if (!list) {
                    ++objv_sec_sk_unread;
                    return;
                }
                verts = static_cast<u32>(list->size());
            } else {
                verts = 3 * (count - 2);
            }
        }
        if (verts == 0) {
            ++objv_sec_sk_topo;
            return;
        }
        if (verts > kObjvMaxVerts) {
            ++objv_sec_sk_big;
            return;
        }
        const VkDeviceSize bytes = (VkDeviceSize{verts} * 16 + 255) & ~VkDeviceSize{255};
        if (objv_used + bytes > kObjvSlotBytes || objv_cur.size() >= kObjvMaxEnts) {
            ++objv_sec_sk_full;
            return;
        }
        if (objv_frame_verts + verts > objv_max_verts) {
            ++objv_sec_sk_vcap; // per-frame vertex cap ([Debug] nvn_fsr_objvel_maxverts)
            return;
        }
        u64 geo = 1469598103934665603ull;
        FnvV(geo, st.vb[0].addr);
        FnvV(geo, index_va);
        FnvV(geo, count);
        FnvV(geo, first_or_base);
        FnvV(geo, topology);
        FnvV(geo, indexed);
        if (cls == 2 && objv_geo_mask.contains(geo)) {
            ++objv_sec_sk_dedup; // already captured in the mask pass this frame
            return;
        }
        u64 loose = geo;
        FnvV(loose, cls);
        FnvV(loose, key.prog[0]);
        u64 full = loose;
        for (u32 b = 0; b < 4; ++b) FnvV(full, st.vb[b].addr);
        if (!ObjvEnsureBuffers()) return;
        // expanded draws are captured by a second draw of their own (triangle list over the expanded indices, rasterizer
        // discard) right after the game's draw; the game's draw itself is unchanged
        PipelineKey ck = key;
        if (list) {
            ck.topology = 4;
            ck.restart = false;
        }
        const VkPipeline p = ObjvCapturePipeline(ck, pl, list != nullptr);
        if (p == VK_NULL_HANDLE) {
            ++objv_sec_sk_nopipe;
            return;
        }
        size_t list_at = 0;
        if (list) {
            list_at = UploadBytes(list->data(), static_cast<u32>(list->size() * 4), 4);
            if (list_at == ~size_t{0}) {
                ++objv_sec_sk_arena;
                return;
            }
        }
        if (!objv_barrier_done) {
            // the slot's previous contents (two frames ago) were read by that frame's velocity pass: order the writes after it
            if (pass_active) {
                EndPass(); // includes a full memory barrier
            } else {
                Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            }
            objv_barrier_done = true;
        }
        ObjvEnt e{};
        e.key = full;
        e.loose = loose;
        e.geo = geo;
        e.offset = objv_used;
        e.verts = verts;
        e.depth_key = dimg->cpu;
        e.cls = cls;
        e.dkey = dkey;
        e.expanded = list != nullptr;
        e.jump = jump;
        if (vdyn) ++objv_sec_cap_vdyn;
        if (jump) ++objv_sec_jumps;
        e.list_at = list_at;
        objv_cur.push_back(e);
        if (list) ++objv_sec_cap_expanded;
        objv_used += bytes;
        objv_frame_verts += verts;
        objv_sec_verts_max = std::max<u64>(objv_sec_verts_max, objv_frame_verts);
        objv_sec_bytes_max = std::max<u64>(objv_sec_bytes_max, objv_used);
        if (cls == 1) {
            objv_geo_mask.insert(geo);
            ++objv_sec_cap_mask;
        } else {
            ++objv_sec_cap_scene;
        }
        objv_cap_now = true;
        objv_cap_pipe = p;
    }

    bool ObjvEnsureFlagsBuf() {
        if (objv_flags_buf) return true;
        try {
            objv_flags_buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                     .size = VkDeviceSize{kSlots} * kObjvMaxEnts * 4,
                                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                     .pQueueFamilyIndices = nullptr},
                                                    MemoryUsage::Download);
            return true;
        } catch (const std::exception& e) {
            objv_static_on = false;
            FsrLog(fmt::format("object velocity: draw flag buffer failed ({}): static-draw exclusion off", e.what()));
            return false;
        }
    }

    // The draw flags this frame slot collected when it was last used (its fence has been waited for: BeginFrame), fed into
    // the static-draw records: visible and never moving for 240 checked frames -> excluded from capture for 600 frames.
    void ObjvReadDrawFlags(u64 f) {
        if (!objv_flags_buf || cur >= objv_flags_dkeys.size() || objv_flags_dkeys[cur].empty()) return;
        objv_flags_buf.Invalidate();
        const u32* fl = reinterpret_cast<const u32*>(objv_flags_buf.Mapped().data()) + size_t{cur} * kObjvMaxEnts;
        auto& keys = objv_flags_dkeys[cur];
        for (size_t i = 0; i < keys.size() && i < kObjvMaxEnts; ++i) {
            const u32 v = fl[i];
            if (!(v & 2u)) continue; // not visible: no verdict
            ObjvStaticRec& r = objv_static[keys[i]];
            if (v & 1u) {
                r.stable = 0;
                r.excl_until = 0;
                ++objv_sec_seen_moving;
            } else if (++r.stable >= objv_static_frames) {
                r.excl_until = f + 600;
                r.stable = objv_static_rearm; // after the re-check, 40 more static frames exclude it again (default 240 / 200)
                ++objv_sec_newly_static;
            }
        }
        keys.clear();
        if (objv_static.size() > 100000) objv_static.clear();
    }

    bool ObjvEnsureVelPipe() {
        if (objv_vel_ready) return true;
        if (objv_vel_failed) return false;
        try {
            const std::array<VkDescriptorSetLayoutBinding, 3> b{{
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            }};
            objv_set_layout = dev.CreateDescriptorSetLayout({
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                .bindingCount = 3,
                .pBindings = b.data(),
            });
            const VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ObjvPush)};
            objv_layout = dev.CreatePipelineLayout({
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = 1,
                .pSetLayouts = objv_set_layout.address(),
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &pcr,
            });
            const std::array<VkDescriptorUpdateTemplateEntry, 3> te{{
                {0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(ObjvDesc, depth), sizeof(VkDescriptorImageInfo)},
                {1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(ObjvDesc, cam), sizeof(VkDescriptorImageInfo)},
                {2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(ObjvDesc, flags), sizeof(VkDescriptorBufferInfo)},
            }};
            objv_tmpl = dev.CreateDescriptorUpdateTemplate({
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .descriptorUpdateEntryCount = 3,
                .pDescriptorUpdateEntries = te.data(),
                .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
                .descriptorSetLayout = *objv_set_layout,
                .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                .pipelineLayout = *objv_layout,
                .set = 0,
            });
            const auto module = [&](const u32* code, size_t bytes) {
                return dev.CreateShaderModule({
                    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .codeSize = bytes,
                    .pCode = code,
                });
            };
            objv_vs = module(VULKAN_NVN_FSR_OBJVEL_VERT_SPV, sizeof(VULKAN_NVN_FSR_OBJVEL_VERT_SPV));
            objv_fs = module(VULKAN_NVN_FSR_OBJVEL_FRAG_SPV, sizeof(VULKAN_NVN_FSR_OBJVEL_FRAG_SPV));
            const VkAttachmentDescription att{0, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                              VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                              VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
            const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
            const VkSubpassDescription sub{
                .flags = 0,
                .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                .inputAttachmentCount = 0,
                .pInputAttachments = nullptr,
                .colorAttachmentCount = 1,
                .pColorAttachments = &ref,
                .pResolveAttachments = nullptr,
                .pDepthStencilAttachment = nullptr,
                .preserveAttachmentCount = 0,
                .pPreserveAttachments = nullptr,
            };
            objv_rp = dev.CreateRenderPass({
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .attachmentCount = 1,
                .pAttachments = &att,
                .subpassCount = 1,
                .pSubpasses = &sub,
                .dependencyCount = 0,
                .pDependencies = nullptr,
            });
            const std::array<VkVertexInputBindingDescription, 2> vb{{{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_VERTEX}}};
            const std::array<VkVertexInputAttributeDescription, 2> va{{{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}}};
            const VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 2, vb.data(), 2, va.data()};
            const VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
            const VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState ba{};
            ba.colorWriteMask = 0xf;
            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 1;
            cb.pAttachments = &ba;
            const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            const VkPipelineDynamicStateCreateInfo dyn_ci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dyn};
            const VkPipelineShaderStageCreateInfo stages[] = {
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, *objv_vs, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, *objv_fs, "main", nullptr},
            };
            objv_pipe = dev.CreateGraphicsPipeline({
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stageCount = 2,
                .pStages = stages,
                .pVertexInputState = &vi,
                .pInputAssemblyState = &ia,
                .pTessellationState = nullptr,
                .pViewportState = &vps,
                .pRasterizationState = &rs,
                .pMultisampleState = &ms,
                .pDepthStencilState = nullptr,
                .pColorBlendState = &cb,
                .pDynamicState = &dyn_ci,
                .layout = *objv_layout,
                .renderPass = *objv_rp,
                .subpass = 0,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex = 0,
            });
            constexpr VkDescriptorType kTex = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            FsrBuildPipe(objv_merge, {kTex, kTex, kTex, kImg, kImg, kTex}, VULKAN_NVN_FSR_OBJVEL_MERGE_COMP_SPV, sizeof(VULKAN_NVN_FSR_OBJVEL_MERGE_COMP_SPV), 16);
            objv_vel_ready = true;
            FsrLog("object velocity: velocity pass and merge pipelines built (host shaders vulkan_nvn_fsr_objvel.vert/.frag, "
                   "vulkan_nvn_fsr_objvel_merge.comp)");
            return true;
        } catch (const std::exception& e) {
            objv_vel_failed = true;
            objv_ok = false;
            FsrLog(fmt::format("object velocity: pipelines failed ({}): geometry object vectors off", e.what()));
            return false;
        }
    }

    struct ObjvPush {
        float vp[4];
        float shift_cur[2];
        float shift_prev[2];
        float p22, p32, max_px;
        u32 flags;
        u32 draw_idx;
        u32 pad[3];
    };
    struct ObjvDesc {
        VkDescriptorImageInfo depth, cam;
        VkDescriptorBufferInfo flags;
    };

    // FsrDispatch, after the block-matching passes: velocity pass + merge into objv_mvo. True = FSR reads objv_mvo and
    // objv_vel is valid for the prep pass this frame.
    bool ObjvRun(u64 f, u32 rw, u32 rh, Image& d, const float (&shift_cur)[2], const float (&shift_prev)[2], bool shift_prev_ok,
                 bool depth_ok, float p22, float p32, bool base_is_block, bool stat_now, Image* mask, Image& cam) {
        objv_stat_want = false;
        if (!objv_on || !objv_ok) return false;
        ++objv_sec_frames;
        if (objv_cur_frame != f || objv_cur.empty()) {
            ++objv_sec_nocap;
            return false;
        }
        if (objv_prev_frame + 1 != f || objv_prev.empty() || !shift_prev_ok) {
            ++objv_sec_nohist;
            objv_sec_unmatched += objv_cur.size();
            return false;
        }
        if (!ObjvEnsureVelPipe()) return false;
        // pair this frame's captures with the previous frame's
        objv_pf.clear();
        objv_pl.clear();
        objv_cf.clear();
        objv_cl.clear();
        for (u32 i = 0; i < objv_prev.size(); ++i) {
            objv_pf[objv_prev[i].key].push_back(i);
            objv_pl[objv_prev[i].loose].push_back(i);
        }
        for (const ObjvEnt& e : objv_cur) {
            ++objv_cf[e.key];
            ++objv_cl[e.loose];
        }
        objv_pairs.clear();
        std::unordered_map<u64, u32> ord;
        for (u32 i = 0; i < objv_cur.size(); ++i) {
            const ObjvEnt& e = objv_cur[i];
            const u32 k = ord[e.key]++;
            u32 j = ~0u;
            if (const auto it = objv_pf.find(e.key); it != objv_pf.end()) {
                if (it->second.size() == objv_cf[e.key]) {
                    j = it->second[k];
                    if (it->second.size() > 1) ++objv_sec_m_order;
                } else {
                    ++objv_sec_ambiguous;
                    continue;
                }
            } else if (const auto lt = objv_pl.find(e.loose); lt != objv_pl.end() && lt->second.size() == 1 && objv_cl[e.loose] == 1) {
                j = lt->second[0];
                ++objv_sec_m_loose;
            } else {
                ++objv_sec_unmatched;
                continue;
            }
            const ObjvEnt& pe = objv_prev[j];
            if (pe.verts != e.verts || pe.cls != e.cls || e.depth_key != d.cpu) {
                ++objv_sec_mismatch;
                continue;
            }
            objv_pairs.push_back({i, j});
        }
        objv_sec_matched += objv_pairs.size();
        ObjvReadDrawFlags(f);
        if (objv_pairs.empty()) return false;
        constexpr VkImageUsageFlags kUse = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        FsrEnsureImage(objv_vel, VK_FORMAT_R16G16B16A16_SFLOAT, rw, rh, kUse | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                       "object velocity (RGBA16F: mv, valid, |mv|)");
        FsrEnsureImage(objv_mvo, VK_FORMAT_R32G32_SFLOAT, rw, rh, kUse, "motion vectors with geometry object vectors (RG32F)");
        FsrEnsureImage(objv_stat, VK_FORMAT_R32_UINT, 8, 1, kUse | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "object velocity counters (R32UI)");
        if (!objv_fb || objv_fb_view != *objv_vel->view) {
            if (objv_fb) fb_graveyard.emplace_back(serial, std::make_shared<vk::Framebuffer>(std::move(objv_fb)));
            const VkImageView v = *objv_vel->view;
            objv_fb = dev.CreateFramebuffer({
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .renderPass = *objv_rp,
                .attachmentCount = 1,
                .pAttachments = &v,
                .width = rw,
                .height = rh,
                .layers = 1,
            });
            objv_fb_view = v;
        }
        // the captures of this frame and the last one are complete (transform feedback writes) before they are read
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        // static-draw check: per drawn pair, bit 1 = visible, bit 0 = moving (zeroed here, read when this slot comes back)
        if (!ObjvEnsureFlagsBuf()) return false; // the velocity shader's flag buffer binding must be valid
        const bool flags_on = objv_mode == 4 && objv_static_on;
        if (flags_on) {
            vkcmd.FillBuffer(*objv_flags_buf, VkDeviceSize{cur} * kObjvMaxEnts * 4, kObjvMaxEnts * 4, 0);
            Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            objv_flags_dkeys[cur].clear();
            objv_flags_frame[cur] = f;
        }
        const VkClearValue clear{.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}};
        const VkRenderPassBeginInfo bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = *objv_rp,
            .framebuffer = *objv_fb,
            .renderArea = {{0, 0}, {rw, rh}},
            .clearValueCount = 1,
            .pClearValues = &clear,
        };
        vkcmd.BeginRenderPass(bi, VK_SUBPASS_CONTENTS_INLINE);
        vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *objv_pipe);
        const ObjvDesc di{{*fsr_near, *d.sampled_view, VK_IMAGE_LAYOUT_GENERAL},
                          {*fsr_near, *cam.view, VK_IMAGE_LAYOUT_GENERAL},
                          {*objv_flags_buf, VkDeviceSize{cur} * kObjvMaxEnts * 4, kObjvMaxEnts * 4}};
        vkcmd.PushDescriptorSetWithTemplateKHR(*objv_tmpl, *objv_layout, 0, &di);
        const VkRect2D sc{{0, 0}, {rw, rh}};
        vkcmd.SetScissor(0, sc);
        const u32 prev_slot = objv_slot ^ 1u;
        u64 verts = 0;
        for (const auto& [i, j] : objv_pairs) {
            const ObjvEnt& e = objv_cur[i];
            const ObjvEnt& pe = objv_prev[j];
            if (e.vp.width == 0.0f || e.vp.height == 0.0f) continue;
            const ObjvPush pc{{e.vp.x, e.vp.y, e.vp.width, e.vp.height},
                              {shift_cur[0], shift_cur[1]},
                              {shift_prev[0], shift_prev[1]},
                              depth_ok ? p22 : 0.0f,
                              depth_ok ? p32 : 0.0f,
                              objv_max_px,
                              (depth_ok ? 1u : 0u) | (e.jump ? 2u : 0u) | (flags_on ? 4u : 0u),
                              flags_on ? static_cast<u32>(objv_flags_dkeys[cur].size()) : 0u,
                              {}};
            if (flags_on) objv_flags_dkeys[cur].push_back(e.dkey);
            vkcmd.PushConstants(*objv_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkcmd.SetViewport(0, e.vp);
            const std::array<VkBuffer, 2> bufs{*objv_xfb[objv_slot], *objv_xfb[prev_slot]};
            const std::array<VkDeviceSize, 2> offs{e.offset, pe.offset};
            vkcmd.BindVertexBuffers(0, 2, bufs.data(), offs.data());
            vkcmd.Draw(e.verts, 1, 0, 0);
            verts += e.verts;
        }
        vkcmd.EndRenderPass();
        GpMarkNamed("fsr_objvel_raster"); // gpu_diagnostics: velocity raster; fsr_objvel_merge is then the merge
        objv_sec_verts += verts;
        ++objv_sec_passes;
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        // merge: object vector where valid, else the base vectors
        if (stat_now) {
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkClearColorValue zero{};
            vkcmd.ClearColorImage(*objv_stat->image, VK_IMAGE_LAYOUT_GENERAL, zero, range);
            Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        const std::array<VkDescriptorImageInfo, 6> mdesc{{
            {*fsr_near, base_is_block ? *fsr_mv->view : *cam.view, VK_IMAGE_LAYOUT_GENERAL},
            {*fsr_near, *cam.view, VK_IMAGE_LAYOUT_GENERAL},
            {*fsr_near, *objv_vel->view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, *objv_mvo->view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, *objv_stat->view, VK_IMAGE_LAYOUT_GENERAL},
            {*fsr_near, mask ? *mask->view : *cam.view, VK_IMAGE_LAYOUT_GENERAL},
        }};
        const std::array<u32, 4> mpc{(stat_now ? 1u : 0u) | (mask ? 2u : 0u), 0u, 0u, 0u};
        vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *objv_merge.pipeline);
        vkcmd.PushConstants(*objv_merge.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, mpc.data());
        vkcmd.PushDescriptorSetWithTemplateKHR(*objv_merge.tmpl, *objv_merge.layout, 0, mdesc.data());
        vkcmd.Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
        // merge written -> readers: animsurf / prep / FSR 3.1 (compute), FSR 4 input copy and the stats copy (transfer). The next
        // frame's velocity raster (write-after-read of objv_vel) is ordered by the ALL_COMMANDS barrier before its render pass
        Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        objv_stat_want = stat_now;
        objv_stat_texels = rw * rh;
        objv_stat_pairs = static_cast<u32>(objv_pairs.size());
        objv_vel_frame = f;
        return true;
    }

    // The game's final draw (after the motion vector pass): prep, FSR, composite. True = the draw may sample fsr_comp.
    bool FsrDispatch(Core::Memory::Memory& mem) {
        const u64 f = frame_no + 1;
        Image* src = fsr_final_src;
        // scene frame: the game drew its 3D scene in this frame (FsrMenuUpdate); otherwise a still picture (menus over a
        // stored scene image, loading / save screens): no scene depth / camera vectors that belong to these pixels, so FSR
        // gets zero motion, the last scene depth and the jitter that is really in the picture (the last scene frame's, 0
        // when no scene depth at all), and accumulates a steady image instead of shivering.
        const bool scene = fsr_scene_now;
        if (scene && (!mv_depth || !mv_img || mv_done_frame != f || mv_last.frame != f || !mv_depth->sampled_view)) {
            ++fsr_skip_nomv;
            return false;
        }
        Image* pt = RenderTarget(mem, st.rt_tex[0], false);
        if (!pt || !src || !fsr_snap) {
            ++fsr_skip_nosrc;
            return false;
        }
        // still frames need no scene data: render size from the snapshot, dummy depth (far) and zero motion vectors are created
        // below, jitter 0 until a scene frame has been seen, default near / far / fov
        Image& d = scene ? *mv_depth : *fsr_snap; // still frames never read d (all scene-only passes are off)
        const u32 rw = scene ? (d.pw ? d.pw : d.width) : (fsr_snap->pw ? fsr_snap->pw : fsr_snap->width);
        const u32 rh = scene ? (d.ph ? d.ph : d.height) : (fsr_snap->ph ? fsr_snap->ph : fsr_snap->height);
        const u32 ow = pt->pw ? pt->pw : pt->width, oh = pt->ph ? pt->ph : pt->height;
        const u32 sw = src->pw ? src->pw : src->width, sh = src->ph ? src->ph : src->height;
        Core::GameSettings::FsrSetSizes(rw, rh, ow, oh);
        if (sw != rw || sh != rh || fsr_snap->width != rw || fsr_snap->height != rh || (scene && (mv_img->width != rw || mv_img->height != rh)) ||
            ow < rw || oh < rh) {
            ++fsr_skip_size;
            fsr_skip_size_text = fmt::format("render {}x{} HUD image {}x{} snapshot {}x{} mv {}x{} output {}x{}", rw, rh, sw, sh, fsr_snap->width,
                                             fsr_snap->height, mv_img->width, mv_img->height, ow, oh);
            return false;
        }
        if (!FsrEnsurePipes() || !FsrEnsureContext(rw, rh, ow, oh)) return false;
        const double t0 = NvnVkNowMs();
        EndPass();
        if (objv_write_ts && objv_ts_pool) vkcmd.ResetQueryPool(*objv_ts_pool, static_cast<u32>(cur) * 8, 8);
        FsrTs(0);
        constexpr VkImageUsageFlags kUse = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        const Image* depth_before = fsr_depth.get();
        FsrEnsureImage(fsr_depth, VK_FORMAT_R32_SFLOAT, rw, rh, kUse | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "depth (R32F copy of the scene depth)");
        if (!scene && fsr_depth.get() != depth_before) {
            VkClearColorValue depth_far{};
            depth_far.float32[0] = 1.0f;
            vkcmd.ClearColorImage(*fsr_depth->image, VK_IMAGE_LAYOUT_GENERAL, depth_far, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
        }
        if (!scene) {
            const Image* mv0_before = fsr_mv0.get();
            FsrEnsureImage(fsr_mv0, VK_FORMAT_R32G32_SFLOAT, rw, rh, kUse | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "zero motion vectors (still frames)");
            if (fsr_mv0.get() != mv0_before) {
                VkClearColorValue zero{};
                vkcmd.ClearColorImage(*fsr_mv0->image, VK_IMAGE_LAYOUT_GENERAL, zero, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
            }
            Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            ++fsr_sec_still;
        }
        Image* camv = scene ? mv_img.get() : fsr_mv0.get(); // camera motion vectors (zero on still frames)
        FsrEnsureImage(fsr_react, VK_FORMAT_R8_UNORM, rw, rh, kUse, "reactive mask (R8)");
        // TRANSFER_DST: FSR 4's output is copied in (nvn_fsr_backend = 4)
        FsrEnsureImage(fsr_out, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, kUse | (fsr4_cmd[0] ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u), "FSR output");
        FsrEnsureImage(fsr_comp, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, kUse, "composite (FSR + HUD)");
        for (u32 i = 0; i < 2; ++i) {
            FsrEnsureImage(fsr_sil[i], VK_FORMAT_R8_UNORM, rw, rh, kUse, "silhouette history (R8)");
            FsrEnsureImage(fsr_hist[i], VK_FORMAT_R8G8B8A8_UNORM, rw, rh, kUse, "input colour history (RGBA8)");
        }
        // camera record of this frame's motion vectors: jitter and depth range (object vectors, FSR)
        const MvFrame& mi = mv_last;
        u32 gw = 0, gh = 0;
        Core::GameSettings::MvRenderSize(&gw, &gh);
        // jitter of the camera record this frame's motion vectors use (game_settings stores it in jitter-grid px, +y up)
        const float jx = mi.paired ? mi.cur.jitter_px[0] : 0.0f, jy = mi.paired ? mi.cur.jitter_px[1] : 0.0f;
        const float ox = gw ? jx * static_cast<float>(rw) / static_cast<float>(gw) : jx; // Halton offset, render px
        const float oy = gh ? jy * static_cast<float>(rh) / static_cast<float>(gh) : jy;
        // picture shift, +x right (m[8] -= 2 ox / w: verified), +y down (image rows); still frames: the shift that is really in
        // the picture (that of the last scene frame when the frame still shows the scene image (scene depth drawn), else 0)
        // still frames: with the camera hook running, the jitter it applied to this frame (live 3D previews in menus are drawn
        // with it; it drops to 0 two frames into a still run, see FsrPresent); without the hook, the picture's own
        // (last scene frame's when a scene image is shown, else 0)
        const float sox = gw ? fsr_still_jit[0] * static_cast<float>(rw) / static_cast<float>(gw) : fsr_still_jit[0];
        const float soy = gh ? fsr_still_jit[1] * static_cast<float>(rh) / static_cast<float>(gh) : fsr_still_jit[1];
        const float still_x = fsr_still_hook ? sox : (fsr_still_has_scene ? fsr_scene_shift[0] : 0.0f);
        const float still_y = fsr_still_hook ? (no_flip_y ? soy : -soy) : (fsr_still_has_scene ? fsr_scene_shift[1] : 0.0f);
        const float shift_x = scene ? ox : still_x;
        const float shift_y = scene ? (no_flip_y ? oy : -oy) : still_y;
        if (scene) {
            fsr_scene_shift[0] = shift_x;
            fsr_scene_shift[1] = shift_y;
        }
        float jit_x = -shift_x, jit_y = -shift_y;         // 0: AMD sample
        if (fsr_sign == 1) jit_x = shift_x, jit_y = shift_y;
        if (fsr_sign == 2) jit_y = shift_y;
        if (fsr_sign == 3) jit_x = shift_x;
        // near / far / vertical fov from the projection itself (row-vector: z_clip = z_v P22 + P32, w = -z_v), depth taken as
        // NDC z in [0, 1] like the motion vector pass: near = P32 / P22, far = P32 / (P22 + 1)
        const float* P = mi.cur.proj;
        const bool depth_ok = mi.paired && P[10] != 0.0f && std::isfinite(P[14] / P[10]) && P[14] / P[10] > 0.0f &&
                              P[10] + 1.0f != 0.0f && P[14] / (P[10] + 1.0f) > P[14] / P[10];
        float cam_near = mi.paired && P[10] != 0.0f ? P[14] / P[10] : 0.0f;
        float cam_far = mi.paired && P[10] + 1.0f != 0.0f ? P[14] / (P[10] + 1.0f) : 0.0f;
        float fov = mi.paired && P[5] != 0.0f ? 2.0f * std::atan(1.0f / std::fabs(P[5])) : 0.0f;
        if (!std::isfinite(cam_near) || !std::isfinite(cam_far) || cam_near <= 0.0f || cam_far <= cam_near) {
            cam_near = 8.0f;
            cam_far = 1.0e6f;
        }
        if (!std::isfinite(fov) || fov <= 0.01f || fov >= 3.1f) fov = 0.873f;
        // reactive mask = silhouette (Numpad2, this frame's dynamic-object mask target when one was found) and the
        // previous frame's silhouette at the reprojected texel, max the motion mask (Numpad8); see vulkan_nvn_fsr_prep.comp
        // the mask target is bound whenever found (its silhouette is history for the object vectors); bit 4 = reactive
        Image* mask = fsr_mask_img;
        const bool react_on = fsr_reactive || fsr_motion || fsr_objmv || fsr_anim;
        // history from the previous FSR frame (same sizes: FsrEnsureImage keeps the images while the sizes hold)
        const bool hist_ok = fsr_hist_frame != 0 && fsr_hist_frame + 1 == f && !fsr_need_reset && (!scene || (mv_last.paired && !mv_last.reset));
        const u32 rd = fsr_hist_idx, wr = fsr_hist_idx ^ 1u;
        if (fsr_reactive) ++(mask ? fsr_sec_mask_bound : fsr_sec_mask_none);
        if (react_on) ++fsr_sec_react_bound;
        const u32 bw = (rw + 7) / 8, bh = (rh + 7) / 8;
        for (u32 i = 0; i < 2; ++i) {
            FsrEnsureImage(fsr_blk[i], VK_FORMAT_R16G16B16A16_SFLOAT, bw, bh, kUse, "object motion blocks (RGBA16F, 8x8 px each)");
            FsrEnsureImage(fsr_objw[i], VK_FORMAT_R8G8B8A8_UNORM, rw, rh, kUse, "object motion weight / cloth reactive (RGBA8)");
        }
        FsrEnsureImage(fsr_mv, VK_FORMAT_R32G32_SFLOAT, rw, rh, kUse, "motion vectors with object motion (RG32F)");
        // picture shift change since the previous FSR frame (block matching sees it on top of the motion)
        const bool shift_prev_ok = fsr_prev_shift_frame != 0 && fsr_prev_shift_frame + 1 == f;
        const float jd_x = shift_prev_ok ? fsr_prev_shift[0] - shift_x : 0.0f, jd_y = shift_prev_ok ? fsr_prev_shift[1] - shift_y : 0.0f;
        const float shift_prev[2] = {fsr_prev_shift[0], fsr_prev_shift[1]};
        const float shift_cur[2] = {shift_x, shift_y};
        fsr_prev_shift[0] = shift_x;
        fsr_prev_shift[1] = shift_y;
        fsr_prev_shift_frame = f;
        const bool obj_prev_ok = fsr_obj_frame != 0 && fsr_obj_frame + 1 == f;
        // 0. object motion: block search, then per-texel vectors (see vulkan_nvn_fsr_objmv_*.comp)
        const bool objmv_now = fsr_objmv && scene;
        if (objmv_now) {
            Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            const struct {
                u32 flags;
                float jd_x, jd_y, p22, p32, near_limit, chg_thr, pad;
            } spc{(mask ? 1u : 0u) | (hist_ok ? 2u : 0u) | (obj_prev_ok ? 4u : 0u) | (depth_ok ? 8u : 0u), jd_x, jd_y,
                  depth_ok ? mi.cur.proj[10] : 0.0f, depth_ok ? mi.cur.proj[14] : 0.0f, fsr_objmv_near, 0.12f, 0.0f};
            const std::array<VkDescriptorImageInfo, 8> sdesc{{
                {*fsr_near, *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_hist[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *mv_img->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, mask ? *mask->view : *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_sil[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *d.sampled_view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_blk[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_blk[wr]->view, VK_IMAGE_LAYOUT_GENERAL},
            }};
            if (fsr_bm_compact_ok && FsrEnsureBmList(bw * bh)) {
                // nvn_fsr_bm_compact: steps 0-2 of the search for every block (writes the blocks that are not searched and
                // lists the others), then the search steps for the listed blocks only (indirect dispatch, count from the list)
                std::array<FsrDesc, 9> cdesc{};
                for (u32 i = 0; i < 8; ++i) cdesc[i].img = sdesc[i];
                cdesc[8].buf = {*fsr_bm_list, 0, VK_WHOLE_SIZE};
                // the list was last read by the previous frame's search (shader + indirect args): reset it after that
                Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
                vkcmd.FillBuffer(*fsr_bm_list, 0, 4, 0); // x = 0 blocks
                vkcmd.FillBuffer(*fsr_bm_list, 4, 8, 1); // y = z = 1
                Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_objc.pipeline);
                vkcmd.PushConstants(*fsr_objc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
                vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_objc.tmpl, *fsr_objc.layout, 0, cdesc.data());
                vkcmd.Dispatch(bw, bh, 1);
                GpMarkNamed("fsr_bm_classify");
                Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
                vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_objl.pipeline);
                vkcmd.PushConstants(*fsr_objl.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
                vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_objl.tmpl, *fsr_objl.layout, 0, cdesc.data());
                vkcmd.DispatchIndirect(*fsr_bm_list, 0);
            } else {
                vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_objs.pipeline);
                vkcmd.PushConstants(*fsr_objs.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
                vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_objs.tmpl, *fsr_objs.layout, 0, sdesc.data());
                vkcmd.Dispatch(bw, bh, 1);
            }
            GpMarkNamed("fsr_bm_search"); // gpu_diagnostics: the search alone; fsr_blockmatch is then the apply pass
            Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            const struct {
                u32 flags;
                float jd_x, jd_y, chg_lo, chg_hi, pad0, pad1, pad2;
            } apc{1u | (hist_ok ? 2u : 0u) | (obj_prev_ok ? 4u : 0u), jd_x, jd_y, 0.10f, 0.25f, 0.0f, 0.0f, 0.0f};
            const std::array<VkDescriptorImageInfo, 8> adesc{{
                {*fsr_near, *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_hist[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *mv_img->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_blk[wr]->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_mv->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_objw[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_objw[wr]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_linear, *fsr_hist[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
            }};
            vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_obja.pipeline);
            vkcmd.PushConstants(*fsr_obja.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(apc), &apc);
            vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_obja.tmpl, *fsr_obja.layout, 0, adesc.data());
            vkcmd.Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
            fsr_obj_frame = f;
        }
        FsrTs(1);
        // 0b. geometry object vectors (nvn_fsr_objvel): velocity pass + merge over the camera / block-matched vectors
        const bool stat_now = !fsr_rstat_pending && NvnVkNowMs() - fsr_rstat_ms >= 1000.0;
        const double objv_t1 = NvnVkNowMs();
        // geometry object vectors: scene frames, and menu-3D still frames whose opaque meshes were captured (save screen /
        // previews: animated models get real vectors; zero camera vectors as the base, validated against their own depth)
        const bool menu_vel = !scene && objv_menu_frame == f && objv_menu_depth && objv_menu_depth->sampled_view;
        const bool vel = (scene || menu_vel) &&
                         ObjvRun(f, rw, rh, scene ? d : *objv_menu_depth, shift_cur, shift_prev, shift_prev_ok, scene && depth_ok, P[10], P[14],
                                 objmv_now, stat_now, mask, *camv);
        objv_sec_run_ms += NvnVkNowMs() - objv_t1;
        FsrTs(2);
        // 0c. animated surfaces (nvn_fsr_animsurf): static surfaces whose low-passed colour stepped (texture flipbooks)
        bool anim = false;
        anim_stat_want = false;
        // still frames too: whatever changes in a still picture (animated 3D previews, menu animations) gets the bypass
        if (fsr_anim && hist_ok && camv) {
            const u32 tw = (rw + 7) / 8, th2 = (rh + 7) / 8;
            for (u32 i = 0; i < 2; ++i) FsrEnsureImage(anim_tile[i], VK_FORMAT_R8G8B8A8_UNORM, tw, th2, kUse, "animated-surface tiles (RGBA8, 8x8 px each)");
            FsrEnsureImage(anim_stat, VK_FORMAT_R32_UINT, 8, 1, kUse | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "animated-surface counters (R32UI)");
            if (stat_now) {
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkClearColorValue zero{};
                vkcmd.ClearColorImage(*anim_stat->image, VK_IMAGE_LAYOUT_GENERAL, zero, range);
            }
            Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            const bool tiles_prev = anim_frame != 0 && anim_frame + 1 == f;
            const std::array<VkDescriptorImageInfo, 8> adesc2{{
                {*fsr_linear, *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_linear, *fsr_hist[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, vel ? *objv_vel->view : *camv->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *camv->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *anim_tile[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *anim_tile[wr]->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *anim_stat->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, scene ? *d.sampled_view : *fsr_depth->view, VK_IMAGE_LAYOUT_GENERAL},
            }};
            // flags: bit 3 edge gate (flat depth), bit 4 linear depth from the projection
            const struct {
                u32 flags;
                float jd_x, jd_y, thr, p22, p32, pad0, pad1;
            } apc2{(stat_now ? 1u : 0u) | (tiles_prev ? 2u : 0u) | (vel ? 4u : 0u) | (fsr_anim_gate && scene ? 8u : 0u) | (depth_ok && scene ? 16u : 0u),
                   jd_x, jd_y, fsr_anim_thr, depth_ok ? P[10] : 0.0f, depth_ok ? P[14] : 0.0f, 0.0f, 0.0f};
            vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_anim_pipe.pipeline);
            vkcmd.PushConstants(*fsr_anim_pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(apc2), &apc2);
            vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_anim_pipe.tmpl, *fsr_anim_pipe.layout, 0, adesc2.data());
            vkcmd.Dispatch(tw, th2, 1);
            // tiles / counters written -> prep and composite (compute), the counter stats copy (transfer)
            Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
            anim = true;
            anim_frame = f;
            anim_wr = wr;
            anim_stat_want = stat_now;
            ++anim_sec_frames;
        } else if (fsr_anim) {
            anim_frame = 0; // no previous tiles next frame
        }
        FsrTs(3);
        // 1. prep: depth -> R32F, reactive mask, history (bit 5: object weight / cloth reactive from pass 0)
        Barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        {
            const std::array<VkDescriptorImageInfo, 13> desc{{
                {*fsr_near, scene ? *d.sampled_view : *fsr_depth->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, mask ? *mask->view : *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_depth->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_react->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_sil[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_sil[wr]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_hist[rd]->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_hist[wr]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *camv->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, *fsr_objw[wr]->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, vel ? *objv_vel->view : *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_near, anim ? *anim_tile[wr]->view : *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
            }};
            // flags: bit 0 silhouette target bound, 1 motion mask, 2 history valid, 4 reactive silhouette (current + previous),
            // 5 object weight / cloth reactive valid, 6 geometry object vectors valid this frame (reactive capped there).
            // Motion mask edges 0.06 .. 0.20 (LDR input; colour outside the previous 3x3 box)
            const struct {
                u32 flags;
                float strength, mot_lo, mot_hi, vel_cap, pad0, pad1, pad2;
            } pc{(mask ? 1u : 0u) | (fsr_motion ? 2u : 0u) | (hist_ok ? 4u : 0u) | (fsr_reactive ? 16u : 0u) |
                     (objmv_now ? 32u : 0u) | (vel ? 64u : 0u) | (anim ? 128u : 0u) | (scene ? 0u : 256u),
                 fsr_react_strength, 0.06f, 0.20f, objv_react_cap, 0.95f, 0.95f, 0.0f};
            vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_prep.pipeline);
            vkcmd.PushConstants(*fsr_prep.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_prep.tmpl, *fsr_prep.layout, 0, desc.data());
            vkcmd.Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
        }
        fsr_hist_idx = wr;
        fsr_hist_frame = f;
        FsrTs(4);
        Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        // once a second: reactive mask + silhouette to the host for the log line (FsrReactPoll at present)
        if (!fsr_rstat_pending && NvnVkNowMs() - fsr_rstat_ms >= 1000.0) {
            // layout: reactive (R8), silhouette (R8), then (8-byte aligned) object weight (RGBA8) and blocks (RGBA16F)
            const size_t off_w = (size_t{rw} * rh * 2 + 7) & ~size_t{7};
            const size_t off_b = (off_w + size_t{rw} * rh * 4 + 7) & ~size_t{7};
            const size_t bytes = off_b + size_t{bw} * bh * 8;
            // [Debug] nvn_fsr_texel_stats: the full-size reactive / silhouette / object images (~12 MB at 1080p) and the
            // CPU loops over them only when asked for; the small animsurf / object-velocity counters below always.
            fsr_rstat_texels = opt_texel_stats;
            if (fsr_rstat_texels && (!fsr_rstat_buf || fsr_rstat_bytes != bytes)) {
                fsr_rstat_buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                        .pQueueFamilyIndices = nullptr},
                                                       MemoryUsage::Download);
                fsr_rstat_bytes = bytes;
            }
            const VkBufferImageCopy r0{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {rw, rh, 1}};
            const VkBufferImageCopy r1{size_t{rw} * rh, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {rw, rh, 1}};
            if (fsr_rstat_texels) vkcmd.CopyImageToBuffer(*fsr_react->image, VK_IMAGE_LAYOUT_GENERAL, *fsr_rstat_buf, r0);
            if (fsr_rstat_texels) vkcmd.CopyImageToBuffer(*fsr_sil[wr]->image, VK_IMAGE_LAYOUT_GENERAL, *fsr_rstat_buf, r1);
            if (fsr_objmv && fsr_rstat_texels) {
                const VkBufferImageCopy r2{off_w, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {rw, rh, 1}};
                const VkBufferImageCopy r3{off_b, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {bw, bh, 1}};
                vkcmd.CopyImageToBuffer(*fsr_objw[wr]->image, VK_IMAGE_LAYOUT_GENERAL, *fsr_rstat_buf, r2);
                vkcmd.CopyImageToBuffer(*fsr_blk[wr]->image, VK_IMAGE_LAYOUT_GENERAL, *fsr_rstat_buf, r3);
            }
            fsr_rstat_obj = fsr_objmv;
            anim_stat_pending = false;
            if (anim_stat_want && anim_stat) {
                if (!anim_stat_buf) {
                    anim_stat_buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                            .size = 32, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                            .pQueueFamilyIndices = nullptr},
                                                           MemoryUsage::Download);
                }
                const VkBufferImageCopy r5{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {8, 1, 1}};
                vkcmd.CopyImageToBuffer(*anim_stat->image, VK_IMAGE_LAYOUT_GENERAL, *anim_stat_buf, r5);
                anim_stat_pending = true;
            }
            objv_stat_pending = false;
            if (objv_stat_want && objv_stat) {
                if (!objv_stat_buf) {
                    objv_stat_buf = allocator.CreateBuffer({.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = nullptr, .flags = 0,
                                                            .size = 32, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .queueFamilyIndexCount = 0,
                                                            .pQueueFamilyIndices = nullptr},
                                                           MemoryUsage::Download);
                }
                const VkBufferImageCopy r4{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {8, 1, 1}};
                vkcmd.CopyImageToBuffer(*objv_stat->image, VK_IMAGE_LAYOUT_GENERAL, *objv_stat_buf, r4);
                objv_stat_pending = true;
                objv_stat_frame = f;
            }
            fsr_rstat_off_w = off_w;
            fsr_rstat_off_b = off_b;
            fsr_rstat_bw = bw;
            fsr_rstat_bh = bh;
            Barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
            fsr_rstat_pending = true;
            fsr_rstat_slot = cur;
            fsr_rstat_w = rw;
            fsr_rstat_h = rh;
            fsr_rstat_frame = f;
            fsr_rstat_bound = react_on;
            fsr_rstat_ms = NvnVkNowMs();
        }
        // 2. FSR
        const double now_ms = NvnVkNowMs();
        const float dt = fsr_prev_dispatch_ms > 0.0 ? static_cast<float>(std::clamp(now_ms - fsr_prev_dispatch_ms, 1.0, 100.0)) : 16.7f;
        fsr_prev_dispatch_ms = now_ms;
        const bool gap = fsr_last_frame + 1 != f;
        // still frames: no camera record for these pixels; resets only for real discontinuities (FsrMenuUpdate sets
        // fsr_need_reset when the picture changes between scene / pure 2D)
        const bool reset = fsr_need_reset || gap || (scene && (!mi.paired || mi.reset));
        // R32G32_SFLOAT, render px, cur + mv = prev (FSR's convention); with object vectors (Numpad9) the image pass 0 wrote
        // with geometry object vectors (nvn_fsr_objvel) the merge's output (objv_mvo)
        Image* const mv_in = vel ? objv_mvo.get() : objmv_now ? fsr_mv.get() : camv;
        fsr_last_mv_img = mv_in;
        fsr_last_react_on = react_on;
        bool ran4 = false;
#ifdef SUYU_HAS_FSR4_BRIDGE
        // nvn_fsr_backend = 4: the same inputs to FSR 4 (falls through to FSR 3.1 on any failure)
        {
            NvnFsr4::DispatchParams p4{};
            p4.reactive = react_on;
            p4.jitter_x = jit_x;
            p4.jitter_y = jit_y;
            p4.render_w = rw;
            p4.render_h = rh;
            p4.output_w = ow;
            p4.output_h = oh;
            p4.frame_ms = dt;
            p4.camera_near = cam_near;
            p4.camera_far = cam_far;
            p4.fov_y = fov;
            p4.view_to_meters = 0.01f; // as FSR 3.1 below
            p4.sharpen = fsr_sharpen;
            p4.sharpness = std::clamp(fsr_sharpness, 0.0f, 1.0f);
            p4.reset = reset;
            ran4 = Fsr4Run(mv_in, react_on, p4);
            if (!ran4 && fsr4_ok && fsr4_want) ++fsr4_sec_fb;
        }
#endif
        if (!ran4) {
        FfxVkPortableUpscaleDispatchInfo di{};
        di.structSize = sizeof(di);
        di.commandBuffer = *vkcmd;
        di.color = FsrImg(fsr_snap.get());
        di.color.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        di.depth = FsrImg(fsr_depth.get());
        di.motionVectors = FsrImg(mv_in);
        di.exposure = FsrImg(nullptr);
        di.reactiveMask = FsrImg(react_on ? fsr_react.get() : nullptr);
        di.transparencyAndCompositionMask = FsrImg(nullptr);
        di.output = FsrImg(fsr_out.get());
        di.jitterOffset = {jit_x, jit_y};
        di.motionVectorScale = {1.0f, 1.0f};
        di.renderSize = {rw, rh};
        di.outputSize = {ow, oh};
        di.frameTimeMilliseconds = dt;
        di.preExposure = 1.0f;
        di.cameraNear = cam_near;
        di.cameraFar = cam_far;
        di.cameraVerticalFovRadians = fov;
        di.viewSpaceToMeters = 0.01f; // MT Framework units assumed to be centimetres (unverified)
        di.sharpness = std::clamp(fsr_sharpness, 0.0f, 1.0f);
        di.enableSharpening = fsr_sharpen ? VK_TRUE : VK_FALSE;
        di.reset = reset || fsr4_ran_last ? VK_TRUE : VK_FALSE; // the history is FSR 4's after a backend switch
        di.frameId = ++fsr_frame_id;
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&fsr_ci, &di); issues != 0) {
            ++fsr_errors;
            fsr_last_error = "dispatch rejected: " + FsrIssuesText(issues);
            return false;
        }
        if (const FfxVkPortableResult r = ffxVkPortableUpscaleContextRecordDispatch(fsr_ctx, &di); r != FFX_VK_PORTABLE_OK) {
            ++fsr_errors;
            fsr_last_error = "dispatch failed: " + FsrResultText(r);
            if (++fsr_error_run >= 30) FsrFail("30 FSR dispatches in a row failed: " + fsr_last_error);
            fsr_need_reset = true;
            return false;
        }
        fsr_error_run = 0;
        } // FSR 3.1
        fsr4_ran_last = ran4;
        FsrTs(5);
        // 3. composite at output size. Producers since the last render pass (whose EndPass barrier already covers the game's and
        // the HUD layer's attachment writes): FSR 3.1 (compute) or FSR 4's output copy (transfer), prep / animsurf (compute)
        Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        {
            // HUD layer path: binding 2 is the layer (bilinear, premultiplied) instead of the snapshot
            const bool layer = fsr_layer_used && fsr_layer;
            const bool bypass = fsr_anim_bypass && anim;
            const bool tm = layer && fsr_tm_used && fsr_tm && fsr_layer && fsr_tm->width == fsr_layer->width && fsr_tm->height == fsr_layer->height;
            const std::array<VkDescriptorImageInfo, 7> desc{{
                {*fsr_near, *fsr_out->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_linear, *src->view, VK_IMAGE_LAYOUT_GENERAL},
                layer ? VkDescriptorImageInfo{*fsr_linear, *fsr_layer->view, VK_IMAGE_LAYOUT_GENERAL}
                      : VkDescriptorImageInfo{*fsr_near, *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, *fsr_comp->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_linear, *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_linear, bypass ? *anim_tile[anim_wr]->view : *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
                {*fsr_linear, tm ? *fsr_tm->view : *fsr_snap->view, VK_IMAGE_LAYOUT_GENERAL},
            }};
            // flags: bit 0 layer path, bit 2 state indicator (not in the frame a Numpad6 dump is taken of), bits 4-5 HUD path
            // (1 layer, 2 diff), bit 6 reactive silhouette, bit 7 motion mask, bits 8-9 jitter convention, bit 10 object vectors
            const bool ind = fsr_indicator && !fsr_dump_req;
            const u32 flags = (layer ? 1u : 0u) | (ind ? 4u : 0u) | ((layer ? 1u : 2u) << 4) | (fsr_reactive ? 64u : 0u) | (fsr_motion ? 128u : 0u) |
                              (static_cast<u32>(fsr_sign & 3) << 8) | (fsr_objmv ? 1024u : 0u) | (objv_on && objv_ok ? 2048u : 0u) | (fsr_anim ? 4096u : 0u) | (bypass ? 8192u : 0u) | (tm ? 16384u : 0u) |
                              (fsr4 && fsr4_ok && fsr4_want && fsr4_ran_last ? 32768u : 0u);
            // bit 15: FSR 4 ran this frame (indicator square 8); bit 13: animated-surface bypass (current input, jitter removed: sampled at + the picture shift)
            const std::array<u32, 8> pc{fsr_view, std::bit_cast<u32>(fsr_hud_lo), std::bit_cast<u32>(fsr_hud_hi), flags,
                                        std::bit_cast<u32>(shift_x), std::bit_cast<u32>(shift_y), std::bit_cast<u32>(0.85f),
                                        std::bit_cast<u32>(0.5f)};
            vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *fsr_compose.pipeline);
            vkcmd.PushConstants(*fsr_compose.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc.data());
            vkcmd.PushDescriptorSetWithTemplateKHR(*fsr_compose.tmpl, *fsr_compose.layout, 0, desc.data());
            vkcmd.Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
        }
        // composite written -> the final draw samples it (its vertex or fragment stage), a dump copies it (transfer). The next
        // frame's composite write (write-after-read) is ordered by the EndPass barrier after the final draw's render pass
        Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        FsrTs(6);
        if (objv_write_ts && objv_ts_pool && cur < fsr_ts_pending.size()) fsr_ts_pending[cur] = true;
        fsr_last_frame = f;
        fsr_need_reset = false;
        ++fsr_dispatched;
        if (reset) ++fsr_resets;
        const double rec = NvnVkNowMs() - t0;
        fsr_rec_sum += rec;
        fsr_rec_max = std::max(fsr_rec_max, rec);
        fsr_last = {f, ox, oy, jit_x, jit_y, reset, mi.paired, mi.reset, gap, cam_near, cam_far, fov, dt, rw, rh, ow, oh};
        return true;
    }

    // "Current mode: ..." (nvn_fsr.txt and the suyu log): once a second (diagnostics gates only) and after every Numpad key
    void FsrModeLine(bool periodic = false) {
        if (periodic && !fsr_chatty) return; // the once-a-second call: nothing is written without the diagnostics gates
        const char* path = !fsr_active || fsr_failed ? "no FSR"
                           : fsr_menu                ? "menu-skip"
                           : fsr_frame_path == 1     ? "HUD layer"
                           : fsr_frame_path == 2     ? "HUD diff"
                           : fsr_frame_path == 3     ? "menu-skip"
                                                     : (fsr_hudlayer ? "HUD layer chosen, but FSR ran on no frame in the last second"
                                           : "HUD diff chosen, but FSR ran on no frame in the last second");
        static constexpr const char* kView[4] = {"view 0 composite", "view 1 FSR only", "view 2 mask tint", "view 3 game picture"};
        const std::string line = fmt::format(
            "Current mode: FSR {} | backend {} | {} | {} | reactive {} | jitter conv {} | sharpen {} | motionmask {} | objmv {} | objvel {} | animsurf {}",
            fsr_active && !fsr_failed ? "on" : "off", FsrBackendText(), path, kView[fsr_view & 3], fsr_reactive ? "on" : "off", fsr_sign,
            fsr_sharpen ? fmt::format("on {:.2f}", fsr_sharpness) : std::string("off"), fsr_motion ? "on" : "off", fsr_objmv ? "on" : "off",
            !objv_ok ? std::string(objv_mode == 0 ? "off (ini 0)" : "unavailable") : objv_on ? fmt::format("on (mode {})", objv_mode) : std::string("off"),
            fsr_anim ? fmt::format("on (thr {:.3f}, edge gate {}, bypass {})", fsr_anim_thr, fsr_anim_gate ? "on" : "off",
                                   fsr_anim_bypass ? "on" : "off")
                     : std::string("off"));
        FsrLog(line);
        LOG_INFO(Render_Vulkan, "nvn_fsr: {}", line);
    }

    // Present: the reactive mask statistics copied once a second by FsrDispatch, once their submit has finished
    void FsrReactPoll() {
        if (!fsr_rstat_pending || frames[fsr_rstat_slot].fence.GetStatus() != VK_SUCCESS) return;
        fsr_rstat_pending = false;
        const u8* v = nullptr;
        const size_t n = size_t{fsr_rstat_w} * fsr_rstat_h;
        u64 r_half = 0, r_any = 0, s_half = 0;
        if (fsr_rstat_texels) {
            fsr_rstat_buf.Invalidate();
            v = reinterpret_cast<const u8*>(fsr_rstat_buf.Mapped().data());
            for (size_t i = 0; i < n; ++i) {
                r_half += v[i] > 127;
                r_any += v[i] > 12;
                s_half += v[n + i] > 127;
            }
        }
        const double k = n ? 100.0 / static_cast<double>(n) : 0.0;
        if (fsr_rstat_texels) FsrLogSec(fmt::format("frame {} reactive: mask bound to FSR {} (this frame), silhouette target {} | last second: silhouette target "
                           "found {} / missing {} frames (Numpad2 on), reactive bound {} frames | texels: silhouette > 0.5 {:.2f}%, "
                           "reactive > 0.5 {:.2f}%, reactive > 0.05 {:.2f}% (reactive = 0.9 x max(silhouette, previous silhouette, motion))",
                           fsr_rstat_frame, fsr_rstat_bound ? "yes" : "no", fsr_reactive ? fmt::format("{:x}", fsr_mask_key) : std::string("off"),
                           fsr_sec_mask_bound, fsr_sec_mask_none, fsr_sec_react_bound, s_half * k, r_half * k, r_any * k));
        fsr_sec_mask_bound = fsr_sec_mask_none = fsr_sec_react_bound = 0;
        if (anim_stat_pending && anim_stat_buf) {
            anim_stat_pending = false;
            anim_stat_buf.Invalidate();
            u32 a[8]{};
            std::memcpy(a, anim_stat_buf.Mapped().data(), sizeof(a));
            const double ev = a[1] ? double(a[1]) : 1.0;
            FsrLogSec(fmt::format("frame {} animated surfaces: of {} evaluated tiles: passed {} ({:.2f}%; full weight {:.2f}%, first-flag weight "
                               "0.35 {:.2f}%), rejected by the edge/depth gate {} ({:.2f}%; gate {}) (threshold {:.3f}, 3x3 min search, >= 40% "
                               "of texels); mean frames between passes of a tile {:.2f} (from {} repeats <= 60 frames; a 24-30 fps flipbook at "
                               "120 fps = 4-5)",
                               fsr_rstat_frame, a[1], a[0], 100.0 * a[0] / ev, 100.0 * a[5] / ev, 100.0 * (a[0] - std::min(a[0], a[5])) / ev, a[4],
                               100.0 * a[4] / ev, fsr_anim_gate ? "on" : "off", fsr_anim_thr, a[3] ? double(a[2]) / a[3] : 0.0, a[3]));
        }
        if (objv_stat_pending && objv_stat_buf) {
            // geometry object vectors of the same frame (vulkan_nvn_fsr_objvel_merge.comp counters)
            objv_stat_pending = false;
            objv_stat_buf.Invalidate();
            u32 c[8]{};
            std::memcpy(c, objv_stat_buf.Mapped().data(), sizeof(c));
            const double tex = std::max<double>(1.0, objv_stat_texels);
            const double val = std::max<double>(1.0, c[0]);
            FsrLogSec(fmt::format("frame {} object velocity texels: valid {:.2f}% of the picture ({} draws paired) | of the valid ones: "
                               "moving against the camera vector > 0.5 px {:.1f}%, within 0.25 px of it {:.1f}% | mean |object - camera| "
                               "{:.3f} px, mean |object| {:.3f} px, in a cloth pose update {:.2f}% | coverage by area: object vectors on "
                               "{:.2f}% of the picture, camera vectors on {:.2f}%; dynamic-object mask area {:.2f}% of the picture, "
                               "{:.1f}% of it with an object vector",
                               objv_stat_frame, 100.0 * c[0] / tex, objv_stat_pairs, 100.0 * c[1] / val, 100.0 * c[2] / val,
                               c[3] / 16.0 / val, c[4] / 16.0 / val, 100.0 * c[7] / val, 100.0 * c[0] / tex, 100.0 - 100.0 * c[0] / tex,
                               100.0 * c[5] / tex, c[5] ? 100.0 * c[6] / c[5] : 0.0));
        }
        if (!fsr_rstat_obj || !fsr_rstat_texels) return;
        // object motion sample of the same frame: blocks searched / confident / cloth hold, texels using object vectors
        const auto half = [](u16 h) {
            const u32 e = (h >> 10) & 31u, m = h & 1023u;
            const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                            : e == 31 ? std::numeric_limits<float>::infinity()
                                      : std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
            return (h & 0x8000u) ? -v : v;
        };
        const size_t nb = size_t{fsr_rstat_bw} * fsr_rstat_bh;
        u64 searched = 0, confident = 0, hold = 0, jump = 0;
        double mag_sum = 0.0;
        for (size_t i = 0; i < nb; ++i) {
            u16 q[4];
            std::memcpy(q, v + fsr_rstat_off_b + i * 8, 8);
            const float conf = half(q[2]), a = half(q[3]);
            searched += conf >= 0.0f;
            if (conf > 0.5f) {
                ++confident;
                mag_sum += std::hypot(half(q[0]), half(q[1]));
            }
            hold += a > 0.0f;
            jump += a > 0.99f;
        }
        u64 w_half = 0, c_half = 0, f_half = 0;
        for (size_t i = 0; i < n; ++i) {
            f_half += v[fsr_rstat_off_w + i * 4] > 127;
            c_half += v[fsr_rstat_off_w + i * 4 + 1] > 127;
            w_half += v[fsr_rstat_off_w + i * 4 + 2] > 127;
        }
        const double kb = nb ? 100.0 / static_cast<double>(nb) : 0.0;
        FsrLogSec(fmt::format("frame {} object vectors: blocks searched {:.2f}% (of {}), confident {:.2f}% (mean |mv| {:.2f} px), cloth "
                           "jump {:.2f}% / hold {:.2f}% | texels: object vector weight > 0.5 {:.2f}%, history fit > 0.5 {:.2f}% (silhouette "
                           "reactive lowered), cloth reactive > 0.5 {:.2f}%",
                           fsr_rstat_frame, searched * kb, nb, confident * kb, confident ? mag_sum / confident : 0.0, jump * kb, hold * kb,
                           w_half * k, f_half * k, c_half * k));
    }

    // Present (render thread): keys, the jitter switch, once-a-second log line, dumps.
    void FsrTs(u32 i) {
        if (gp_on) {
            static constexpr const char* kFsrGp[7] = {"fsr_start", "fsr_blockmatch", "fsr_objvel_merge", "fsr_animsurf", "fsr_prep", "fsr_upscale", "fsr_composite"};
            GpMarkNamed(kFsrGp[i < 7 ? i : 0]);
        }
        if (objv_write_ts && objv_ts_pool) objv_write_ts(*vkcmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *objv_ts_pool, static_cast<u32>(cur) * 8 + i);
    }

    // GPU time per FSR stage of every finished frame slot (timestamps 0..6 of FsrDispatch)
    void FsrTsPoll() {
        if (!objv_ts_pool) return;
        for (u32 s = 0; s < fsr_ts_pending.size() && s < frames.size(); ++s) {
            if (!fsr_ts_pending[s] || frames[s].fence.GetStatus() != VK_SUCCESS) continue;
            fsr_ts_pending[s] = false;
            u64 t[7]{};
            if (dev.GetQueryResults(*objv_ts_pool, s * 8, 7, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) continue;
            bool ok = true;
            for (u32 i = 1; i < 7; ++i) ok = ok && t[i] >= t[i - 1];
            if (!ok) continue;
            for (u32 i = 0; i < 6; ++i) fsr_ts_sum[i] += double(t[i + 1] - t[i]) * objv_ts_period / 1e6;
            ++fsr_ts_n;
        }
    }

    // ---- automatic benchmark ([Debug] nvn_bench = 1; needs gpu_diagnostics = true and nvn_fsr = on) -------------------------
    // Numpad * or Numpad - starts / aborts a run, Numpad + cycles the scene label (village_stand, village_move, quest; ini nvn_bench_scene).
    // A run walks a fixed list of states, each = warm-up seconds (excluded) + measure seconds. State changes happen on the
    // per-second boundary of nvn_perf.txt (Prof::PerfSecond), so every S line belongs to exactly one state; the S line carries
    // " bench=<run>:<idx>:<p|w|m>:<name> bscene=<scene> bad=<frames not scene kind> drift=<0|1>" (p = partial second after
    // the key, w = warm-up, m = measure). Every state sets ALL toggles explicitly; the user's runtime values are restored.
    struct BenchState {
        const char* name;
        int backend; // 0 FSR off, 1 FSR 3.1, 2 FSR 4
        bool geo;    // geometry object vectors (transform feedback capture)
        bool bm;     // 8x8 block matching of the FSR input
    };
    static constexpr BenchState kBenchStates[] = {
        {"fsr_off", 0, true, true},       {"fsr3.1", 1, true, true},     {"fsr3.1_bm0", 1, true, false}, {"fsr4", 2, true, true},
        {"fsr4_bm0", 2, true, false},     {"fsr4_geo0", 2, false, true}, {"fsr3.1_geo0", 1, false, true}, {"fsr_off", 0, true, true},
    };
    static constexpr int kBenchStateCount = static_cast<int>(sizeof(kBenchStates) / sizeof(kBenchStates[0]));

    bool BenchUsable(int i) const {
        const BenchState& b = kBenchStates[i];
        if (b.backend == 2 && !fsr4_ok) return false;   // FSR 4 not initialised: would silently run 3.1
        if (!b.geo && !objv_ok) return false;           // geometry vectors unavailable: identical to the default state
        return true;
    }
    int BenchNextUsable(int from) const {
        for (int i = from; i < kBenchStateCount; ++i) {
            if (BenchUsable(i)) return i;
        }
        return -1;
    }
    void BenchSetStatus(const std::string& s) {
        std::lock_guard bench_lock(g_bench_status_mutex);
        g_bench_status = s;
    }
    void BenchLog(const std::string& line) {
        FsrLog("bench: " + line);
        prof.PerfLine("B " + line);
    }
    void BenchInit() {
        bench_init_done = true;
        const auto flag = [](const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; };
        if (!flag(Core::GameSettings::Value("Debug", "nvn_bench"))) return;
        if (!prof.perf.is_open()) {
            FsrLog("bench: [Debug] nvn_bench = 1 needs gpu_diagnostics = true (nvn_perf.txt); the benchmark is OFF");
            return;
        }
        const auto num = [](const char* key, int def, int lo, int hi) {
            const std::string v = Core::GameSettings::Value("Debug", key);
            if (v.empty()) return def;
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            return (end == v.c_str() || *end != 0) ? def : static_cast<int>(std::clamp<long>(n, lo, hi));
        };
        bench_warm = num("nvn_bench_warmup", 3, 1, 30);
        bench_measure = num("nvn_bench_measure", 15, 5, 120);
        std::string sc = Core::GameSettings::Value("Debug", "nvn_bench_scene");
        for (char& c : sc) {
            if (c == ' ' || c == '=' || c == ':') c = '_';
        }
        bench_scene = sc.empty() ? "village_stand" : sc;
        bench_enabled = true;
        FsrLog(fmt::format("bench: ON. Numpad * or Numpad - starts / aborts a run, Numpad + cycles the scene (now '{}'). {} states x ({} s warm-up + {} s measure)",
                           bench_scene, kBenchStateCount, bench_warm, bench_measure));
    }
    void BenchApply(int idx) {
        const BenchState& b = kBenchStates[idx];
        fsr_active = b.backend != 0;
        fsr4_want = b.backend == 2;
        objv_on = objv_ok && b.geo;
        fsr_objmv = b.bm;
        // everything else is pinned to the ini values so nothing carries over from key presses
        fsr_reactive = Core::GameSettings::NvnFsrReactive();
        fsr_motion = Core::GameSettings::NvnFsrMotionMask();
        fsr_anim = Core::GameSettings::NvnFsrAnimSurf();
        fsr_hudlayer = Core::GameSettings::NvnFsrHudLayer();
        fsr_sign = Core::GameSettings::NvnFsrJitterConv();
        fsr_view = 0;
        fsr_sharpness = fsr_sharpness_ini;
        fsr_sharpen = fsr_sharpness_ini > 0.0f;
        fsr_sharp_idx = fsr_sharpen ? 1 : 0;
        fsr_need_reset = true;
        bench_bad_frames = 0;
        BenchLog(fmt::format("ev=state run={} scene={} idx={} name={} fsr={} geo={} bm={} anim={} reactive={} motion={} hud={} conv={} sharp={:.2f}",
                             bench_run, bench_scene, idx, b.name, b.backend == 0 ? "off" : b.backend == 1 ? "3.1" : "4", objv_on ? 1 : 0,
                             fsr_objmv ? 1 : 0, fsr_anim ? 1 : 0, fsr_reactive ? 1 : 0, fsr_motion ? 1 : 0, fsr_hudlayer ? 1 : 0, fsr_sign,
                             fsr_sharpen ? fsr_sharpness : 0.0f));
    }
    /// A toggle was changed by a key during the run (not by the bench): the window is not clean.
    bool BenchDrift() const {
        const BenchState& b = kBenchStates[bench_idx];
        return fsr_active != (b.backend != 0) || fsr4_want != (b.backend == 2) || objv_on != (objv_ok && b.geo) || fsr_objmv != b.bm ||
               fsr_anim != Core::GameSettings::NvnFsrAnimSurf() || fsr_reactive != Core::GameSettings::NvnFsrReactive() ||
               fsr_motion != Core::GameSettings::NvnFsrMotionMask() || fsr_hudlayer != Core::GameSettings::NvnFsrHudLayer() ||
               fsr_sign != Core::GameSettings::NvnFsrJitterConv() || fsr_view != 0;
    }
    void BenchRestore() {
        fsr_active = bench_orig.active;
        fsr4_want = bench_orig.want4;
        objv_on = bench_orig.objv;
        fsr_objmv = bench_orig.objmv;
        fsr_anim = bench_orig.anim;
        fsr_reactive = bench_orig.reactive;
        fsr_motion = bench_orig.motion;
        fsr_hudlayer = bench_orig.hud;
        fsr_sign = bench_orig.sign;
        fsr_view = bench_orig.view;
        fsr_sharpen = bench_orig.sharpen;
        fsr_sharpness = bench_orig.sharpness;
        fsr_sharp_idx = bench_orig.sharp_idx;
        fsr_need_reset = true;
    }
    /// Console line (suyu log path: redacted; shown in a terminal the exe was started from, and in user\log). Only called
    /// from bench code, which only runs when nvn_bench is on.
    void BenchConsole(const std::string& line) {
        LOG_INFO(Render_Vulkan, "{}", line);
    }
    void BenchStatusText(bool console_progress = false) {
        const int per = bench_warm + bench_measure;
        int left_states = 0;
        for (int i = bench_idx + 1; i < kBenchStateCount; ++i) left_states += BenchUsable(i) ? 1 : 0;
        const int sec = std::max(0, bench_sec);
        const int total_left = (per - sec) + left_states * per;
        int pos = 0, total = 0;
        for (int i = 0; i < kBenchStateCount; ++i) {
            if (!BenchUsable(i)) continue;
            ++total;
            if (i <= bench_idx) pos = total;
        }
        BenchSetStatus(fmt::format("BENCH {} run {} state {}/{} {} {} {}s | {}s left | keep the scene, do not press keys", bench_scene, bench_run, pos, total,
                                   kBenchStates[bench_idx].name, sec < bench_warm ? "warm-up" : "MEASURING", sec < bench_warm ? bench_warm - sec : per - sec,
                                   total_left));
        if (console_progress && sec >= 0 && ++bench_console_tick >= 15) {
            bench_console_tick = 0;
            BenchConsole(fmt::format("BENCH progress: state {}/{} {} ({}), {}s left, run {}", pos, total, kBenchStates[bench_idx].name,
                                     sec < bench_warm ? "warm-up" : "MEASURING", total_left, bench_run));
        }
    }
    void BenchFinish(bool aborted) {
        bench_active = false;
        bench_apply_pending = false;
        BenchRestore();
        BenchLog(fmt::format("ev={} run={} scene={}", aborted ? "abort" : "done", bench_run, bench_scene));
        BenchSetStatus(aborted ? fmt::format("BENCH ABORTED run {} (settings restored)", bench_run)
                               : fmt::format("BENCH DONE run {} ({}), settings restored, results in nvn_perf.txt", bench_run, bench_scene));
        FsrLog(fmt::format("bench: {} (run {}, scene {}). Settings restored: FSR {}", aborted ? "ABORTED" : "DONE - you can stop; run tools\\perf-summary.ps1",
                           bench_run, bench_scene, fsr_active ? "on" : "off"));
        BenchConsole(aborted ? fmt::format("BENCH ABORTED run {} (settings restored)", bench_run)
                             : fmt::format("BENCH DONE run {} ({}), settings restored, results in nvn_perf.txt", bench_run, bench_scene));
#ifdef _WIN32
        MessageBeep(aborted ? MB_ICONHAND : MB_ICONEXCLAMATION);
#endif
        FsrModeLine();
    }
    /// Called from the S-line builder (end of a second): tags the second just ended and moves the state machine on. The
    /// toggles themselves change at the next present (BenchPoll), never in the middle of a frame.
    std::string BenchSecondTag() {
        const BenchState& b = kBenchStates[bench_idx];
        const char ph = bench_sec < 0 ? 'p' : bench_sec < bench_warm ? 'w' : 'm';
        const bool drift = BenchDrift();
        std::string t = fmt::format(" bench={}:{}:{}:{} bscene={} bad={} drift={}", bench_run, bench_idx, ph, b.name, bench_scene, bench_bad_frames, drift ? 1 : 0);
        bench_bad_frames = 0;
        ++bench_sec;
        if (bench_sec >= bench_warm + bench_measure) {
            const int next = BenchNextUsable(bench_idx + 1);
            if (next < 0) {
                bench_finish_pending = true;
            } else {
                bench_idx = next;
                bench_sec = 0;
                bench_apply_pending = true;
            }
        }
        if (!bench_finish_pending) BenchStatusText(true);
        return t;
    }
    void BenchPoll(u64 frame_no_now) {
        const auto pressed = [](int key, bool& was) {
            const bool down = Common::HostKeyDown(key);
            const bool hit = down && !was;
            was = down;
            return hit;
        };
        if (pressed(VK_ADD, bench_key_down[1]) && !bench_active) {
            static const char* const kScenes[] = {"village_stand", "village_move", "quest"};
            size_t i = 0;
            while (i < 3 && bench_scene != kScenes[i]) ++i;
            bench_scene = kScenes[(i + 1) % 3];
            BenchSetStatus(fmt::format("BENCH scene = {} (press Numpad * or Numpad - to start)", bench_scene));
            FsrLog(fmt::format("bench: scene = {} (Numpad * or Numpad - starts the run)", bench_scene));
            BenchConsole(fmt::format("BENCH scene = {} (press Numpad * or Numpad - to start)", bench_scene));
        }
        // two independent edge flags so either key works even if the other is swallowed or stuck
        const bool start_mul = pressed(VK_MULTIPLY, bench_key_down[0]);
        const bool start_sub = pressed(VK_SUBTRACT, bench_key_down[2]);
        if (start_mul || start_sub) {
            const char* const which = start_mul ? "Numpad *" : "Numpad -";
            BenchConsole(fmt::format("BENCH start key seen ({}), run active {}", which, bench_active ? "yes (this aborts it)" : "no"));
            FsrLog(fmt::format("bench: start key seen ({}) at frame {}", which, frame_no_now));
            if (bench_active) {
                BenchFinish(true);
            } else if (fsr_failed) {
                FsrLog("bench: cannot start, FSR failed to initialise (fsr_failed)");
                BenchConsole("BENCH cannot start: FSR failed to initialise (see nvn_fsr.txt)");
                BenchSetStatus("BENCH cannot start: FSR failed to initialise");
            } else {
                bench_orig = {fsr_active, fsr4_want, objv_on, fsr_objmv, fsr_anim, fsr_reactive, fsr_motion, fsr_hudlayer, fsr_sign, fsr_view,
                              fsr_sharpen, fsr_sharpness, fsr_sharp_idx};
                ++bench_run;
                bench_active = true;
                bench_finish_pending = false;
                bench_idx = BenchNextUsable(0);
                if (bench_idx < 0) {
                    bench_active = false;
                    FsrLog("bench: cannot start, no usable state");
                    BenchConsole("BENCH cannot start: no usable state");
                    BenchSetStatus("BENCH cannot start: no usable state");
                    --bench_run;
                } else {
                bench_sec = -1; // the second in progress is partial: tagged 'p' and ignored
                int usable = 0;
                for (int i = 0; i < kBenchStateCount; ++i) usable += BenchUsable(i) ? 1 : 0;
                BenchLog(fmt::format("ev=run run={} scene={} frame={} warm={} measure={} states={} fsr4_available={} objvel_available={}", bench_run, bench_scene,
                                     frame_no_now, bench_warm, bench_measure, usable, fsr4_ok ? 1 : 0, objv_ok ? 1 : 0));
                BenchApply(bench_idx);
                BenchStatusText();
                bench_console_tick = 0;
                BenchConsole(fmt::format("BENCH run {} started, scene {}, ~{}s, keep the scene and do not press keys", bench_run, bench_scene,
                                         usable * (bench_warm + bench_measure)));
#ifdef _WIN32
                MessageBeep(MB_OK);
#endif
                }
            }
        }
        if (!bench_active) return;
        if (bench_finish_pending) {
            bench_finish_pending = false;
            BenchFinish(false);
            return;
        }
        if (bench_apply_pending) {
            bench_apply_pending = false;
            BenchApply(bench_idx);
#ifdef _WIN32
            MessageBeep(MB_OK);
#endif
        }
        // frames that are not 3D scene frames (menu, loading, still pictures, FSR failure) make the window invalid
        if (fsr_still_kind != 2 || fsr_menu || fsr_failed) ++bench_bad_frames;
    }

    void FsrPresent() {
        if (!bench_init_done) BenchInit();
        if (bench_enabled) BenchPoll(frame_no);
        FsrReactPoll();
        FsrTsPoll();
        ++fsr_ab_frames;
        [[maybe_unused]] bool key_event = false;
        // the Numpad keys are FSR debug hooks: polled only with nvn_fsr_debug / nvn_bench (fsr_dbg)
        if (fsr_dbg) {
        const auto pressed = [](int key, bool& was) {
            const bool down = Common::HostKeyDown(key);
            const bool hit = down && !was;
            was = down;
            return hit;
        };
        const bool numpad1 = pressed(VK_NUMPAD1, fsr_key_down[0]);
        if (numpad1 && !fsr_failed && Common::HostKeyDown(VK_CONTROL)) {
            // Ctrl+Numpad1: FSR 4 <-> FSR 3.1.4 (nvn_fsr_backend = 4 runs only)
            key_event = true;
            if (!fsr4_ok) {
                FsrLog(fmt::format("frame {}: Ctrl+Numpad1 -> FSR 4 not available ({}); start with [Debug] nvn_fsr_backend = 4", frame_no,
                                   fsr4_reason.empty() ? std::string("nvn_fsr_backend is 3") : fsr4_reason));
            } else {
                fsr4_want = !fsr4_want;
                fsr_need_reset = true;
                FsrLog(fmt::format("frame {}: Ctrl+Numpad1 -> backend {} (history reset)", frame_no,
                                   fsr4_want ? "FSR 4" : "FSR 3.1.4"));
            }
        } else if (numpad1 && !fsr_failed) {
            const double now_ab = NvnVkNowMs();
            if (fsr_ab_ms > 0.0 && now_ab > fsr_ab_ms) {
                FsrLog(fmt::format("frame {}: Numpad1 A/B: with FSR {} the game ran {} frames in {:.2f} s = {:.1f} fps", frame_no,
                                   fsr_active ? "ON" : "OFF", fsr_ab_frames, (now_ab - fsr_ab_ms) / 1000.0,
                                   fsr_ab_frames * 1000.0 / (now_ab - fsr_ab_ms)));
            }
            fsr_ab_ms = now_ab;
            fsr_ab_frames = 0;
            fsr_active = !fsr_active;
            fsr_need_reset = true;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad1 -> FSR {}", frame_no, fsr_active ? "ON (history reset)" : "OFF (game picture, no jitter)"));
        }
        // Numpad2/3/5 only change FSR's own output: in view 3 (the game's picture) nothing of it is on screen, and in view 0
        // only where the HUD mask is 0
        const auto vis = [this] {
            return fsr_view == 3 ? " [NOT VISIBLE: view 3 shows the game's picture, not FSR; Numpad4 to view 1 or 0]"
                   : fsr_view == 1 ? " [visible: view 1 = FSR output only]"
                                   : " [visible only outside the HUD mask in this view; view 1 shows FSR alone]";
        };
        if (pressed(VK_NUMPAD2, fsr_key_down[1])) {
            fsr_reactive = !fsr_reactive;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad2 -> reactive mask {} (mask target {:x}; FSR input only: affects ghosting on moving "
                               "objects, no change on a still scene){}",
                               frame_no, fsr_reactive ? "ON" : "OFF", fsr_mask_key, vis()));
        }
        if (pressed(VK_NUMPAD3, fsr_key_down[2])) {
            fsr_sign = (fsr_sign + 1) % 4;
            fsr_need_reset = true;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad3 -> jitter sign convention {} (history reset). Changes ONLY the jitterOffset value given "
                               "to FSR; the camera projection jitter is unchanged. Look for the sharpest, calmest picture on static edges{}",
                               frame_no, FsrSignText(fsr_sign), vis()));
        }
        if (pressed(VK_NUMPAD4, fsr_key_down[3])) {
            fsr_view = (fsr_view + 1) % 4;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad4 -> view {}", frame_no, FsrViewText(fsr_view)));
        }
        if (pressed(VK_NUMPAD5, fsr_key_down[4])) {
            // off -> ini strength -> 0.6 -> 1.0 -> off: FSR's RCAS strength is exp2(-(2 - 2 sharpness)) of its maximum, so
            // the ini default 0.3 is only 38% of it (hard to see on this LDR 1080p input); 1.0 is the full RCAS
            fsr_sharp_idx = (fsr_sharp_idx + 1) % 4;
            fsr_sharpen = fsr_sharp_idx != 0;
            fsr_sharpness = fsr_sharp_idx == 1 ? fsr_sharpness_ini : fsr_sharp_idx == 2 ? 0.6f : 1.0f;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad5 -> sharpening {} (sharpness {:.2f} -> RCAS at {:.0f}% of its strongest, {:.2f} stops below "
                               "it; cycle: off, ini {:.2f}, 0.60, 1.00){}",
                               frame_no, fsr_sharpen ? "ON" : "OFF", fsr_sharpness, 100.0f * std::exp2(-(2.0f - 2.0f * fsr_sharpness)),
                               2.0f - 2.0f * fsr_sharpness, fsr_sharpness_ini, vis()));
        }
        if (pressed(VK_NUMPAD6, fsr_key_down[5]) && !fsr_dump_req) {
            fsr_dump_req = true;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad6 -> dumping the next frame FSR runs in", frame_no));
        }
        if (pressed(VK_NUMPAD7, fsr_key7_down)) {
            fsr_hudlayer = !fsr_hudlayer;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad7 -> HUD {}", frame_no,
                               fsr_hudlayer ? "LAYER (HUD draws go to a premultiplied layer over FSR)" : "old diff composite"));
        }
        if (pressed(VK_NUMPAD8, fsr_key8_down)) {
            fsr_motion = !fsr_motion;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad8 -> motion mask {} (frame-difference ghost guard in the FSR reactive mask){}", frame_no,
                               fsr_motion ? "ON" : "OFF", vis()));
        }
        if (pressed(VK_NUMPAD0, fsr_key0_down)) {
            fsr_anim = !fsr_anim;
            anim_frame = 0;
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad0 -> animated-surface reactive {} (static surfaces whose low-passed colour stepped: "
                               "texture / UV flipbooks such as the banners; needs geometry object vectors){}",
                               frame_no, fsr_anim ? "ON" : "OFF", vis()));
        }
        if (pressed(VK_NUMPAD9, fsr_key9_down)) {
            if (objv_ok) {
                // geometry + block -> geometry only -> block only -> off -> geometry + block
                const u32 state = objv_on ? (fsr_objmv ? 0u : 1u) : (fsr_objmv ? 2u : 3u);
                const u32 next = (state + 1) % 4;
                objv_on = next <= 1;
                fsr_objmv = next == 0 || next == 2;
            } else {
                fsr_objmv = !fsr_objmv;
            }
            key_event = true;
            FsrLog(fmt::format("frame {}: Numpad9 -> object motion: geometry vectors {} (transform feedback of the tracked draws, "
                               "nvn_fsr_objvel {}), block matching {} (characters / cloth jump reactive){}",
                               frame_no, objv_ok ? (objv_on ? "ON" : "OFF") : "unavailable", ObjvModeText(objv_mode), fsr_objmv ? "ON" : "OFF",
                               vis()));
        }
        if (key_event) FsrModeLine();
        } // fsr_dbg
        // the jitter is only wanted while FSR really runs (frames without FSR, e.g. screens whose frame is built
        // differently, would otherwise show the raw jitter): active and FSR ran within the last 30 frames. FSR itself
        // does not need the jitter to start, so the first frames after enabling run unjittered.
        // still frames (no scene pass): no camera jitter once two in a row were seen, so 3D previews drawn in menus with
        // the main camera (save screen, equipment / character previews) are not jittered against a still picture
        const bool running = fsr_active && !fsr_failed && !fsr_menu && fsr_last_frame != 0 && frame_no - fsr_last_frame <= 30 &&
                             (fsr_still_run < 2 || fsr_menu3d_draw != 0);
        if (running != fsr_jitter_on) {
            fsr_jitter_on = running;
            Core::GameSettings::FsrSetActive(running);
            FsrLog(fmt::format("frame {}: camera jitter {}", frame_no, running ? "ON" : "OFF"));
        }
        // where the snapshot went in the frame just drawn: keep its draw list, log it when the pattern changes (at most
        // every 2 s) and every 30 s
        const double now_list = NvnVkNowMs();
        // without the debug hooks the per-frame strings below are not built: an integer test of the same shape
        // (snapshot reason, snapshot / last post pass relative to H's first draw, post passes) decides whether to log
        bool snap_line = fsr_frame == frame_no && fsr_snap_ok;
        if (snap_line && !fsr_dbg) {
            const auto relv = [](u32 v, u32 base) { return v == ~0u || base == ~0u ? std::numeric_limits<s64>::min() : s64{v} - s64{base}; };
            const std::array<s64, 4> shape{s64{fsr_snap_why}, relv(fsr_snap_draw, fsr_snap_hfirst), relv(fsr_snap_post, fsr_snap_hfirst),
                                           s64{fsr_snap_posts}};
            snap_line = shape != fsr_snap_shape && now_list - fsr_list_ms >= 2000.0;
            if (snap_line) fsr_snap_shape = shape;
        }
        if (snap_line) {
            const auto rel = [](u32 v, u32 base) { return v == ~0u || base == ~0u ? std::string("-") : fmt::format("{:+d}", s64{v} - s64{base}); };
            const std::string sig = fmt::format("{} | snapshot after draw {} (H first draw +0 = {}, snapshot {} rel. to it, last post pass {}, "
                                                "post passes {})",
                                                FsrSnapWhyText(fsr_snap_why), fsr_snap_draw == 0 ? 0 : fsr_snap_draw - 1, fsr_snap_hfirst,
                                                rel(fsr_snap_draw, fsr_snap_hfirst), rel(fsr_snap_post, fsr_snap_hfirst), fsr_snap_posts);
            const std::string shape = sig.substr(0, sig.find(" | ")) + fmt::format("{} {} {}", rel(fsr_snap_draw, fsr_snap_hfirst),
                                                                                  rel(fsr_snap_post, fsr_snap_hfirst), fsr_snap_posts);
            const bool changed = shape != fsr_snap_sig;
            if ((changed && now_list - fsr_list_ms >= 2000.0) || now_list - fsr_list_ms >= 30000.0 || (fsr_dump_req && fsr_last_frame == frame_no)) {
                fsr_snap_sig = shape;
                fsr_list_ms = now_list;
                FsrLog(fmt::format("frame {}: snapshot point{}: {}", frame_no, changed ? " (changed)" : "", sig));
                if (fsr_dbg) { // the draw list is recorded only with the debug hooks (FsrRecord)
                    fsr_list = fsr_recs;
                    fsr_list_n = fsr_recs_n;
                    fsr_list_frame = frame_no;
                    FsrLog(FsrDrawListText());
                }
            }
        }
        if (fsr_dump_req && fsr_last_frame == frame_no) {
            fsr_dump_req = false;
            try {
                FsrWriteDump();
            } catch (const std::exception& e) {
                FsrLog(fmt::format("dump failed: {}", e.what()));
            }
        }
        ++fsr_presents;
        const double now = NvnVkNowMs();
        if (fsr_line_ms == 0.0) fsr_line_ms = now;
        if (now - fsr_line_ms < 1000.0) return;
        const double secs = (now - fsr_line_ms) / 1000.0;
        fsr_line_ms = now;
        // ini re-reads (file opens) once a second: debug hooks only; otherwise the FsrInit values stay
        if (const int sd = fsr_dbg ? Core::GameSettings::NvnFsrSnapDraw() : fsr_snapdraw; sd != fsr_snapdraw) {
            FsrLog(fmt::format("frame {}: ini nvn_fsr_snapdraw {} -> {}: snapshot {}", frame_no, fsr_snapdraw, sd,
                               sd > 0 ? fmt::format("forced right after draw {}", sd) : std::string("automatic")));
            fsr_snapdraw = sd;
            fsr_need_reset = true;
        }
        if ((fsr_menu || !fsr_scene_now) && !fsr_menu_targets.empty()) { // menu-skip or still-picture frames
            FsrLogSec(fsr_menu_targets);
            fsr_menu_targets.clear();
        }
        if (fsr_dbg) {
            fsr_hud_lo = Core::GameSettings::NvnFsrHudLo();
            fsr_hud_hi = Core::GameSettings::NvnFsrHudHi();
        }
        {
            std::string combos;
            for (const auto& [c, n] : fsr_hud_combos) combos += fmt::format("{}[{} x{}]", combos.empty() ? "" : " ", c, n);
            FsrLogSec(fmt::format("frame {} HUD {}: frames layer {} diff {} menu-skip {} no-FSR {} (FSR on still pictures {}) | redirected draws {} ({:.1f}/layer frame) | "
                               "aborts {} merges {} clears of H into the layer {} HUD draws sampling another render target {} | "
                               "opaque draws with the alpha logic-op pass (no alphaToOne) {}, multiply draws {} | variants {}: {}",
                               frame_no, fsr_hudlayer ? "layer on" : "layer off (diff)", fsr_sec_layer, fsr_sec_diff, fsr_sec_skip, fsr_sec_none, fsr_sec_still,
                               fsr_sec_redirected, fsr_sec_layer ? double(fsr_sec_redirected) / fsr_sec_layer : 0.0, fsr_sec_aborts, fsr_sec_merges,
                               fsr_sec_clears, fsr_sec_rt_reads, fsr_sec_alpha_pass, fsr_sec_multiply, fsr_hud_variants,
                               combos.empty() ? "-" : combos));
            fsr_sec_layer = fsr_sec_diff = fsr_sec_skip = fsr_sec_none = fsr_sec_redirected = fsr_sec_still = 0;
            fsr_sec_aborts = fsr_sec_merges = fsr_sec_clears = fsr_sec_rt_reads = fsr_sec_alpha_pass = fsr_sec_multiply = 0;
        }
        const FsrLast& L = fsr_last;
        // which entry of AMD's sequence the jitter of the last dispatch was (end-to-end check of game_settings' Halton)
        int best = -1;
        float best_err = 1e9f;
        for (size_t i = 0; i < fsr_amd_jitter.size(); ++i) {
            const float e = std::hypot(fsr_amd_jitter[i][0] - L.ox, fsr_amd_jitter[i][1] - L.oy);
            if (e < best_err) best_err = e, best = static_cast<int>(i);
        }
        u32 ph_idx = 0, ph_cnt = 0;
        Core::GameSettings::FsrJitterPhase(&ph_idx, &ph_cnt);
        FsrLogSec(fmt::format(
            "frame {} {:.1f} fps {:.2f} ms | FSR {}{} dispatched {} (resets {}) | last: frame {} {}x{} -> {}x{} jitter offset ({:+.4f}, {:+.4f}) "
            "render px (AMD index {} err {:.5f}; game_settings phase {}/{}) -> FSR jitterOffset ({:+.4f}, {:+.4f}) sign {} reset {}{} "
            "near {:.3g} far {:.3g} fov {:.4f} dt {:.2f} ms | CPU record {:.3f} ms avg {:.3f} max | skipped: no snapshot {} no source {} no mv {} "
            "size {}{} | snapshot after draw {} ({}{}) after {} draws into {:x}; mask {:x} (draw {}) reactive {} | view {} sharpen {} | errors {}{}",
            frame_no, fsr_presents / secs, secs * 1000.0 / std::max<u64>(1, fsr_presents), fsr_active ? "on" : "off", fsr_failed ? " (FAILED)" : "",
            fsr_dispatched, fsr_resets, L.frame, L.rw, L.rh, L.ow, L.oh, L.ox, L.oy, best, best < 0 ? 0.0f : best_err, ph_idx, ph_cnt, L.jx, L.jy,
            fsr_sign, L.reset ? "yes" : "no", L.reset ? fmt::format(" (paired {} cam-cut {} gap {})", L.paired, L.cam_reset, L.gap) : "",
            L.cam_near, L.cam_far, L.fov, L.dt, fsr_dispatched ? fsr_rec_sum / fsr_dispatched : 0.0, fsr_rec_max, fsr_skip_nosnap,
            fsr_skip_nosrc, fsr_skip_nomv, fsr_skip_size, fsr_skip_size ? " (" + fsr_skip_size_text + ")" : "",
            fsr_snap_draw == 0 ? 0 : fsr_snap_draw - 1,
            fsr_snap_why == 1 || fsr_snap_why == 2 ? "auto" : fsr_snap_why == 3 ? "fallback" : fsr_snap_why >= 4 ? "forced" : "none",
            fsr_snap_why == 1 || fsr_snap_why == 2 ? fmt::format(", {} post passes", fsr_snap_posts) : std::string(), fsr_snap_src_draws, fsr_src_key, fsr_mask_key, fsr_mask_draw, fsr_reactive ? "on" : "off", fsr_view,
            fsr_sharpen ? fmt::format("on (sharpness {:.2f}, RCAS pass runs)", fsr_sharpness) : std::string("off"), fsr_errors,
            fsr_last_error.empty() ? "" : " last: " + fsr_last_error));
        {
            const double n = std::max<u32>(1u, fsr_ts_n);
            const double gpu_total = (fsr_ts_sum[0] + fsr_ts_sum[1] + fsr_ts_sum[2] + fsr_ts_sum[3] + fsr_ts_sum[4] + fsr_ts_sum[5]) / n;
            FsrLogSec(fmt::format(
                "frame {} FSR cost: {:.1f} fps | GPU per frame (avg of {} frames): block matching {:.3f} ms, object velocity pass + merge "
                "{:.3f}, animated surfaces {:.3f}, prep {:.3f}, {} {:.3f}, composite {:.3f} = {:.3f} ms (transform feedback "
                "capture runs inside the game's draws: not separable; extra capture-only draws {:.1f}/frame) | render-thread CPU per "
                "second: draw classification + capture recording {:.1f} ms, pose-signal hashing {:.1f} ms (nvn_fsr_signals {}), velocity "
                "pass + merge recording {:.1f} ms, FSR dispatch recording {:.1f} ms, capture pipeline builds {:.1f} ms | static draws "
                "excluded from capture {} (newly static {}, seen moving {})",
                frame_no, fsr_presents / secs, fsr_ts_n, fsr_ts_sum[0] / n, fsr_ts_sum[1] / n, fsr_ts_sum[2] / n, fsr_ts_sum[3] / n,
                fsr4_sec_frames == 0 ? "FSR 3.1"
                : fsr4_sec_fb == 0   ? "FSR 4 (incl. input copies, D3D12 handoff, output copy)"
                                     : "FSR (mixed FSR 4 / FSR 3.1 frames)",
                fsr_ts_sum[4] / n, fsr_ts_sum[5] / n, gpu_total, objv_sec_frames ? double(objv_sec_cap_expanded) / objv_sec_frames : 0.0,
                objv_sec_cpu_ms - objv_sec_sig_ms, objv_sec_sig_ms, objv_signals ? "on" : "off", objv_sec_run_ms, fsr_rec_sum,
                objv_sec_pipe_ms, objv_sec_sk_static, objv_sec_newly_static, objv_sec_seen_moving));
            for (double& v : fsr_ts_sum) v = 0.0;
            fsr_ts_n = 0;
#ifdef SUYU_HAS_FSR4_BRIDGE
            if (fsr_backend_ini == 4) {
                double s4 = 0.0, m4 = 0.0;
                u32 n4 = 0;
                if (fsr4) fsr4->TakeGpuStats(s4, n4, m4);
                fsr4_sec_d3d12_ms = n4 ? s4 / n4 : 0.0;
                FsrLogSec(fmt::format("frame {} FSR backend: {} | FSR frames this second: FSR 4 {}, FSR 3.1 while FSR 4 was wanted {} | FSR 4 "
                                   "dispatch GPU (D3D12 timestamps around ffxDispatch) avg {:.3f} ms max {:.3f} ms over {} frames | failures "
                                   "{} of 3{}",
                                   frame_no, FsrBackendText(), fsr4_sec_frames, fsr4_sec_fb, n4 ? s4 / n4 : 0.0, m4, n4, fsr4_failures,
                                   fsr4_reason.empty() ? std::string() : " | " + fsr4_reason));
            }
#endif
            fsr4_sec_frames = fsr4_sec_fb = 0;
            objv_sec_sig_ms = objv_sec_run_ms = 0.0;
            objv_sec_sk_static = objv_sec_newly_static = objv_sec_seen_moving = 0;
        }
        if (objv_mode > 0) {
            FsrLogSec(fmt::format(
                "frame {} object velocity: {} | FSR frames {} (no capture {}, no history {}), velocity passes {} | captured: mask-target "
                "draws {}, skinned scene draws {} (skipped as already captured in the mask pass {}), capture bytes max {:.1f} MB/frame | "
                "paired {} (repeated keys paired by order {}, by key without vertex buffers {}), unpaired (new) {}, ambiguous {}, "
                "size/class/depth mismatch {} | captured as expanded restart strips {} | skipped (reasons): not a scene-size "
                "colour+depth draw {}, mask target not a separate pass {}, excluded by the mode {}, blended {}, no depth test {}, "
                "vertex shader not skinned {}, topology {}, non-indexed restart strip {}, index data unreadable {}, arena full {}, "
                "too big {}, capture buffers full {}, no capture pipeline {} | "
                "capture pipelines {} ({:.1f} ms this second, {:.0f} ms total) | velocity vertices {} | scene draws tracked because "
                "their vertex data changes (CPU-animated) {}, of them low-rate update (jump) frames {} | mask target {:x} (separate "
                "pass {}) | cost: captured vertices max {}/frame (cap {} hit {} draws), velocity vertices {:.0f}/pass, CPU {:.2f} ms/s "
                "(draw classification, hashing, recording), GPU: see the FSR cost line{}{} | pose updates "
                "(low-rate jumps) {}",
                frame_no, !objv_ok ? "UNAVAILABLE" : objv_on ? "on" : "off", objv_sec_frames, objv_sec_nocap, objv_sec_nohist, objv_sec_passes,
                objv_sec_cap_mask, objv_sec_cap_scene, objv_sec_sk_dedup, objv_sec_bytes_max / 1048576.0, objv_sec_matched, objv_sec_m_order,
                objv_sec_m_loose, objv_sec_unmatched, objv_sec_ambiguous, objv_sec_mismatch, objv_sec_cap_expanded, objv_sec_sk_target,
                objv_sec_sk_maskoff, objv_sec_sk_mode, objv_sec_sk_blend, objv_sec_sk_nodepth, objv_sec_sk_noskin, objv_sec_sk_topo,
                objv_sec_sk_restart, objv_sec_sk_unread, objv_sec_sk_arena, objv_sec_sk_big, objv_sec_sk_full, objv_sec_sk_nopipe,
                objv_pipes_built, objv_sec_pipe_ms, objv_pipes_ms, objv_sec_verts, objv_sec_cap_vdyn, objv_sec_jumps, fsr_mask_key,
                objv_mask_ok ? "yes" : "no", objv_sec_verts_max, objv_max_verts, objv_sec_sk_vcap,
                objv_sec_passes ? double(objv_sec_verts) / objv_sec_passes : 0.0, objv_sec_cpu_ms,
                "", "", objv_sec_updates));
            objv_sec_verts_max = objv_sec_sk_vcap = objv_sec_updates = 0;
            objv_sec_gpu_n = 0;
            objv_sec_cpu_ms = objv_sec_gpu_ms = 0.0;
            if (objv_signals && ++objv_hist_secs >= 5) { // the skip histogram: every 5 s (counts are per 5 s)
                objv_hist_secs = 0;
                if (!objv_hist.empty()) FsrLogSec(fmt::format("frame {} object velocity (last 5 s) {}", frame_no, ObjvSkipHistText()));
                objv_hist.clear();
                if (!objv_sigstat.empty()) FsrLogSec(fmt::format("frame {} object velocity (last 5 s) {}", frame_no, ObjvSigText()));
                objv_sigstat.clear();
            }
            objv_sec_cap_vdyn = objv_sec_jumps = 0;
            objv_sec_frames = objv_sec_nocap = objv_sec_nohist = objv_sec_passes = objv_sec_cap_mask = objv_sec_cap_scene = 0;
            objv_sec_sk_dedup = objv_sec_bytes_max = objv_sec_matched = objv_sec_m_order = objv_sec_m_loose = objv_sec_unmatched = 0;
            objv_sec_ambiguous = objv_sec_mismatch = objv_sec_sk_topo = objv_sec_sk_restart = objv_sec_sk_big = objv_sec_sk_full = 0;
            objv_sec_sk_nopipe = objv_sec_verts = 0;
            objv_sec_cap_expanded = objv_sec_sk_unread = objv_sec_sk_arena = objv_sec_sk_target = objv_sec_sk_mode = 0;
            objv_sec_sk_maskoff = objv_sec_sk_blend = objv_sec_sk_nodepth = objv_sec_sk_noskin = 0;
            objv_sec_pipe_ms = 0.0;
        }
        FsrModeLine(true);
        fsr_presents = fsr_dispatched = fsr_resets = 0;
        fsr_skip_nosnap = fsr_skip_nosrc = fsr_skip_nomv = fsr_skip_size = 0;
        fsr_rec_sum = fsr_rec_max = 0.0;
    }

    // Numpad6: this frame's FSR input, FSR output, composite and HUD image as PNG (alpha forced opaque).
    void FsrWriteDump() {
        std::error_code ec;
        const auto dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "upscaler_dump";
        std::filesystem::create_directories(dir, ec);
        const auto save = [&](Image* img, const char* what) -> std::vector<u8> {
            if (!img) return {};
            std::vector<u8> px = MvReadback(*img, VK_IMAGE_ASPECT_COLOR_BIT, 4);
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            const u32 w = img->pw ? img->pw : img->width, h = img->ph ? img->ph : img->height;
            const std::string name = fmt::format("fsr_{}_{}_{}x{}.png", frame_no, what, w, h);
            WritePng(dir / name, w, h, px.data());
            return px;
        };
        const std::vector<u8> in_px = save(fsr_snap.get(), "input");
        save(fsr_out.get(), "fsr");
        save(fsr_comp.get(), "composite");
        const std::vector<u8> hud_px = save(fsr_final_src, "hud");
        // the other FSR inputs of this frame as raw texels (row-major, no header), for the fsr4-probe (--depth file etc.):
        // depth R32F (NDC z, 1 = far), the motion vectors FSR read (render px, cur + mv = prev, jitter removed), reactive R8
        std::string raw_text;
        const auto save_raw = [&](Image* img, u32 bpp, const char* what, const char* ext) {
            if (!img) {
                raw_text += fmt::format("{}: none; ", what);
                return;
            }
            const std::vector<u8> px = MvReadback(*img, VK_IMAGE_ASPECT_COLOR_BIT, bpp);
            const u32 w = img->pw ? img->pw : img->width, h = img->ph ? img->ph : img->height;
            const std::string name = fmt::format("fsr_{}_{}_{}x{}.{}", frame_no, what, w, h, ext);
            std::ofstream(dir / name, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(px.data()),
                                                                               static_cast<std::streamsize>(px.size()));
            raw_text += fmt::format("{} ({} bytes); ", name, px.size());
        };
        save_raw(fsr_depth.get(), 4, "depth", "r32f");
        if (fsr_last_mv_img && (fsr_last_mv_img->format == VK_FORMAT_R32G32_SFLOAT || fsr_last_mv_img->format == VK_FORMAT_R16G16_SFLOAT)) {
            const bool f32 = fsr_last_mv_img->format == VK_FORMAT_R32G32_SFLOAT;
            save_raw(fsr_last_mv_img, f32 ? 8 : 4, "mv", f32 ? "rg32f" : "rg16f");
        } else {
            save_raw(nullptr, 0, "mv", "");
        }
        save_raw(fsr_last_react_on ? fsr_react.get() : nullptr, 1, "reactive", "r8");
        FsrLog(fmt::format("frame {}: raw FSR inputs: {}", frame_no, raw_text));
        // HUD mask coverage, as the composite shader computes it (render texels): raw = smoothstep(1/255, 6/255, max |hud - snap|)
        // > 0 when the largest channel difference is >= 2/255; >= 0.5 from 4/255; then the 3x3 max (dilation)
        std::string coverage = "HUD mask coverage: not measured (input/hud missing or sizes differ)";
        if (fsr_snap && fsr_final_src && !in_px.empty() && in_px.size() == hud_px.size()) {
            const u32 w = fsr_snap->pw ? fsr_snap->pw : fsr_snap->width, h = fsr_snap->ph ? fsr_snap->ph : fsr_snap->height;
            if (size_t{w} * h * 4 == in_px.size()) {
                std::vector<u8> raw(size_t{w} * h);
                u64 n_any = 0, n_half = 0, n_dil = 0, n_20 = 0, n_strong = 0, n_strong_dil = 0;
                const int lo255 = static_cast<int>(std::ceil(fsr_hud_lo * 255.0f));
                std::vector<u8> strong(raw.size());
                for (size_t i = 0; i < raw.size(); ++i) {
                    int d = 0;
                    for (u32 c = 0; c < 3; ++c) d = std::max(d, std::abs(int{in_px[i * 4 + c]} - int{hud_px[i * 4 + c]}));
                    raw[i] = d >= 2 ? 1 : 0;
                    n_any += d >= 2;
                    n_half += d >= 4;
                    n_20 += d >= 20;
                    strong[i] = d >= lo255 ? 1 : 0;
                    n_strong += d >= lo255;
                }
                for (u32 y = 0; y < h; ++y) {
                    for (u32 x = 0; x < w; ++x) {
                        bool m = false, ms = false;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int xx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1);
                                const int yy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1);
                                m = m || raw[size_t(yy) * w + xx] != 0;
                                ms = ms || strong[size_t(yy) * w + xx] != 0;
                            }
                        }
                        n_dil += m;
                        n_strong_dil += ms;
                    }
                }
                const double t = static_cast<double>(raw.size());
                coverage = fmt::format("HUD mask coverage (render texels {}x{}, snapshot vs final H): mask > 0 {:.2f}%, mask >= 0.5 {:.2f}%, "
                                       "after the 3x3 dilation {:.2f}%; differ by >= 20/255 {:.2f}%; STRONG mask (d >= {}/255, the area shown as HUD) {:.2f}%, after dilation {:.2f}% (a HUD-only difference is typically "
                                       "a few %; tens of % = the snapshot is not the finished scene)",
                                       w, h, 100.0 * n_any / t, 100.0 * n_half / t, 100.0 * n_dil / t, 100.0 * n_20 / t,
                                       lo255, 100.0 * n_strong / t, 100.0 * n_strong_dil / t);
            }
        }
        FsrLog(fmt::format("frame {}: {}", frame_no, coverage));
        // HUD layer: the layer image of this frame (straight-alpha PNG: colour un-premultiplied) and its coverage
        std::string layer_text = fmt::format("HUD layer: not used in this frame (path: {})", FsrPathText(fsr_frame_path));
        if (fsr_layer && fsr_frame_path == 1) {
            std::vector<u8> px = MvReadback(*fsr_layer, VK_IMAGE_ASPECT_COLOR_BIT, 4);
            const u32 w = fsr_layer->width, h = fsr_layer->height;
            u64 n_any = 0, n_half = 0, n_full = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                const u32 a = px[i + 3];
                n_any += a > 0;
                n_half += a >= 128;
                n_full += a == 255;
                for (u32 c = 0; c < 3; ++c) px[i + c] = a == 0 ? 0 : static_cast<u8>(std::min<u32>(255, (px[i + c] * 255u + a / 2) / a));
            }
            WritePng(dir / fmt::format("fsr_{}_hudlayer_{}x{}.png", frame_no, w, h), w, h, px.data());
            const double t = std::max<double>(1.0, static_cast<double>(px.size() / 4));
            layer_text = fmt::format("HUD layer coverage ({}x{}, {} draws redirected, from draw {}): alpha > 0 {:.2f}%, alpha >= 0.5 {:.2f}%, "
                                     "alpha = 1 {:.2f}% (fsr_{}_hudlayer_{}x{}.png: STRAIGHT alpha, colour un-premultiplied from the layer's "
                                     "premultiplied RGB; in this path the 'hud' PNG is H WITHOUT the HUD)",
                                     w, h, fsr_frame_path_draws, fsr_layer_begin_draw, 100.0 * n_any / t, 100.0 * n_half / t, 100.0 * n_full / t,
                                     frame_no, w, h);
        }
        FsrLog(fmt::format("frame {}: {}", frame_no, layer_text));
        // animated-surface tiles of this frame over the FSR input: magenta = flagged now, dark blue = flagged last frame
        if (anim_tile[anim_wr] && anim_frame == frame_no && fsr_snap && !in_px.empty()) {
            Image& ti = *anim_tile[anim_wr];
            const std::vector<u8> tiles = MvReadback(ti, VK_IMAGE_ASPECT_COLOR_BIT, 4);
            const u32 w = fsr_snap->pw ? fsr_snap->pw : fsr_snap->width, h = fsr_snap->ph ? fsr_snap->ph : fsr_snap->height;
            if (size_t{w} * h * 4 == in_px.size() && tiles.size() >= size_t{ti.width} * ti.height * 4) {
                std::vector<u8> dbg(in_px);
                u64 now_n = 0, last_n = 0;
                for (u32 ty = 0; ty < ti.height; ++ty) {
                    for (u32 tx = 0; tx < ti.width; ++tx) {
                        const u8* t = tiles.data() + (size_t{ty} * ti.width + tx) * 4;
                        now_n += t[0] > 50;
                        last_n += t[3] < 192; // rejected by the gate
                    }
                }
                for (u32 y = 0; y < h; ++y) {
                    for (u32 x = 0; x < w; ++x) {
                        u8* d = dbg.data() + (size_t{y} * w + x) * 4;
                        d[3] = 255;
                        const u32 tx = std::min(x / 8, ti.width - 1), ty = std::min(y / 8, ti.height - 1);
                        const u8* t = tiles.data() + (size_t{ty} * ti.width + tx) * 4;
                        if (t[0] > 200) { // passed, full weight: magenta
                            d[0] = static_cast<u8>((d[0] + 255) / 2), d[1] = static_cast<u8>(d[1] / 2), d[2] = static_cast<u8>((d[2] + 255) / 2);
                        } else if (t[0] > 50) { // passed, first flag (weight 0.35): pink
                            d[0] = static_cast<u8>((d[0] + 255) / 2), d[1] = static_cast<u8>((d[1] + 128) / 2), d[2] = static_cast<u8>((d[2] + 192) / 2);
                        } else if (t[3] < 192) { // raw flag rejected by the edge / depth gate: cyan
                            d[0] = static_cast<u8>(d[0] / 2), d[1] = static_cast<u8>((d[1] + 255) / 2), d[2] = static_cast<u8>((d[2] + 255) / 2);
                        } else if (t[2] > 50) {
                            d[0] = static_cast<u8>(d[0] / 2), d[1] = static_cast<u8>(d[1] / 2), d[2] = static_cast<u8>((d[2] + 160) / 2);
                        }
                    }
                }
                WritePng(dir / fmt::format("fsr_{}_animsurf_{}x{}.png", frame_no, w, h), w, h, dbg.data());
                FsrLog(fmt::format("frame {}: fsr_{}_animsurf png: passed tiles {} ({:.2f}%, magenta full weight, pink first flag), rejected "
                                   "by the edge gate {} (cyan), dark blue = passed last frame", frame_no,
                                   frame_no, now_n, 100.0 * now_n / std::max<double>(1.0, double(ti.width) * ti.height), last_n));
            }
        } else {
            FsrLog(fmt::format("frame {}: no animsurf png ({}; needs geometry object vectors and history this frame)", frame_no,
                               fsr_anim ? "animated surfaces on, but they did not run in this frame" : "animated surfaces off"));
        }
        // geometry object vectors of this frame: PNG (grey 128 = 0 px, 16 levels per px: red = x, green = y; blue 255 =
        // valid; black = no object vector there)
        if (!(objv_vel && objv_vel_frame == frame_no)) {
            FsrLog(fmt::format("frame {}: no object velocity png: the velocity pass did not run in this frame (geometry vectors {}, "
                               "last velocity pass in frame {})",
                               frame_no, !objv_ok ? "unavailable" : objv_on ? "on" : "off", objv_vel_frame));
        } else {
            const std::vector<u8> raw = MvReadback(*objv_vel, VK_IMAGE_ASPECT_COLOR_BIT, 8);
            const u32 w = objv_vel->width, h = objv_vel->height;
            const auto half = [](u16 v) {
                const u32 e = (v >> 10) & 31u, m = v & 1023u;
                const float f = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                                : e == 31 ? std::numeric_limits<float>::infinity()
                                          : std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
                return (v & 0x8000u) ? -f : f;
            };
            std::vector<u8> png(size_t{w} * h * 4, 0);
            u64 valid = 0;
            double mag = 0.0;
            for (size_t i = 0; i < size_t{w} * h && i * 8 + 7 < raw.size(); ++i) {
                u16 q[4];
                std::memcpy(q, raw.data() + i * 8, 8);
                png[i * 4 + 3] = 255;
                if (half(q[2]) <= 0.5f) continue;
                const float mx = half(q[0]), my = half(q[1]);
                ++valid;
                mag += std::hypot(mx, my);
                png[i * 4 + 0] = static_cast<u8>(std::clamp(128.0f + mx * 16.0f, 0.0f, 255.0f));
                png[i * 4 + 1] = static_cast<u8>(std::clamp(128.0f + my * 16.0f, 0.0f, 255.0f));
                png[i * 4 + 2] = 255;
            }
            WritePng(dir / fmt::format("fsr_{}_objvel_{}x{}.png", frame_no, w, h), w, h, png.data());
            // debug variant over the FSR input: red = object vector differs from the camera vector by > 0.25 px (real object
            // motion), blue = low-rate pose update (jump) texels, green = object vector equal to the camera's, else the picture
            if (mv_img && mv_img->width == w && mv_img->height == h && in_px.size() == size_t{w} * h * 4) {
                const std::vector<u8> cam = MvReadback(*mv_img, VK_IMAGE_ASPECT_COLOR_BIT, 8);
                std::vector<u8> dbg(in_px);
                u64 moving = 0, jumpn = 0;
                for (size_t i = 0; i < size_t{w} * h && i * 8 + 7 < raw.size() && i * 8 + 7 < cam.size(); ++i) {
                    dbg[i * 4 + 3] = 255;
                    u16 q[4];
                    std::memcpy(q, raw.data() + i * 8, 8);
                    if (half(q[2]) <= 0.5f) continue;
                    float c2[2];
                    std::memcpy(c2, cam.data() + i * 8, 8);
                    const bool mov = std::hypot(half(q[0]) - c2[0], half(q[1]) - c2[1]) > 0.25f;
                    const bool jmp = half(q[3]) > 0.5f;
                    moving += mov;
                    jumpn += jmp;
                    const u8 tint[3] = {static_cast<u8>(mov ? 255 : 0), static_cast<u8>(!mov && !jmp ? 160 : 0), static_cast<u8>(jmp ? 255 : 0)};
                    for (u32 k = 0; k < 3; ++k) dbg[i * 4 + k] = static_cast<u8>((dbg[i * 4 + k] + tint[k]) / 2);
                }
                WritePng(dir / fmt::format("fsr_{}_objvel_moving_{}x{}.png", frame_no, w, h), w, h, dbg.data());
                FsrLog(fmt::format("frame {}: fsr_{}_objvel_moving png: object vector differs from the camera vector by > 0.25 px on {:.2f}% "
                                   "of the picture (red), pose-update texels {:.2f}% (blue), green = object vector equal to the camera's",
                                   frame_no, frame_no, 100.0 * moving / (double(w) * h), 100.0 * jumpn / (double(w) * h)));
            }
            FsrLog(fmt::format("frame {}: object velocity dump fsr_{}_objvel_{}x{}.png: valid texels {:.2f}% (mean |mv| {:.3f} px), {} draws "
                               "paired (grey 128 = 0 px, 16 levels per px, red = x, green = y, blue = valid; black = camera vector)",
                               frame_no, frame_no, w, h, 100.0 * valid / std::max<double>(1.0, double(w) * h), valid ? mag / valid : 0.0,
                               objv_stat_pairs));
        }
        coverage += "\n" + layer_text;
        const FsrLast& L = fsr_last;
        std::ofstream(dir / fmt::format("fsr_{}.txt", frame_no), std::ios::trunc) << fmt::format(
            "nvn_fsr dump of frame {}: input = HUD image copied before its first HUD draw ({}x{}), fsr = upscaler output ({}x{}; backend {}), "
            "composite = what the final draw sampled (view {}), hud = the game's image with HUD (render size; the game scales it bilinearly).\n"
            "raw inputs: {}\n"
            "jitter offset ({:+.5f}, {:+.5f}) render px, FSR jitterOffset ({:+.5f}, {:+.5f}), sign convention {}\nreset {} near {} far {} fov {} "
            "dt {:.3f} ms sharpen {} sharpness {:.2f} reactive {}\nsnapshot after draw {}: {} (nvn_fsr_snapdraw {})\n{}\n{}\n",
            frame_no, L.rw, L.rh, L.ow, L.oh, FsrBackendText(), FsrViewText(fsr_view), raw_text, L.ox, L.oy, L.jx, L.jy, FsrSignText(fsr_sign), L.reset, L.cam_near, L.cam_far,
            L.fov, L.dt, fsr_sharpen, fsr_sharpness, fsr_reactive, fsr_snap_draw == 0 ? 0 : fsr_snap_draw - 1, FsrSnapWhyText(fsr_snap_why),
            fsr_snapdraw, coverage, fsr_list_frame == frame_no ? FsrDrawListText() : std::string("(draw list not captured for this frame)"));
        FsrLog(fmt::format("dump written: upscaler_dump/fsr_{}_*.png", frame_no));
    }
#endif // SUYU_HAS_FSR_VULKAN

    // ---- members ---------------------------------------------------------------------------------------------
    std::set<UpKey> up_seen;                 // upscaler probe state, untouched unless the probe is on
    std::map<UpKey, u32> up_frame_counts;
    std::vector<u32> up_rt0;
    std::vector<std::string> up_lines;
    u32 up_draws{};
    s32 up_last_depth{-1};
    struct UpImg {
        u32 w{}, h{}, fmt{};
        bool depth{};
        u32 draws{}, first{}, last{};
        u32 sampled{}, first_sampled{}; // later draws that bound this image as a sampled texture (after it was first drawn into)
    };
    std::map<u32, UpImg> up_imgs; // per frame, keyed by RtKey
    u32 up_dumped{};              // image dump files written so far (cap 40, fixed frames)
    u32 up_total_dumped{};        // all probe image files, fixed + F7 (cap 400)
    u32 up_f7_dumped{};           // image files of the current F7 trigger (cap 40)
    u64 up_f7_frame{};            // frame (probe numbering) being captured after an F7 hit; 0 = none
    bool up_f7_down{};            // F7 edge detection
    bool up_f6_down{};            // F6 (burst) edge detection
    u32 up_burst_id{};            // F6 bursts started this run (0 = none yet)
    u64 up_burst_first{};         // first frame (probe numbering) of the current burst of 3
    u32 up_ub_color{}, up_ub_depth{}; // per frame: uniform dumps done for 1920x1080 color / depth draws
    const Device& device;
    MemoryAllocator& allocator;
    const vk::Device& dev;
    vk::Queue queue;
    vk::CommandPool pool;
    vk::CommandBuffers cmd_buffers;
    std::array<Frame, kSlots> frames;
    VkCommandBuffer util_cmd{};
    vk::Fence util_fence;
    vk::Buffer zero_buffer;
    vk::Sampler default_sampler;
    struct QMeta {
        u64 p0, p1;
        u32 count, w, h, rt;
    };
    struct LinearSeen {
        u64 serial{~0ull}, hash{}, key{};
    };
    std::unordered_map<u32, LinearSeen> linear_seen;
    u32 spv_dumped{};
    vk::QueryPool qpool;
    vk::Buffer probe_buf;
    vk::Buffer pick_buf;
    std::array<std::string, 512> pick_meta;
    u32 pn{};
    bool pick_on{};
    std::array<bool, 256> qprobe{};
    std::array<QMeta, 256> qmeta{};
    u32 qn{};
    bool query_on{};
    std::unique_ptr<Image> placeholder;
    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::mutex lock;
    std::mutex note_lock;
    std::vector<std::string> log_lines;
    Stats stats;
    // [Debug] probes "draw vp" counter: viewport_set, viewport bits x4, scissor_set, scissor x4, pass w, h -> draws since the last WriteStats
    struct VpProbeHash {
        size_t operator()(const std::array<u32, 12>& k) const {
            u64 h = 1469598103934665603ull;
            for (const u32 v : k) h = (h ^ v) * 1099511628211ull;
            return static_cast<size_t>(h);
        }
    };
    std::unordered_map<std::array<u32, 12>, u64, VpProbeHash> vp_probe;
    Prof prof;
    std::mutex dump_lock; // ir_dumped / spv_dumped and the IR dump files (BuildProgram runs on pool threads)
    vk::PipelineCache pipeline_cache;
    size_t cache_saved_size{};
    u64 cache_check_tick{};
    // Shader cache (user/nvn_vk_shaders.bin) and its prewarm. The maps are filled before the prewarm
    // starts and never change shape afterwards; entry contents change under warm_lock.
    std::ofstream shader_out;
    std::unordered_set<u64> recorded_programs, recorded_pipelines;
    std::unordered_set<u64> loaded_programs; // program keys that were in the cache file at start
    std::vector<std::unique_ptr<ProgramRecord>> program_records;
    std::vector<std::unique_ptr<PipelineRecord>> pipeline_records;
    std::unordered_map<u64, std::unique_ptr<WarmProgramEntry>> warm_programs;   // program content key
    std::unordered_map<u64, std::unique_ptr<WarmPipelineEntry>> warm_pipelines; // pipeline content key
    std::unordered_map<u64, std::vector<WarmPipelineEntry*>> warm_by_program;
    std::unordered_map<u64, std::vector<const StageTranscript*>> warm_stage_index; // StageTranscript::quick
    std::mutex warm_lock;
    u32 warm_diag_lines = 0; // guest thread only
    std::condition_variable warm_cv;
    std::atomic<size_t> warm_left{0};
    std::atomic<u64> prewarm_end{0};
    std::atomic<bool> warm_failed{false};
    std::string warm_error;
    u64 prewarm_start{};
    size_t warm_total[2]{};
    u64 shader_written[2]{};
    bool prewarm_logged{};
    bool opt_prescan{true}, opt_prewarm{true};
    bool opt_unsafe_reads{}; // [Debug] nvn_unsafe_reads
    u64 opt_spot_every{1};   // [Debug] nvn_spot_every
    bool opt_prescan_lazy{}; // [Debug] nvn_prescan_lazy
    bool opt_texel_stats{};  // [Debug] nvn_fsr_texel_stats (absent: = gpu_diagnostics)
    bool mv_stat_texels{}, fsr_rstat_texels{}; // the pending once-a-second stats copy includes the full-size images
    const std::vector<std::unique_ptr<NR::List>>* lazy_lists{}; // lazy prescan not run yet in this submit
    size_t lazy_li{}, lazy_ri{};                                // record the replay is executing
    u32 up_cat{};                                  // 0 vb, 1 ib, 2 cb1, 3 ub (set by the caller of Upload)
    // (texture pool, handle) -> image and sampler resolved for it in this submit; dropped whenever an image is created or removed
    struct TexBind {
        Image* img{};
        VkSampler sampler{};
    };
    // hash for the pair / tuple keys of the per-submit caches below (looked up several times per draw)
    struct KeyHash {
        template <class T>
        size_t operator()(const T& t) const {
            size_t h = 0x9e3779b97f4a7c15ull;
            std::apply([&](const auto&... v) { ((h = (h ^ static_cast<size_t>(v)) * 0x100000001b3ull, h ^= h >> 29), ...); }, t);
            return h;
        }
    };
    std::unordered_map<std::pair<u64, u32>, TexBind, KeyHash> tex_bind_cache;
    struct IdxRange {
        u32 lo, hi;
        bool restart;
    };
    std::unordered_map<std::tuple<u64, u32, bool>, IdxRange, KeyHash> idx_range_cache;
    std::vector<u8> idx_scratch;
    std::unordered_map<std::tuple<u64, u32, size_t>, size_t, KeyHash> up_cache; // (va, size, align) -> arena offset, this submit
    const char* upload_fail{""}; // why the last Upload() returned ~0
    std::unique_ptr<WorkerPool> workers;
    State st;
    vk::CommandBuffer vkcmd;
    // a frame split by the FSR 4 handoff: EndFrame submits this command buffer (not frames[cur].cmd), waiting on
    // end_wait_sem (timeline) for end_wait_value
    VkCommandBuffer end_cmd_override{};
    VkSemaphore end_wait_sem{};
    u64 end_wait_value{};
    bool frame_open{};
    bool pass_active{};
    bool pass_dirty{true};
    u32 pass_key_rt{};
    u32 pass_w{}, pass_h{};
    u32 pass_pw{}, pass_ph{}; // the pass's real (scaled) size
    bool pass_viewport_applied{};
    u32 cur{};
    u64 serial{1};
    u64 completed_serial_hint{};
    u64 frame_no{};
    u32 dumped{};
    u32 errors{};
    bool flip_front{}, no_flip_y{}, dump_all{};
    Image* last_present{};
    std::vector<vk::Image> pres_img; // ring, size [Debug] pres_ring (default 3)
    u32 pres_w{}, pres_h{}, pres_next{};
    std::atomic<int> pres_latest{-1};
    u64 pres_ctr{};
    std::vector<u64> pres_seq;
    std::vector<double> pres_done_ms;
    std::atomic<int> shown_idx{-1};         // ring slot the last composite read
    std::atomic<u64> shown_seq{0};        // present-path probe: copy shown by the last composite
    std::atomic<double> shown_done_ms{0.0}; // when that copy finished
    std::chrono::steady_clock::time_point start_time;

    std::unordered_map<u32, std::unique_ptr<Image>> rt_images;
    std::unordered_map<u64, std::unique_ptr<Image>> textures;
    std::unordered_map<u64, std::unique_ptr<vk::Sampler>> samplers;
    // shared: a prewarmed program or pipeline can serve several guest keys
    std::unordered_map<ShaderKey, std::shared_ptr<Program>, ShaderKeyHash> programs;
    std::unordered_map<PipelineKey, std::shared_ptr<Pipeline>, PipelineKeyHash> pipelines;
    std::map<std::string, std::unique_ptr<vk::RenderPass>> render_passes;
    std::map<std::string, std::unique_ptr<vk::Framebuffer>> framebuffers;
    std::vector<std::pair<u64, std::unique_ptr<Image>>> graveyard;
    std::vector<std::pair<u64, std::shared_ptr<void>>> fb_graveyard;
    std::vector<Transition> pending_transitions;
    std::vector<TexUpload> texture_uploads;
    struct CapTex {
        u32 slot;
        Image* img;
        Tegra::Texture::TICEntry tic;
    };
    std::vector<CapTex> cap_tex;
    // [Upscaler] nvn_fsr_split_scale (SplitNow)
    bool split_on{};                 // configured and applicable (FsrSplitScaleActive)
    bool split_failed{};             // a pass could not take it: old single-factor behaviour from then on
    std::set<u32> split_present_keys; // RtKeys of images the game has presented (kept at 1X)
    std::set<std::string> split_notes; // fallback reasons already logged
    // [Debug] nvn_mv_pass state (untouched while the pass is off)
    bool mv_on{};
    bool mv_ready{}, mv_failed{};
    vk::Sampler mv_sampler;
    vk::DescriptorSetLayout mv_set_layout;
    vk::PipelineLayout mv_layout;
    vk::DescriptorUpdateTemplate mv_template;
    vk::ShaderModule mv_module;
    vk::Pipeline mv_pipeline;
    std::unique_ptr<Image> mv_img; // created once at the render size (again only if that changes)
    bool mv_img_fresh{};
    std::set<u32> mv_present_keys; // RtKeys of images the game has presented
    u64 mv_depth_frame{};          // last frame (frame_no + 1 numbering) a depth+colour scene draw ran in
    Image* mv_depth{};             // this frame: scene depth and colour target 0 of the last scene draw
    Image* mv_color{};
    u64 mv_done_frame{};           // frame whose pass ran (frame_no + 1 numbering during the replay)
    MvFrame mv_last{}, mv_stat_info{}, mv_dump_prev{};
    vk::Buffer mv_stat_buf;
    size_t mv_stat_bytes{};
    bool mv_stat_pending{};
    u32 mv_stat_slot{};
    double mv_stat_last_ms{};
    u64 mv_stat_lines{};
    u64 mv_dispatched{}, mv_paired_count{}, mv_reset_count{}, mv_no_depth{};
    bool mv_f2_down{};
    bool mv_chatty{true}; // once-a-second nvn_mv_pass.txt stats (see mv_keys / gpu_diagnostics)
    bool mv_keys{true};  // F2 polled: nvn_mv_pass alone, or nvn_fsr with the debug hooks (NvnFsrDebugHooks)
    bool up_probe{};     // [Debug] nvn_upscaler_probe (cached: tested per draw)
    int mv_dump_stage{}; // F2: 0 idle, 1 = next present reads the previous colour, 2 = the one after writes the dump
    u64 mv_dump_first{};
    std::vector<u8> mv_dump_prev_color;
    std::ofstream mv_log;
#ifdef SUYU_HAS_FSR_VULKAN
    // [Debug] nvn_fsr state (untouched while nvn_fsr is off)
    struct FsrLast {
        u64 frame{};
        float ox{}, oy{}, jx{}, jy{};
        bool reset{}, paired{}, cam_reset{}, gap{};
        float cam_near{}, cam_far{}, fov{}, dt{};
        u32 rw{}, rh{}, ow{}, oh{};
    };
    bool fsr_on{};            // ini nvn_fsr (and SUYU_FSR_VULKAN built)
    bool fsr_chatty{};        // once-a-second / per-frame nvn_fsr.txt lines and the suyu-log mode line: nvn_fsr_debug, nvn_bench or gpu_diagnostics
    std::set<std::string> fsr_hud_errs; // HUD pipeline variant errors already logged (once per distinct text)
    bool fsr_dbg{};           // [Debug] nvn_fsr_debug / nvn_bench (read once): keys, ini re-reads, draw lists; off = constants
    bool fsr_dbg_checking{};  // [Dev] nvn_fsr_debug_checking (own key, not part of nvn_fsr_debug): FFX debug checking create flag
    bool fsr_active{};        // runtime: Numpad1
    bool fsr_failed{};        // unsupported / failed: off for the run
    bool fsr_reactive{};      // runtime: Numpad2
    bool fsr_sharpen{};       // runtime: Numpad5
    float fsr_sharpness{0.3f};
    int fsr_sign{};           // runtime: Numpad3 (jitter sign convention 0..3)
    u32 fsr_view{};           // runtime: Numpad4 (composite mode 0..3)
    bool fsr_menu{};          // menu skip active (FsrMenuUpdate)
    u32 fsr_menu_run{};       // consecutive menu frames
    u64 fsr_scene_draws_frame{};
    bool fsr_menuskip{};            // [Debug] nvn_fsr_menuskip (old menu skip)
    bool fsr_scene_now{true};       // this frame drew its 3D scene (FsrMenuUpdate)
    bool fsr_still_has_scene{};     // still frame that shows a scene image
    u32 fsr_still_kind{2};          // 2 scene, 1 still scene picture, 0 pure 2D
    double fsr_still_log_ms{};
    float fsr_scene_shift[2]{};     // picture shift (jitter) of the last scene frame
    std::unique_ptr<Image> fsr_mv0; // zero motion vectors for still frames
    u64 fsr_sec_still{};
    u32 fsr_still_run{};       // consecutive frames without a scene pass (camera jitter off from 2)
    bool fsr_menu3d_on{true};  // [Debug] nvn_fsr_menu3d
    u32 fsr_menu3d_draw{};     // last frame (still): last opaque mesh draw into H, 0 = none
    u32 fsr_mesh_last{};       // this frame: last opaque mesh draw into H
    double fsr_m3d_log_ms{};
    bool fsr_still_hook{};     // this frame had a main-camera record
    float fsr_still_jit[2]{};  // its applied jitter (jitter-grid px, +y up)
    struct FsrTargetSum {
        u32 ck, dk, n, first;
        u32 cw, ch, dw, dh;
    };
    std::array<FsrTargetSum, 24> fsr_tsum{};
    u32 fsr_tsum_n{};
    u64 fsr_frame_dodraws0{};
    std::string fsr_menu_targets; // target summary of the last menu-skip frame (logged once a second)
    u32 fsr_scene_draws{};    // this frame: render-size colour+depth draws outside H (FsrOnDraw)
    bool fsr_jitter_on{};     // what game_settings was told (FsrSetActive)
    bool fsr_key_down[6]{};
    // automatic benchmark (BenchInit / BenchPoll)
    struct BenchOrig {
        bool active, want4, objv, objmv, anim, reactive, motion, hud;
        int sign;
        u32 view;
        bool sharpen;
        float sharpness;
        u32 sharp_idx;
    };
    BenchOrig bench_orig{};
    bool bench_init_done{}, bench_enabled{}, bench_active{}, bench_apply_pending{}, bench_finish_pending{};
    bool bench_key_down[3]{}; // 0 Numpad *, 1 Numpad +, 2 Numpad -
    int bench_idx{}, bench_sec{}, bench_warm{3}, bench_measure{15}, bench_console_tick{};
    u32 bench_run{}, bench_bad_frames{};
    std::string bench_scene{"village_stand"};
    bool fsr_dump_req{};
    bool fsr_pipes_ready{};
    FsrPipe fsr_prep, fsr_compose, fsr_objs, fsr_obja;
    // [Debug] nvn_fsr_bm_compact = 1: block matching as a classify pass + an indirect search over the listed blocks only
    FsrPipe fsr_objc, fsr_objl;
    bool fsr_bm_compact{};      // ini value
    bool fsr_bm_compact_ok{};   // pipelines + list buffer built (else the one-pass search runs)
    vk::Buffer fsr_bm_list;     // 16 B dispatch args (x = searched blocks, y = z = 1) + one u32 per searched block
    VkDeviceSize fsr_bm_list_bytes{};
    vk::Sampler fsr_near, fsr_linear;
    FfxVkPortableUpscaleContext* fsr_ctx{};
    FfxVkPortableUpscaleCreateInfo fsr_ci{};
    std::vector<std::array<float, 2>> fsr_amd_jitter; // AMD's sequence for the context's sizes (cross-check)
    // [Debug] nvn_fsr_backend = 4: FSR 4 through the driver's DX12 ffx loader (nvn_fsr4.cpp); FSR 3.1 stays the fallback
    int fsr_backend_ini{3};
    bool fsr4_ok{};             // bridge initialised (device extensions, D3D12 device on the LUID, FSR 4.x provider)
    bool fsr4_want{};           // runtime: FSR 4 selected (ini 4; Ctrl+Numpad1 toggles)
    bool fsr4_ran_last{};       // the last FSR dispatch was FSR 4 (a backend change resets the history)
    bool fsr4_drop_ctx{};       // after a failure: rebuild the context + shared images before the next try
    bool fsr4_in_ours{};        // shared input images owned by our queue family (else by D3D12 / EXTERNAL)
    bool fsr4_out_ours{};       // shared output image owned by our queue family
    u32 fsr4_failures{};        // runtime failures this run; 3 -> FSR 3.1 for the rest of the run
    u64 fsr4_retry_frame{};     // after a failure: FSR 3.1 until this frame
    std::string fsr4_reason;    // why FSR 4 is not running (mode line)
    u64 fsr4_sec_frames{}, fsr4_sec_fb{}; // FSR frames this second on FSR 4 / on FSR 3.1 while FSR 4 was wanted
    std::array<VkCommandBuffer, kSlots> fsr4_cmd{}; // per frame slot: the command buffer the frame continues in after the split
    vk::CommandBuffers fsr4_cmd_buffers;
#ifdef SUYU_HAS_FSR4_BRIDGE
    std::unique_ptr<NvnFsr4::Bridge> fsr4;
#endif
    Image* fsr_last_mv_img{};   // the motion vectors the last dispatch read (Numpad6 dump)
    bool fsr_last_react_on{};   // reactive bound in the last dispatch
    std::unique_ptr<Image> fsr_snap, fsr_depth, fsr_react, fsr_out, fsr_comp;
    // reactive mask history (prep shader): silhouette and input colour of the last prep, ping-pong by fsr_hist_idx
    std::array<std::unique_ptr<Image>, 2> fsr_sil, fsr_hist;
    u32 fsr_hist_idx{};             // the slot the last prep wrote
    u64 fsr_hist_frame{};           // frame (FsrDispatch numbering) the last prep wrote the history in, 0 = none
    bool fsr_motion{true};          // [Debug] nvn_fsr_motionmask / Numpad8
    bool fsr_key8_down{};
    // object motion vectors ([Debug] nvn_fsr_objmv / Numpad9): block vectors (RGBA16F, 1 texel per 8x8 block) and the
    // per-texel weight / low-rate reactive (RGBA8), ping-pong by fsr_hist_idx; fsr_mv = the vectors FSR reads when on
    bool fsr_objmv{true};
    bool fsr_key9_down{};
    float fsr_objmv_near{3000.0f};
    std::array<std::unique_ptr<Image>, 2> fsr_blk, fsr_objw;
    std::unique_ptr<Image> fsr_mv;
    u64 fsr_obj_frame{};            // frame the object passes last wrote fsr_blk / fsr_objw in, 0 = none
    float fsr_prev_shift[2]{};      // picture shift (render px, +y down) of the previous FSR frame
    u64 fsr_prev_shift_frame{};
    u32 fsr_rstat_bw{}, fsr_rstat_bh{};
    size_t fsr_rstat_off_w{}, fsr_rstat_off_b{};
    bool fsr_rstat_obj{};
    bool fsr_indicator{false};      // [Debug] nvn_fsr_indicator
    std::unique_ptr<Image> fsr_ind; // menu-skip indicator square (cleared grey, copied into H)
    // reactive mask statistics: once a second fsr_react and the silhouette are copied to fsr_rstat_buf (read at present)
    vk::Buffer fsr_rstat_buf;
    size_t fsr_rstat_bytes{};
    bool fsr_rstat_pending{};
    u32 fsr_rstat_slot{}, fsr_rstat_w{}, fsr_rstat_h{};
    u64 fsr_rstat_frame{};
    bool fsr_rstat_bound{};
    double fsr_rstat_ms{};
    u64 fsr_sec_mask_bound{}, fsr_sec_mask_none{}, fsr_sec_react_bound{};
    bool fsr_need_reset{true};
    u64 fsr_frame_id{};
    u64 fsr_last_frame{};     // frame (frame_no numbering) FSR last ran in
    double fsr_prev_dispatch_ms{};
    // per frame (FsrOnDraw)
    u64 fsr_frame{};
    u32 fsr_draw_idx{}, fsr_src_draws{};
    u32 fsr_src_key{};        // RtKey of the image the final draw samples (HUD image)
    Image* fsr_final_src{};
    Image* fsr_mask_img{};
    u32 fsr_mask_key{}; // for logs (the pointer is only valid during its frame)
    u32 fsr_mask_draw{};
    bool fsr_final_seen{}, fsr_snap_ok{}, fsr_sub_now{};
    u32 fsr_snap_draw{}, fsr_snap_src_draws{};
    u32 fsr_src_w{}, fsr_src_h{};  // size of H (logical), 0 until the final draw was seen
    float fsr_hud_lo{0.08f};       // [Debug] nvn_fsr_hud_lo / _hi (composite strong HUD mask edges), polled once a second
    float fsr_hud_hi{0.25f};
    int fsr_snapdraw{};            // [Debug] nvn_fsr_snapdraw (0 = automatic), polled once a second
    Image* fsr_h_img{};            // H as a render target this frame (valid during the frame only)
    u32 fsr_h_first{~0u}, fsr_h_last_post{~0u}, fsr_h_posts{}; // per frame: H's first draw, last post pass, post passes
    bool fsr_h_run_open{true};     // per frame: no non-post draw into H yet
    bool fsr_snap_pending{};       // per frame: a post pass into H happened after the last copy
    u32 fsr_snap_why{}, fsr_snap_post{~0u}, fsr_snap_posts{}, fsr_snap_hfirst{~0u}, fsr_snap_mark{~0u};
    std::array<FsrDrawRec, kFsrDrawRecs> fsr_recs{}, fsr_list{}; // this frame's H draws / the copy that gets logged
    u32 fsr_recs_n{}, fsr_list_n{};
    u64 fsr_list_frame{};
    std::string fsr_snap_sig;      // last logged "where the snapshot went" signature
    std::array<s64, 4> fsr_snap_shape{-1, -1, -1, -1}; // the same as integers (no debug hooks: tested per frame instead of strings)
    double fsr_list_ms{};          // when the draw list was last logged
    std::vector<std::pair<u32, u32>> fsr_sub_pos; // (stage, index in that stage's texture descriptors) of the HUD image
    std::vector<u32> fsr_sub_rescale;             // split scale: rescale bit index of those descriptors (~0u = array)
    // HUD layer ([Debug] nvn_fsr_hudlayer / Numpad7)
    bool fsr_hudlayer{true};        // wanted (ini, Numpad7)
    bool fsr_key7_down{};
    std::unique_ptr<Image> fsr_layer, fsr_merge;
    bool fsr_layer_on{};            // per frame: draws into H go to the layer now
    bool fsr_layer_used{};          // per frame: the layer holds this frame's HUD (not merged into H)
    bool fsr_layer_aborted{};       // per frame: the redirect was abandoned (no restart this frame)
    Image* fsr_layer_h{};           // H while the redirect is on (valid during the frame only)
    u32 fsr_layer_draws{}, fsr_layer_begin_draw{};
    bool fsr_redirect_now{};        // this draw goes into the layer (FsrOnDraw -> DoDraw)
    u32 fsr_redirect_class{};
    bool fsr_final_now{};           // this draw is the final draw
    VkPipeline fsr_hud_pipe{};      // this draw's layer pipeline variant
    std::unordered_map<PipelineKey, vk::Pipeline, PipelineKeyHash> fsr_hud_pipes;
    std::map<std::string, u32> fsr_hud_combos; // distinct blend combinations of the variants -> variant count
    u64 fsr_hud_variants{};
    u32 fsr_frame_path{}, fsr_frame_path_draws{}; // last final draw: FsrPathText, redirected draws
    double fsr_layer_note_ms{};
    u64 fsr_sec_layer{}, fsr_sec_diff{}, fsr_sec_skip{}, fsr_sec_none{}, fsr_sec_redirected{};
    u64 fsr_sec_aborts{}, fsr_sec_merges{}, fsr_sec_clears{}, fsr_sec_rt_reads{};
    // HUD layer, opaque draws without alphaToOne (class 4): the alpha-only logic-op pipeline of the pair, this draw's
    VkPipeline fsr_hud_pipe2{};
    std::unique_ptr<Image> fsr_tm;  // HUD multiply transmittance (class 6 draws), H size
    bool fsr_tm_used{};             // per frame: a multiply draw ran (fsr_tm valid)
    bool fsr_tm_target{};           // BeginPass: H's attachment is fsr_tm (multiply draw, second pass)
    u64 fsr_sec_multiply{};
    std::unordered_map<PipelineKey, vk::Pipeline, PipelineKeyHash> fsr_hud_pipes_a;
    u64 fsr_sec_alpha_pass{};
    float fsr_sharpness_ini{0.3f}; // [Debug] nvn_fsr_sharpness (Numpad5 cycles off / ini / 0.6 / 1.0)
    u32 fsr_sharp_idx{};
    float fsr_react_strength{0.9f}; // [Debug] nvn_fsr_reactive_strength
    // geometry object motion vectors ([Debug] nvn_fsr_objvel, Numpad9; see ObjvOnDraw / ObjvRun)
    int objv_mode{};                // ini mode 0..3
    bool objv_ok{};                 // mode > 0 and the device can capture (transform feedback), nothing failed
    bool objv_on{};                 // runtime (Numpad9)
    bool objv_buf_failed{}, objv_vel_ready{}, objv_vel_failed{};
    float objv_react_cap{0.1f}, objv_max_px{64.0f};
    std::array<vk::Buffer, 2> objv_xfb; // capture buffers, slot = frame & 1
    u32 objv_slot{};
    u64 objv_cur_frame{}, objv_prev_frame{};
    std::vector<ObjvEnt> objv_cur, objv_prev; // this frame's / the previous frame's captures
    VkDeviceSize objv_used{};
    std::unordered_set<u64> objv_geo_mask;      // geometry keys captured in the mask pass this frame
    std::unordered_set<u64> objv_geo_mask_all;  // identity keys (dkey) of every draw into the mask target this frame
    std::vector<u8> objv_vscratch;
    u64 objv_ttl{600};
    struct ObjvHistEnt {
        u64 vs{}, fs{}, n{};
        u32 count{}, topology{};
        bool blended{}, dyn{}, vstex{}, in_mask{}, vchanged{};
        std::string cbufs;
    };
    std::unordered_map<u64, ObjvHistEnt> objv_hist;
    u64 objv_sec_cap_vdyn{}, objv_sec_jumps{};
    u32 objv_hist_secs{};
    u64 objv_menu_frame{};       // frame whose menu-3D meshes were captured
    Image* objv_menu_depth{};    // their depth target (valid during that frame)
    bool objv_signals{};        // [Debug] nvn_fsr_signals
    bool objv_static_on{true};  // [Debug] nvn_fsr_objvel_static
    u32 objv_static_frames{240}; // [Debug] nvn_fsr_static_frames: static checked frames before the exclusion
    u32 objv_static_rearm{200};  // credit kept after an exclusion ends (frames - 40, min 0)
    struct ObjvStaticRec {
        u32 stable{};
        u64 excl_until{};
    };
    std::unordered_map<u64, ObjvStaticRec> objv_static;
    vk::Buffer objv_flags_buf; // per frame slot: kObjvMaxEnts draw flags (host visible)
    std::array<std::vector<u64>, kSlots> objv_flags_dkeys;
    std::array<u64, kSlots> objv_flags_frame{};
    u64 objv_sec_sk_static{}, objv_sec_seen_moving{}, objv_sec_newly_static{};
    double objv_sec_sig_ms{}, objv_sec_run_ms{};
    // FSR cost: GPU timestamps per frame slot (FsrTs), CPU per category
    std::array<bool, kSlots> fsr_ts_pending{};
    double fsr_ts_sum[6]{};
    u32 fsr_ts_n{};
    double fsr_sec_dispatch_cpu_ms{};
    bool fsr_anim_bypass{true};
    bool fsr_anim_gate{true}; // [Debug] nvn_fsr_animsurf_edge_gate
    // Numpad1 A/B: frames and time since the last toggle
    u64 fsr_ab_frames{};
    double fsr_ab_ms{};
    // animated surfaces ([Debug] nvn_fsr_animsurf, Numpad0)
    bool fsr_anim{true}, fsr_key0_down{};
    float fsr_anim_thr{0.03f};
    FsrPipe fsr_anim_pipe;
    std::array<std::unique_ptr<Image>, 2> anim_tile;
    std::unique_ptr<Image> anim_stat;
    vk::Buffer anim_stat_buf;
    bool anim_stat_want{}, anim_stat_pending{};
    u64 anim_frame{};
    u32 anim_wr{};
    u64 anim_sec_frames{};
    struct ObjvSlotRec {
        u32 n{};
        u8 bytes[kSigSlotBytes]{};
    };
    struct ObjvSigRec {
        u64 frame{}, vhash{}, thash{}, tchash{}, vchange{}, jump_frame{};
        ObjvSigCounter vtx, tex, texel;
        ObjvSlotRec slot[2][14];
    };
    std::unordered_map<u64, ObjvSigRec> objv_sig;    // per draw identity: pose-update signals (ObjvSignals)
    struct ObjvSlotStat {
        ObjvSigCounter all;
        ObjvSigCounter chunk[kSigSlotBytes / 16];
    };
    struct ObjvSigStat {
        u64 vs{}, fs{}, draws{};
        ObjvSigCounter vtx, tex, texel;
        ObjvSlotStat slot[2][14];
    };
    std::unordered_map<u64, ObjvSigStat> objv_sigstat; // per (vs, fs), logged every 5 s
    u64 objv_sec_updates{};
    std::unordered_map<u64, u32> objv_ub_count_cur;  // this frame: draws per vertex-stage constant buffer address
    std::unordered_set<u64> objv_ub_global;          // addresses bound by > 32 tracked draws last frame (not hashed)
    std::vector<u8> objv_uscratch;
    u32 objv_max_verts{4000000};
    u64 objv_frame_verts{};
    u64 objv_sec_sk_vcap{}, objv_sec_verts_max{};
    double objv_sec_cpu_ms{};
    // GPU time of the velocity pass + merge: timestamp pair per frame slot
    vk::QueryPool objv_ts_pool;
    PFN_vkCmdWriteTimestamp objv_write_ts{};
    float objv_ts_period{};
    std::vector<u8> objv_ts_written;
    double objv_sec_gpu_ms{};
    u32 objv_sec_gpu_n{};
    bool objv_barrier_done{};
    std::vector<std::pair<u32, u32>> objv_rt_counts; // per frame: draws per scene colour target (mask pass detection)
    u32 objv_rt_mask{};
    bool objv_mask_ok{};
    bool objv_cap_now{};            // this draw is captured
    bool objv_cap_ts{};             // [Debug] nvn_fsr_capture_ts (timestamps around every capture; needs gpu_diagnostics)
    u64 objv_perf_caps{};           // captures recorded since the last nvn_perf S line (objv_cap= per frame)
    double fsr4_sec_d3d12_ms{};     // last second's FSR 4 dispatch GPU avg (D3D12 timestamps), for the S line
    VkPipeline objv_cap_pipe{};
    std::unordered_map<PipelineKey, vk::Pipeline, PipelineKeyHash> objv_pipes; // capture variants of the game's pipelines
    std::unordered_map<PipelineKey, vk::Pipeline, PipelineKeyHash> objv_pipes_rd; // capture-only (list, rasterizer discard)
    std::map<std::tuple<u64, u32, bool, u32>, std::vector<u32>> objv_expand_cache; // restart strip -> list indices
    u64 objv_pipes_built{};
    double objv_pipes_ms{}, objv_sec_pipe_ms{};
    vk::DescriptorSetLayout objv_set_layout;
    vk::PipelineLayout objv_layout;
    vk::DescriptorUpdateTemplate objv_tmpl;
    vk::ShaderModule objv_vs, objv_fs;
    vk::RenderPass objv_rp;
    vk::Pipeline objv_pipe;
    vk::Framebuffer objv_fb;
    VkImageView objv_fb_view{};
    FsrPipe objv_merge;
    std::unique_ptr<Image> objv_vel, objv_mvo, objv_stat;
    std::unordered_map<u64, std::vector<u32>> objv_pf, objv_pl;
    std::unordered_map<u64, u32> objv_cf, objv_cl;
    std::vector<std::pair<u32, u32>> objv_pairs;
    vk::Buffer objv_stat_buf;
    bool objv_stat_want{}, objv_stat_pending{};
    u64 objv_stat_frame{};
    u32 objv_stat_texels{}, objv_stat_pairs{};
    u64 objv_vel_frame{}; // frame whose velocity pass objv_vel holds (Numpad6 dump)
    u64 objv_sec_frames{}, objv_sec_nocap{}, objv_sec_nohist{}, objv_sec_passes{}, objv_sec_cap_mask{}, objv_sec_cap_scene{};
    u64 objv_sec_sk_dedup{}, objv_sec_bytes_max{}, objv_sec_matched{}, objv_sec_m_order{}, objv_sec_m_loose{}, objv_sec_unmatched{};
    u64 objv_sec_ambiguous{}, objv_sec_mismatch{}, objv_sec_sk_topo{}, objv_sec_sk_restart{}, objv_sec_sk_big{}, objv_sec_sk_full{};
    u64 objv_sec_sk_nopipe{}, objv_sec_verts{};
    u64 objv_sec_cap_expanded{}, objv_sec_sk_unread{}, objv_sec_sk_arena{}, objv_sec_sk_target{}, objv_sec_sk_mode{};
    u64 objv_sec_sk_maskoff{}, objv_sec_sk_blend{}, objv_sec_sk_nodepth{}, objv_sec_sk_noskin{};
    // log
    FsrLast fsr_last{};
    u64 fsr_presents{}, fsr_dispatched{}, fsr_resets{}, fsr_errors{};
    u64 fsr_skip_nosnap{}, fsr_skip_nosrc{}, fsr_skip_nomv{}, fsr_skip_size{};
    u32 fsr_error_run{};
    std::string fsr_skip_size_text, fsr_last_error;
    double fsr_rec_sum{}, fsr_rec_max{}, fsr_line_ms{};
    std::ofstream fsr_log;
#endif
};

std::unique_ptr<Renderer> g_renderer;

#ifdef _WIN32
/// First line of each process's records in nvn_vk_crash.txt (the file accumulates over runs): pid, exe file name, PE link
/// timestamp / image size / checksum (dumpbin /headers shows the same values for an exe on disk) and the source revision,
/// so a crash file sent from another machine can be matched to the exe that wrote it. Built once in NvnVkStart.
std::string g_crash_build_line;

std::string CrashBuildLine() {
    const auto* exe = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') name = p + 1; // file name only: no user paths in the record
    }
    return fmt::format("=== process {} | exe {} | PE timestamp {:08x} size_of_image {:x} checksum {:08x} | source {} {} built {}",
                       GetCurrentProcessId(), name, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum,
                       Common::g_scm_rev, Common::g_scm_branch, Common::g_build_date);
}

/// Unwinds the faulting thread's stack from the exception context (not the handler's own stack). Plain data only, so the
/// SEH guard can stay: a corrupt stack ends the walk instead of faulting inside the handler.
int CrashWalkStack(const CONTEXT* start, u64 lo, u64 hi, u64* pcs, int max) {
    int n = 0;
#if defined(_M_X64)
    __try {
        CONTEXT ctx = *start;
        while (n < max && ctx.Rip != 0) {
            pcs[n++] = ctx.Rip;
            DWORD64 image_base = 0;
            if (const PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr)) {
                PVOID handler_data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
            } else { // leaf function: the return address is at rsp
                if (ctx.Rsp < lo || ctx.Rsp + 8 > hi) break;
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
            if (ctx.Rsp < lo || ctx.Rsp >= hi) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    (void)start, (void)lo, (void)hi, (void)pcs, (void)max;
#endif
    return n;
}

/// "<module>+<offset>" (the exe as "rva <offset>", the form the older records and the symboliser notes use).
std::string CrashWhere(u64 pc, u64 exe_base) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(pc), &mod) || mod == nullptr) {
        return fmt::format("{:x} (no module)", pc);
    }
    const u64 base = reinterpret_cast<u64>(mod);
    if (base == exe_base) return fmt::format("rva {:x}", pc - base);
    char path[MAX_PATH]{};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') name = p + 1;
    }
    return fmt::format("{}+{:x}", name, pc - base);
}

/// Vectored handler: sees FIRST-CHANCE access violations of every thread, including ones that are handled later
/// (SehRun's own __except, dynarmic fastmem, drivers/overlays probing memory). So a record is not proof of a renderer
/// fault: it says which thread and module faulted, and the renderer phase only for our own threads. The real unhandled
/// signal is suyu's "[crash] UNHANDLED" log line. No dbghelp here (not thread safe, slow): symbolise offline with the pdb.
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != 0xc0000005) return EXCEPTION_CONTINUE_SEARCH;
    thread_local bool in_handler = false;
    if (in_handler) return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<u32> records{0};
    if (records.fetch_add(1) >= 64) return EXCEPTION_CONTINUE_SEARCH; // a storm of handled faults: keep the first ones only
    in_handler = true;
    const u64 exe_base = reinterpret_cast<u64>(GetModuleHandleA(nullptr));
    const u64 at = reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    const CONTEXT* c = info->ContextRecord;
#if defined(_M_X64)
    const u64 sp = c->Rsp;
#else
    const u64 sp = 0;
#endif
    const char* role = t_nvn_role;
    std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_vk_crash.txt", std::ios::app);
    static std::atomic<bool> header{false};
    if (!header.exchange(true)) out << (g_crash_build_line.empty() ? std::string("=== process (no build line)") : g_crash_build_line) << "\n";
    const std::string where = CrashWhere(at, exe_base);
    out << fmt::format("access violation at {}, address {:x}, write {}; {} | first-chance (may be handled later) | thread {} {} | sp {:x} stack {:x}-{:x}\n",
                       where, info->ExceptionRecord->ExceptionInformation[1], info->ExceptionRecord->ExceptionInformation[0],
                       role ? fmt::format("phase {} detail {:x} line {}", g_phase, static_cast<u64>(g_detail), static_cast<int>(g_line))
                            : fmt::format("not a renderer thread (render phase was {})", g_phase),
                       GetCurrentThreadId(), role ? role : "other", sp, lo, hi);
    u64 pcs[32];
    const int n = CrashWalkStack(c, lo, hi, pcs, 32);
    for (int i = 0; i < n; ++i) out << fmt::format("  #{} {}\n", i, CrashWhere(pcs[i], exe_base));
    in_handler = false;
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void SubmitSink(Core::Memory::Memory& mem, std::vector<std::unique_ptr<NR::List>>&& lists, u32) {
    if (g_renderer) g_renderer->Submit(mem, std::move(lists));
}
void PresentSink(Core::Memory::Memory& mem, u32 texture) {
    if (g_renderer) g_renderer->Present(mem, texture);
}

} // namespace

static void SyncProbe(u32 id, u32 value, u32 guest, double t);
void NvnVkStart(const Device& device, MemoryAllocator& allocator) {
    for (const char* f : {"nvn_ours_draws.txt", "suyu_draws.txt", "nvn_ours_rtdraws.txt", "suyu_rtdraws.txt", "nvn_ours_regions.txt", "suyu_regions.txt", "nvn_ours_texhash.txt", "suyu_texhash.txt", "nvn_ours_bindprog.txt", "nvn_ours_cbinds.txt"}) {
        std::error_code ec;
        std::filesystem::remove(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / f, ec);
    }
    if (Core::GameSettings::NvnVulkanMode() < 2) return;
#ifdef _WIN32
    g_crash_build_line = CrashBuildLine();
    AddVectoredExceptionHandler(1, CrashHandler);
#endif
    try {
        g_renderer = std::make_unique<Renderer>(device, allocator);
        if (g_renderer->enabled) {
            g_renderer->StartRenderThread();
            NR::SetSink(SubmitSink);
            NR::SetPresentSink(PresentSink);
            if (Kernel::StallRing::On() && Core::GameSettings::NvnVulkanMode() == 3) Kernel::StallRing::g_sync_probe = &SyncProbe;
        }
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "NVN renderer failed to start: {}", e.what());
        g_renderer.reset();
    }
}

namespace {
std::atomic<unsigned> g_suyu_frame{0};
}
unsigned NvnVkSuyuFrame() { return g_suyu_frame.load(); }
std::atomic<bool> g_capture{false};
bool NvnVkCaptureActive() { return g_capture.load(); }
void NvnVkSuyuFrameTick() {
    // Wiggle-hitch comparison: with our renderer off the gap detector runs on suyu's composites.
    if (Kernel::StallRing::On() && !(g_renderer && g_renderer->enabled)) {
        static double last_ms = 0;
        static u32 gap_dumps = 0;
        const double now_ms = NvnVkNowMs();
        if (last_ms != 0 && now_ms - last_ms > 60.0 && ++gap_dumps <= 60) {
            std::string text = "SUYU-ONLY MODE (our renderer off) " + Prof::StallEventText(g_suyu_frame, now_ms - last_ms, now_ms);
            if (Kernel::StallRing::g_guest_probe) {
                text += Kernel::StallRing::g_guest_probe(last_ms, now_ms);
            }
            NvnVkDumpLine("nvn_stall.txt", text);
        }
        last_ms = now_ms;
    }
    // Create user\capture.txt while standing in the spot to dump the next frame in full (one capture per run).
    static bool armed = false;
    if (armed) {
        armed = false;
        g_capture = true;
    } else {
        g_capture = false;
    }
    const auto f = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "capture.txt";
    std::error_code ec;
    // F9 (global key state, works with the game window focused) takes the capture.
    static bool f9_was = false;
    const bool f9 = Common::HostKeyDown(0x78);
    if (f9 && !f9_was) armed = true;
    f9_was = f9;
    if (++g_suyu_frame % 30 == 0 && std::filesystem::exists(f, ec)) {
        std::filesystem::remove(f, ec);
        armed = true;
    }
}
// Present-path probe, called from RendererVulkan::Composite in present mode: stage 0 at entry,
// stage 1 after present_manager.Present. Logs which of our copies the composite showed.
static void SyncProbe(u32 id, u32 value, u32 guest, double t) {
    PpLine(fmt::format("Y{} id {} value {} t{:.1f}", guest ? "g" : "h", id, value, t));
}

void NvnVkCompositeProbe(int stage) {
    if (!(g_renderer && g_renderer->enabled)) return;
    if (!Core::GameSettings::GpuDiagnosticsEnabled()) return; // per-composite C line: logging only
    static double t_enter = 0, last_enter = 0;
    static u64 n = 0, prev_seq = 0;
    const double now = NvnVkNowMs();
    if (stage == 0) {
        t_enter = now;
        return;
    }
    const u64 seq = g_renderer->shown_seq.load();
    const double done = g_renderer->shown_done_ms.load();
    const double dt = last_enter != 0 ? t_enter - last_enter : 0.0;
    last_enter = t_enter;
    const s64 ds = static_cast<s64>(seq) - static_cast<s64>(prev_seq);
    prev_seq = seq;
    const char* flag = "";
    if (n > 1) {
        if (ds == 0) flag = " DUP";
        else if (ds > 1) flag = " SKIP";
    }
    ++n;
    PpLine(fmt::format("C{} t{:.1f} dt {:.1f} | shows P{} ring {} (d{:+}) age {:.1f} | composite {:.1f}{}{}", n, t_enter, dt, seq,
                       g_renderer->shown_idx.load(), ds, t_enter - done, now - t_enter, flag, dt > 25.0 ? " LATE-COMPOSITE" : ""));
}

namespace {
std::mutex g_dump_line_lock;
std::string g_bq_buf; // nvn_bq.txt lines not yet appended (same order as written)
double g_bq_flush_ms = 0;

void FlushBqLocked() {
    if (g_bq_buf.empty()) return;
    std::ofstream f(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_bq.txt", std::ios::app);
    f << g_bq_buf;
    g_bq_buf.clear();
}
} // namespace

void NvnVkDumpLine(const char* file, const std::string& text) {
    std::scoped_lock lk{g_dump_line_lock};
    if (std::strcmp(file, "nvn_bq.txt") == 0) {
        // [Debug] probes: several lines per frame from the buffer queue and the present. One open/append/close per line cost
        // the render and guest threads a file open per call; the lines are kept in order and appended about once a second.
        g_bq_buf += text;
        g_bq_buf += '\n';
        const double now = NvnVkNowMs();
        if (now - g_bq_flush_ms >= 1000.0 || g_bq_buf.size() >= (256u << 10)) {
            g_bq_flush_ms = now;
            FlushBqLocked();
        }
        return;
    }
    if (!g_bq_buf.empty() && std::strcmp(file, "nvn_stall.txt") == 0) FlushBqLocked(); // a stall dump: nvn_bq.txt complete up to it
    std::ofstream f(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / file, std::ios::app);
    f << text << "\n";
}

void NvnVkFlushDumpLines() {
    std::scoped_lock lk{g_dump_line_lock};
    FlushBqLocked();
}

double NvnVkNowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string NvnVkBenchStatus() {
    std::lock_guard lock(g_bench_status_mutex);
    return g_bench_status;
}

bool NvnVkPresentImage(VkImage& image, unsigned& width, unsigned& height) {
    return g_renderer && g_renderer->enabled && g_renderer->PresentedImage(image, width, height);
}

void NvnVkPresentImageUsed(MasterSemaphore& sem, u64 tick) {
    if (!(g_renderer && g_renderer->enabled) || !Core::GameSettings::PresentNoDrain()) return;
    std::scoped_lock lk{g_renderer->pres_lock};
    g_renderer->pres_sem = &sem;
    const int i = g_renderer->pres_handed;
    if (i >= 0 && static_cast<size_t>(i) < g_renderer->pres_read_tick.size()) g_renderer->pres_read_tick[i] = tick;
    g_renderer->pres_handed = -1;
}

void NvnVkSavePng(const char* name, unsigned w, unsigned h, const unsigned char* rgba) {
    Renderer::WritePng(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name, w, h, rgba);
}

void NvnVkWaitRenderIdle() {
    if (g_renderer) g_renderer->WaitRenderIdle();
}

void NvnVkStop() {
    NR::SetSink(nullptr);
    NR::SetPresentSink(nullptr);
    if (g_renderer) g_renderer->StopRenderThread();
    g_renderer.reset();
    NvnVkFlushDumpLines();
}

} // namespace Vulkan
