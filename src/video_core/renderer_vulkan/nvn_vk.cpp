// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// G3c, shadow stage: executes the NVN call lists the recorder (core/arm/recomp/nvn_rec) hands over at
// every queue submit, on Vulkan, into images of our own. Everything NVN-side comes from the recorded
// arguments and the objects they point at; guest memory is read with the pool map the game gave NVN.
// Texture descriptors are still read from the pools the guest driver fills (the TIC/TSC decoders are
// suyu's), until the native pool registration exists. Nothing here is shown; frames are dumped.

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
#include "common/logging/log.h"
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
constexpr u32 kShaderCacheVersion = 2; // 2: program keys cover only the texture slots a stage samples

/// Largest 2D image side the GPU allows (VkPhysicalDeviceLimits::maxImageDimension2D); set when the renderer starts.
std::atomic<u32> g_max_image_dim{16384};

/// suyu's Graphics > Resolution scale, applied to every render target of the replacement renderer
/// (the game's picture stays 1920x1080-based; targets, viewports and scissors are scaled).
float RenderScaleFactor() {
    static const float factor = [] {
        // The shader recompiler's rescaling pass bakes in the same Settings::values.resolution_info.
        Settings::UpdateRescalingInfo();
        auto& info = Settings::values.resolution_info;
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
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<u64>(t.QuadPart);
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

    /// Returns an empty string on success, else what failed (the caller stops loudly).
    std::string Open(const std::filesystem::path& path) {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return "QueryPerformanceFrequency failed";
        ms_per_tick = 1000.0 / static_cast<double>(f.QuadPart);
        slow_frame_ticks = static_cast<u64>(f.QuadPart) * 25 / 1000;
        slow_event_ticks = static_cast<u64>(f.QuadPart) * 10 / 1000;
        second_ticks = static_cast<u64>(f.QuadPart);
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
class WorkerPool {
public:
    explicit WorkerPool(u32 count) {
        for (u32 i = 0; i < count; ++i) threads.emplace_back([this] { Loop(); });
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
#ifdef _WIN32
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vkey);
#endif

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
    friend void ::Vulkan::NvnVkCompositeProbe(int);

public:
    Renderer(const Device& device_, MemoryAllocator& allocator_) : device{device_}, allocator{allocator_}, dev{device_.GetLogical()} {
        g_max_image_dim = device_.GetPhysical().GetProperties().limits.maxImageDimension2D;
        const std::string opts = OptionsText();
        flip_front =opts.find("flipfront") != std::string::npos;
        no_flip_y = opts.find("noflipy") != std::string::npos;
        if (const char* e = std::getenv("NVNVK_DUMP")) dump_all = std::atoi(e) != 0;
        if (!device.HasSeparateNvnQueue() || !device.IsKhrPushDescriptorSupported()) {
            Note(fmt::format("disabled: separate queue {} push descriptors {}", device.HasSeparateNvnQueue(),
                             device.IsKhrPushDescriptorSupported()));
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
        LoadPipelineCache();
        // [Debug] A/B switches (defaults on): nvn_prescan, nvn_prewarm, nvn_workers (count; 0 = build inline in the replay).
        opt_prescan = DebugOption("nvn_prescan", 1) != 0;
        opt_prewarm = DebugOption("nvn_prewarm", 1) != 0;
        {
            const int w = DebugOption("nvn_workers", -1);
            const u32 hc = std::thread::hardware_concurrency();
            const u32 count = w >= 0 ? static_cast<u32>(w) : std::max<u32>(2, hc > 1 ? hc - 1 : 2);
            if (count > 0) workers = std::make_unique<WorkerPool>(count);
        }
        cache_check_tick = ProfNow();
        const std::string opts_line = fmt::format("options: prescan {} prewarm {} workers {}{}", opt_prescan, opt_prewarm, workers ? workers->Size() : 0,
                                                  workers ? "" : " (prescan and prewarm need workers: both off, everything builds inline)");
        Note(opts_line);
        prof.Line("P " + opts_line);
        LoadShaderCache();
        if (opt_prewarm && workers) {
            StartPrewarm();
        } else {
            prewarm_logged = true;
            prof.Line("P prewarm off");
        }
        Note(fmt::format("started; arena {} MB x {}, flipfront={} noflipy={}", kArenaBytes >> 20, kSlots, flip_front, no_flip_y));
    }

    ~Renderer() {
        StopRenderThread();
        if (enabled) {
            workers.reset(); // background prewarm jobs use the members below
            device.GetLogical().WaitIdle();
            SavePipelineCache();
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

    /// Missing support is a loud stop on purpose: write the report, tell the player, end the process.
    [[noreturn]] void Fatal(const std::string& what) {
        Note("FATAL: " + what);
        WriteStats();
        const std::string text = "NVN Vulkan renderer: " + what + "\nSee nvn_vk.txt / nvn_vk_crash.txt in the user folder.";
        MessageBoxA(nullptr, text.c_str(), "HEY - something is missing", MB_OK | MB_ICONERROR | MB_TOPMOST);
        TerminateProcess(GetCurrentProcess(), 3);
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
        last_submit_end = ProfNow();
        if (Core::GameSettings::NvnVulkanMode() == 3) {
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
            ProfScope ps{prof, PB_CacheSave}; // lands in the next frame's line
            SavePipelineCache();
        }
    }

    void PresentBody(Core::Memory::Memory& mem, u32 texture) {
        ++stats.presents;
        ++frame_no;
        try {
            if (const auto it = rt_images.find(RtKey(mem, texture)); it != rt_images.end()) {
                last_present = it->second.get();
                cur_tex = tex_index.try_emplace(texture, static_cast<u32>(tex_index.size())).first->second;
                if (Core::GameSettings::NvnVulkanMode() == 3) PresentCopy(*last_present);
                const bool want = dump_all ? (frame_no % 600 == 0) : (frame_no == 600 || NvnVkCaptureActive());
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
        if (log_lines.size() < 400) log_lines.push_back(s);
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
            .support_int16 = device.IsShaderInt16Supported(),
            .support_uniform_and_storage_buffer_16bit = device.IsUniformAndStorageBuffer16BitAccessSupported(),
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
            .support_sampled_image_array_nonuniform_indexing = device.IsSampledImageArrayNonUniformIndexingSupported(),
            .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),
            .lower_left_origin_mode = false,
            .need_declared_frag_colors = false,
            .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY || driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                           driver_id == VK_DRIVER_ID_MESA_RADV || driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                           driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA,
            .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
            .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
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
            .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
            .support_conditional_barrier = device.SupportsConditionalBarriers(),
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
    __declspec(noinline) void UploadFailure(const char* what, u64 va, u32 size) {
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

    Image* RenderTarget(Core::Memory::Memory& mem, u32 tex, bool depth) {
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
        if (it != rt_images.end() && it->second->width == w && it->second->height == h) {
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
        ScaledSize(w, h, img->pw, img->ph);
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
        rt_images[key] = std::move(img);
        return raw;
    }

    // Texture sampled through a descriptor the guest driver wrote (TIC), decoded with suyu's code.
    Image* SampledImage(Core::Memory::Memory& mem, const Tegra::Texture::TICEntry& tic) {
        const u64 va = tic.Address();
        u32 cpu = 0, avail = 0;
        if (!NR::GpuToCpu(mem, va, cpu, avail)) {
            ++stats.notes["texture: address not in a known pool"];
            if (stats.notes.size() < 60) {
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
                std::vector<u8> bytes(n);
                mem.ReadBlock(cpu, bytes.data(), n);
                for (u32 i = 0; i < n; i += 8) {
                    u64 v;
                    std::memcpy(&v, bytes.data() + i, 8);
                    c = (c ^ v) * 1099511628211ull;
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
            if (it->second && !tic.IsPitchLinear() && it->second->spot_serial != serial) {
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
        for (u32 i = 0; i < n; ++i) {
            const u32 off = n > 1 ? static_cast<u32>(u64{bytes - 8} * i / (n - 1)) & ~7u : 0;
            u64 v;
            mem.ReadBlock(cpu + off, &v, 8);
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
        if (guest_size == 0 || guest_size > avail || info.layer_stride == 0 && info.resources.layers > 1) {
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
            .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : VkImageCreateFlags{0},
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
    __declspec(noinline) void DumpDrawState(Core::Memory::Memory& mem, const State& ds, const std::array<u32, 8> (&handles)[2],
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
        queue.Submit(si, *util_fence);
        {
            ProfScope ps{prof, PB_WaitDump};
            util_fence.Wait();
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
    __declspec(noinline) std::unique_ptr<Program> BuildProgram(Core::Memory::Memory* mem, u64 vs_addr, u64 fs_addr, u32 tex_pool,
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
            for (u32 si = 0; si < 2; ++si) {
                const u64 pa = s.prog[si];
                if (pa == 0x410220100ull || pa == 0x40fadee00ull || pa == 0x4102cdb00ull || pa == 0x40e27da00ull || pa == 0x40d513800ull ||
                    pa == 0x40cdc3a00ull || pa == 0x40ced0100ull || pa == 0x40e3ac800ull || pa == 0x40ded1300ull || pa == 0x40fd9a200ull ||
                    pa == 0x40dc5b400ull || pa == 0x40e4cc500ull || pa == 0x40e01ad00ull || pa == 0x40ff1c600ull || pa == 0x40e2c7f00ull || pa == 0x410215e00ull) {
                    std::ofstream ir(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / fmt::format("nvn_vk_shadow_ir_{:x}.txt", pa), std::ios::trunc);
                    ir << Shader::IR::DumpProgram(p->t[si]->program);
                }
            }
            for (u32 si = 0; si < 2; ++si) {
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
            if (Kernel::StallRing::TimedGuard dl{dump_lock, Kernel::StallRing::LockDump}; spv_dumped < 8) {
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
    __declspec(noinline) const StageTranscript* MatchStage(Core::Memory::Memory& mem, u64 nvn_addr, Shader::Stage stage, bool* had_candidates = nullptr) {
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
    __declspec(noinline) void DiagWarmMiss(u64 vs_addr, u64 fs_addr, bool vs_cand, bool fs_cand, const StageTranscript* vt,
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
    __declspec(noinline) std::shared_ptr<Program> WarmProgramFor(Core::Memory::Memory& mem, u64 vs_addr, u64 fs_addr, u32 tex_pool,
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

    void BeginFrame() {
        cur = (cur + 1) % kSlots;
        auto& f = frames[cur];
        if (f.pending) {
            ProfScope ps{prof, PB_WaitFrame};
            f.fence.Wait();
            f.pending = false;
            ++completed_serial_hint;
        }
        f.fence.Reset();
        f.used = 0;
        up_cache.clear();
        idx_range_cache.clear();
        tex_bind_cache.clear();
        VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .pNext = nullptr,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                    .pInheritanceInfo = nullptr};
        vkcmd = vk::CommandBuffer(f.cmd, device.GetDispatchLoader());
        vkcmd.Begin(bi);
        frame_open = true;
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
        vkcmd.End();
        auto& f = frames[cur];
        VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .pNext = nullptr,
                              .waitSemaphoreCount = 0,
                              .pWaitSemaphores = nullptr,
                              .pWaitDstStageMask = nullptr,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &f.cmd,
                              .signalSemaphoreCount = 0,
                              .pSignalSemaphores = nullptr};
        {
            ProfScope ps{prof, PB_Submit};
            queue.Submit(si, *f.fence);
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
        for (u32 i = 0; i < st.rt_count; ++i) {
            Image* img = RenderTarget(mem, st.rt_tex[i], false);
            if (!img) return false;
            views.push_back(*img->view);
            w = img->width;
            h = img->height;
            pw = img->pw;
            ph = img->ph;
        }
        if (key.depth_format) {
            Image* d = RenderTarget(mem, st.depth_tex, true);
            if (!d) return false;
            views.push_back(*d->view);
            if (w == 0) {
                w = d->width;
                h = d->height;
                pw = d->pw;
                ph = d->ph;
            }
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
    __declspec(noinline) vk::Pipeline CreatePipelineObject(const PipelineKey& key, const Program& prog,
                                                       std::vector<VkVertexInputAttributeDescription> attributes, VkRenderPass rp) {
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
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = key.rt_count;
        cb.pAttachments = ba.data();
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                      VK_DYNAMIC_STATE_DEPTH_BIAS};
        const VkPipelineDynamicStateCreateInfo dyn_ci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 4, dyn};
        PHASE("pipeline step", 0);
        const VkPipelineShaderStageCreateInfo stages[] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, *prog.vs, "main", nullptr},
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
    __declspec(noinline) void PrebuildForSubmit(Core::Memory::Memory& mem, const std::vector<std::unique_ptr<NR::List>>& lists) {
        PHASE("prescan", 0);
        const u64 t_scan = ProfNow();
        auto s = std::make_unique<State>(st); // heap: the guest fiber stack is small
        std::vector<std::unique_ptr<ProgramJob>> pjobs;
        std::vector<std::unique_ptr<PipelineJob>> qjobs;
        std::unordered_set<ShaderKey, ShaderKeyHash> want_prog;
        std::unordered_set<PipelineKey, PipelineKeyHash> want_pipe;
        u64 warm_ticks = 0;
        for (const auto& list : lists) {
            for (const auto& rec : list->recs) {
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
        shader_out.write(reinterpret_cast<const char*>(&type), 4);
        shader_out.write(reinterpret_cast<const char*>(&size), 4);
        shader_out.write(reinterpret_cast<const char*>(&sum), 8);
        shader_out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
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
        const bool exists = std::filesystem::exists(path, ec);
        if (exists) {
            std::ifstream f(path, std::ios::binary);
            if (!f) Fatal("shader cache: cannot open " + path.string());
            std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (f.bad()) Fatal("shader cache: cannot read " + path.string());
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
            while (pos < data.size()) {
                if (data.size() - pos < 16) corrupt(fmt::format("truncated record header at {}", pos));
                u32 type = 0, size = 0;
                u64 sum = 0;
                std::memcpy(&type, data.data() + pos, 4);
                std::memcpy(&size, data.data() + pos + 4, 4);
                std::memcpy(&sum, data.data() + pos + 8, 8);
                pos += 16;
                if (data.size() - pos < size) corrupt(fmt::format("truncated record at {}", pos));
                u64 check = 1469598103934665603ull;
                Fnv(check, data.data() + pos, size);
                if (check != sum) corrupt(fmt::format("checksum mismatch at {}", pos));
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
    void SavePipelineCache() {
        size_t size = 0;
        if (const VkResult r = pipeline_cache.Read(&size, nullptr); r != VK_SUCCESS) {
            Fatal(fmt::format("pipeline cache: size query failed ({})", static_cast<int>(r)));
        }
        if (size <= cache_saved_size) return;
        std::vector<u8> data(size);
        if (const VkResult r = pipeline_cache.Read(&size, data.data()); r != VK_SUCCESS) {
            Fatal(fmt::format("pipeline cache: read failed ({})", static_cast<int>(r)));
        }
        data.resize(size);
        const auto path = PipelineCachePath();
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            f.close();
            if (!f) Fatal("pipeline cache: cannot write " + tmp.string());
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) Fatal("pipeline cache: cannot replace " + path.string() + ": " + ec.message());
        cache_saved_size = size;
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
        if (opt_prescan && workers) PrebuildForSubmit(mem, lists);
        PHASE("begin frame", stats.submits);
        BeginFrame();
        // The GPU keeps its bindings from one submit to the next, and the game relies on it
        // (textures and uniform buffers it binds once are never rebound), so `st` is not reset here.
        for (auto& list : lists) {
            ++stats.lists;
            for (const auto& rec : list->recs) {
                ++stats.recs;
                Execute(mem, rec);
            }
        }
        PHASE("end frame", stats.submits);
        EndFrame();
        if (Core::GameSettings::FenceHold()) {
            // [Debug] fence_hold: the guest driver's own submit (and so its fence) happens after this hook returns;
            // hold it until our GPU work for this submit has finished.
            const double h0 = NvnVkNowMs();
            {
                ProfScope ps{prof, PB_WaitCapture};
                frames[cur].fence.Wait();
            }
            frames[cur].pending = false;
            PpLine(fmt::format("H{} t{:.1f} fence_hold waited {:.1f} ms", stats.submits, h0, NvnVkNowMs() - h0));
        }
        if (query_on && qn > 0) {
            {
                ProfScope ps{prof, PB_WaitCapture};
                frames[cur].fence.Wait();
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
                frames[cur].fence.Wait();
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
        if (Image* t = RenderTarget(mem, st.rt_tex[r.r[1]], false)) ++t->clears;
        std::memcpy(ca.clearValue.color.float32, r.snap, 16);
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
                    td_scaled |= b.img && b.img->pw != 0;
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
        PHASE("begin pass", st.rt_tex[0]);
        if (!BeginPassFor(mem, key)) {
            ++stats.notes["draw skipped: render pass unavailable"];
            return;
        }
        PHASE("record draw", st.prog[0]);
        if (Kernel::StallRing::On()) { // [Debug] probes: which viewport/scissor/pass sizes the game draws with (counted per combination)
            ++stats.notes[fmt::format("draw vp {} [{:.0f} {:.0f} {:.0f} {:.0f}] sc {} [{} {} {} {}] pass {}x{}", st.viewport_set, st.viewport[0], st.viewport[1],
                                      st.viewport[2], st.viewport[3], st.scissor_set, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3], pass_w, pass_h)];
        }
        if (stats.submits >= 600 && stats.submits < 603 && draw_logged < 60) {
            ++draw_logged;
            ++stats.notes[fmt::format("  draw[{}] topo {} count {} idx {} vp {} [{:.0f} {:.0f} {:.0f} {:.0f}] sc {} [{} {} {} {}] pass {}x{} rt {} depth {:x} ds {:x} poly {:x} color {:x} chan {:x} blend {:x} bl {} {} {} {} prog {:x} {:x}",
                                      draw_logged, topology, r.r[3], indexed, st.viewport_set, st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3],
                                      st.scissor_set, st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3], pass_w, pass_h, st.rt_count, st.depth_tex,
                                      st.ds_flags, st.poly_flags, st.color_state, st.chan_mask, key.blend_enable, key.blend[0][0], key.blend[0][1], key.blend[0][2], key.blend[0][3], st.prog[0], st.prog[1])];
        }
        vkcmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pl->pipeline);
        // viewport / scissor
        const f32 vx = st.viewport_set ? st.viewport[0] : 0, vy = st.viewport_set ? st.viewport[1] : 0;
        const f32 vw = st.viewport_set ? st.viewport[2] : static_cast<f32>(pass_w);
        const f32 vh = st.viewport_set ? st.viewport[3] : static_cast<f32>(pass_h);
        const float sx = pass_w ? static_cast<float>(pass_pw) / static_cast<float>(pass_w) : 1.0f;
        const float sy = pass_h ? static_cast<float>(pass_ph) / static_cast<float>(pass_h) : 1.0f;
        VkViewport vp{vx * sx, (vy + vh) * sy, vw * sx, -vh * sy, 0.0f, 1.0f};
        if (no_flip_y) vp = {vx * sx, vy * sy, vw * sx, vh * sy, 0.0f, 1.0f};
        vkcmd.SetViewport(0, vp);
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
        if (indexed) {
            vkcmd.BindIndexBuffer(arena, index_at, index_type);
            vkcmd.DrawIndexed(index_count, 1, 0, vb_ranged ? -static_cast<s32>(vtx_shift) : base_vertex, 0);
        } else {
            vkcmd.Draw(r.r[3], 1, vb_ranged ? 0 : r.r[2], 0);
        }
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
        queue.Submit(si, *util_fence);
        {
            ProfScope ps{prof, PB_WaitDump};
            util_fence.Wait();
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
                f.fence.Wait();
                f.pending = false;
            }
        }
    }

    bool util_pending{};
    int pres_prev{-1};
    /// A util submit may still be in flight (present_nodrain): finish it before its fence or command buffer is reused.
    void SettleUtil() {
        if (util_pending) {
            ProfScope ps{prof, PB_WaitUtil};
            util_fence.Wait();
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
        queue.Submit(si, *util_fence);
        util_fence.Wait();
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
#ifdef _WIN32
        f8 = (GetAsyncKeyState(0x77) & 0x8000) != 0;
#endif
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
            const u32 n = std::max<u32>(static_cast<u32>(Core::GameSettings::PresRing()), Core::GameSettings::PresentFifo() ? 6u : 0u);
            pres_img.resize(n);
            pres_seq.assign(n, 0);
            pres_done_ms.assign(n, 0.0);
        }
        const bool first = !pres_img[0];
        if (first) {
            pres_w = src.pw ? src.pw : src.width;
            pres_h = src.ph ? src.ph : src.height;
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
        const u32 idx = pres_next;
        pres_next = (pres_next + 1) % static_cast<u32>(pres_img.size());
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
        queue.Submit(si, *util_fence);
        const double pp_t2 = NvnVkNowMs();
        if (nodrain) {
            // Publish the previous frame's copy (its fence was settled above): suyu never reads an unfinished image.
            util_pending = true;
            const u64 seq = ++pres_ctr;
            pres_seq[idx] = seq;
            pres_done_ms[idx] = pp_t2;
            if (pres_prev >= 0) PublishCopy(pres_prev);
            PresentOrderLine(seq, idx, pp_t2, pp_t2 - pp_t0);
            pres_prev = static_cast<int>(idx);
            return;
        }
        {
            ProfScope ps{prof, PB_WaitUtil};
            util_fence.Wait();
        }
        const double pp_t3 = NvnVkNowMs();
        const u64 seq = ++pres_ctr;
        pres_seq[idx] = seq;
        pres_done_ms[idx] = pp_t3;
        PublishCopy(static_cast<int>(idx));
        PresentOrderLine(seq, idx, pp_t3, pp_t3 - pp_t0);
        {
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
        const int i = NextCopy();
        if (i < 0) return false;
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
        queue.Submit(si, *util_fence);
        {
            ProfScope ps{prof, PB_WaitDump};
            util_fence.Wait();
        }
        buf.Invalidate();
        WritePng(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name, img.width, img.height, buf.Mapped().data());
        Note(fmt::format("dumped {}", name));
    }

    void WriteStats() {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        const std::string memory_line = fmt::format("process private {} MB, peak pagefile {} MB; system commit free {} MB, address space free {} MB\n",
                                                    pmc.PrivateUsage >> 20, pmc.PeakPagefileUsage >> 20, ms.ullAvailPageFile >> 20, ms.ullAvailVirtual >> 20);
        std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_vk.txt", std::ios::trunc);
        out << memory_line;
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
        for (const auto& [k, v] : stats.notes) out << fmt::format("  {}: {}\n", k, v);
        Kernel::StallRing::TimedGuard lk{note_lock, Kernel::StallRing::LockNote};
        for (const auto& l : log_lines) out << l << "\n";
    }

    // ---- members ---------------------------------------------------------------------------------------------
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
};

std::unique_ptr<Renderer> g_renderer;

#ifdef _WIN32
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == 0xc0000005) {
        const auto base = reinterpret_cast<u64>(GetModuleHandleA(nullptr));
        std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_vk_crash.txt", std::ios::app);
        out << fmt::format("access violation at rva {:x}, address {:x}, write {}; phase {} detail {:x}\n",
                           reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress) - base,
                           info->ExceptionRecord->ExceptionInformation[1], info->ExceptionRecord->ExceptionInformation[0],
                           g_phase, static_cast<u64>(g_detail), static_cast<int>(g_line));
        // Stack, with names when dbghelp finds the build's symbols next to the build.
        using SymInit = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
        using SymFrom = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
        const HMODULE dbg = LoadLibraryA("dbghelp.dll");
        SymInit init = dbg ? reinterpret_cast<SymInit>(GetProcAddress(dbg, "SymInitialize")) : nullptr;
        SymFrom from = dbg ? reinterpret_cast<SymFrom>(GetProcAddress(dbg, "SymFromAddr")) : nullptr;
        const HANDLE proc = GetCurrentProcess();
        const bool have = init && from && init(proc, "E:/GUU test/suyu-a32recomp-src/build/bin", TRUE);
        void* frames[32];
        const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        for (USHORT i = 0; i < n; ++i) {
            std::string name = "?";
            if (have) {
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                std::memset(sym, 0, sizeof(SYMBOL_INFO));
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 250;
                DWORD64 disp = 0;
                if (from(proc, reinterpret_cast<DWORD64>(frames[i]), &disp, sym)) name = sym->Name;
            }
            out << fmt::format("  #{} rva {:x} {}\n", i, reinterpret_cast<u64>(frames[i]) - base, name);
        }
    }
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
#ifdef _WIN32
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int vkey);
#endif
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
#ifdef _WIN32
    // F9 (global key state, works with the game window focused) takes the capture.
    static bool f9_was = false;
    const bool f9 = (GetAsyncKeyState(0x78) & 0x8000) != 0;
    if (f9 && !f9_was) armed = true;
    f9_was = f9;
#endif
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

void NvnVkDumpLine(const char* file, const std::string& text) {
    static std::mutex m;
    std::scoped_lock lk{m};
    std::ofstream f(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / file, std::ios::app);
    f << text << "\n";
}

double NvnVkNowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool NvnVkPresentImage(VkImage& image, unsigned& width, unsigned& height) {
    return g_renderer && g_renderer->enabled && g_renderer->PresentedImage(image, width, height);
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
}

} // namespace Vulkan
