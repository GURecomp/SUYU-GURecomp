// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "core/hle/kernel/stall_ring.h"
#include "core/arm/recomp/game_settings.h"
#include "core/arm/recomp/nvn_rec.h"
#include "core/arm/recomp/nvn_shadow.h"
#include "core/arm/recomp/nvn_trace.h"
#include "video_core/renderer_vulkan/nvn_vk.h"
#include "core/memory.h"

namespace Core::NvnRec {

namespace {

// ---- what the game says about its memory pools (for GPU address -> guest memory) -------------
struct Pool {
    u32 cpu{}; // guest address of the memory, from the builder's storage call
    u32 size{};
};
std::mutex s_lock;
std::unordered_map<u32, std::pair<u32, u32>> s_builders; // pool builder -> (memory, size)
std::unordered_map<u32, Pool> s_pools;                   // pool object -> memory
std::unordered_map<u32, std::array<u64, 2>> s_programs;  // program object -> code addresses
std::unordered_map<u32, std::unique_ptr<List>> s_lists;  // command buffer -> its recording
std::unordered_map<u32, u32> s_handle_cmd;               // candidate command handle -> command buffer
Sink s_sink{nullptr};
PresentSink s_present_sink{nullptr};
struct SimplePool {
    u32 mempool{}, offset{}, count{};
};
std::unordered_map<u32, SimplePool> s_texpools, s_samplerpools; // pool object -> where its descriptors live
std::vector<u32> s_window_textures;
std::vector<std::pair<u32, u32>> s_dirty; // guest memory ranges the game wrote through NVN texture calls

u64 Read64(Memory::Memory& mem, u32 a) {
    return mem.Read32(a) | (u64{mem.Read32(a + 4)} << 32);
}

// Probe output: the first calls of each entry point, raw.
struct Sample {
    u32 r[4], sp[4], s[4];
    bool has[3]{};
    u32 d[3][24]{}; // words at what r1..r3 point to, when that is guest memory
};
std::map<std::string, std::vector<Sample>> s_samples;
std::map<std::string, u64> s_counts;
std::map<std::string, u32> s_submit_matches;
std::vector<std::string> s_submit_log;

const u32* Vfp(const u32* regs) {
    return regs + 24; // A32Context: r[16] n z c v q ge thumb fpscr (byte 92), then s[0..] at byte 96
}

u32 StackWord(Memory::Memory& mem, const u32* r, u32 k) {
    return mem.IsValidVirtualAddressRange(r[13] + 4 * k, 4) ? mem.Read32(r[13] + 4 * k) : 0;
}

Rec Capture(Op op, const u32* r, Memory::Memory& mem) {
    Rec rec{};
    rec.op = op;
    for (u32 k = 0; k < 4; ++k) rec.r[k] = r[k];
    if (mem.IsValidVirtualAddressRange(r[13], 16)) {
        for (u32 k = 0; k < 4; ++k) rec.sp[k] = mem.Read32(r[13] + 4 * k);
    } else {
        for (u32 k = 0; k < 4; ++k) rec.sp[k] = StackWord(mem, r, k);
    }
    for (u32 k = 0; k < 4; ++k) rec.s[k] = Vfp(r)[k];
    return rec;
}

void Snapshot(Rec& rec, Memory::Memory& mem, u32 addr, u32 words) {
    if (addr == 0 || words == 0 || words > 16 || !mem.IsValidVirtualAddressRange(addr, words * 4)) return;
    rec.snap_words = static_cast<u16>(words);
    for (u32 k = 0; k < words; ++k) rec.snap[k] = mem.Read32(addr + 4 * k);
}

List* ListFor(u32 cmd) {
    thread_local u32 cached_cmd = 0;
    thread_local List* cached = nullptr;
    if (cached && cached_cmd == cmd) return cached;
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    auto& slot = s_lists[cmd];
    if (!slot) {
        slot = std::make_unique<List>();
        slot->cmd = cmd;
    }
    cached = slot.get();
    cached_cmd = cmd;
    return cached;
}

struct Spec {
    const char* name;
    Op op;
    u8 ptr;   // which argument points at a state object: 1..3 = r1..r3, 0 none
    u8 words; // words to snapshot from it
};

const Spec kSpecs[] = {
    {"nvnCommandBufferBindUniformBuffer", Op::BindUniform, 0, 0},
    {"nvnCommandBufferBindTexture", Op::BindTexture, 0, 0},
    {"nvnCommandBufferBindVertexBuffer", Op::BindVertexBuffer, 0, 0},
    {"nvnCommandBufferBindColorState", Op::BindColor, 1, 2},
    {"nvnCommandBufferDrawElementsBaseVertex", Op::DrawElements, 0, 0},
    {"nvnCommandBufferBindVertexAttribState", Op::BindVertexAttrib, 2, 16},
    {"nvnCommandBufferBindVertexStreamState", Op::BindVertexStream, 2, 16},
    {"nvnCommandBufferBindBlendState", Op::BindBlend, 1, 2},
    {"nvnCommandBufferDrawArrays", Op::DrawArrays, 0, 0},
    {"nvnCommandBufferBindPolygonState", Op::BindPolygon, 1, 2},
    {"nvnCommandBufferSetDepthClamp", Op::SetDepthClamp, 0, 0},
    {"nvnCommandBufferSetPolygonOffsetClamp", Op::SetPolygonOffsetClamp, 0, 0},
    {"nvnCommandBufferSetScissor", Op::SetScissor, 0, 0},
    {"nvnCommandBufferSetScissors", Op::SetScissors, 3, 4},
    {"nvnCommandBufferSetViewport", Op::SetViewport, 0, 0},
    {"nvnCommandBufferSetViewports", Op::SetViewports, 3, 4},
    {"nvnCommandBufferBarrier", Op::Barrier, 0, 0},
    {"nvnCommandBufferBindChannelMaskState", Op::BindChannelMask, 1, 1},
    {"nvnCommandBufferSetBlendColor", Op::SetBlendColor, 1, 4},
    {"nvnCommandBufferBindDepthStencilState", Op::BindDepthStencil, 1, 8},
    {"nvnCommandBufferSetStencilMask", Op::SetStencilMask, 0, 0},
    {"nvnCommandBufferSetStencilRef", Op::SetStencilRef, 0, 0},
    {"nvnCommandBufferSetStencilValueMask", Op::SetStencilValueMask, 0, 0},
    {"nvnCommandBufferClearColor", Op::ClearColor, 2, 4},
    {"nvnCommandBufferSetAlphaRef", Op::SetAlphaRef, 0, 0},
    {"nvnCommandBufferClearDepthStencil", Op::ClearDepthStencil, 0, 0},
    {"nvnCommandBufferBindMultisampleState", Op::BindMultisample, 1, 8},
    {"nvnCommandBufferSetPrimitiveRestart", Op::SetPrimitiveRestart, 0, 0},
    {"nvnCommandBufferSetLineWidth", Op::SetLineWidth, 0, 0},
    {"nvnCommandBufferSetPointSize", Op::SetPointSize, 0, 0},
    {"nvnCommandBufferSetTexturePool", Op::SetTexturePool, 0, 0},
    {"nvnCommandBufferSetSamplerPool", Op::SetSamplerPool, 0, 0},
};
constexpr size_t kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

// Entries that are only counted and sampled (arguments written to nvn_rec.txt for reading).
const char* const kCountOnly[] = {
    "nvnCommandBufferReportCounter",
    "nvnCommandBufferSetShaderScratchMemory",
    "nvnCommandBufferAddControlMemory",
    "nvnCommandBufferAddCommandMemory",
    "nvnCommandBufferGetCommandMemoryUsed",
    "nvnQueueFenceSync",
    "nvnQueueFlush",
    "nvnQueueWaitSync",
    "nvnWindowAcquireTexture",
    "nvnWindowSetCrop",
    "nvnWindowInitialize",
    "nvnTexturePoolRegisterTexture",
    "nvnSamplerPoolRegisterSampler",
    "nvnMemoryPoolFlushMappedRange",
    "nvnSamplerBuilderSetMinMagFilter",
    "nvnSamplerBuilderSetWrapMode",
    "nvnSamplerBuilderSetBorderColor",
    "nvnSamplerBuilderSetLodBias",
    "nvnSamplerBuilderSetLodClamp",
    "nvnSamplerBuilderSetMaxAnisotropy",
    "nvnSamplerBuilderSetCompare",
    "nvnSamplerInitialize",
    "nvnTextureViewSetDefaults",
    "nvnTextureViewSetLayers",
    "nvnTextureViewSetLevels",
    "nvnDeviceGetTextureHandle",
    "nvnQueueInitialize",
};
constexpr size_t kCountOnlyCount = sizeof(kCountOnly) / sizeof(kCountOnly[0]);

void Sampled(const char* name, const u32* r, Memory::Memory& mem) {
    // Probe bookkeeping only (call counts + the first raw samples, dumped at exit). It cost a global lock and two
    // string-keyed map lookups on every recorded NVN call, on the guest thread: tens of thousands per second.
    static const bool diag = Core::GameSettings::GpuDiagnosticsEnabled();
    if (!diag) return;
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    ++s_counts[name];
    auto& v = s_samples[name];
    if (v.size() < 24) {
        Sample s{};
        for (u32 k = 0; k < 4; ++k) s.r[k] = r[k];
        for (u32 k = 0; k < 4; ++k) s.sp[k] = StackWord(mem, r, k);
        for (u32 k = 0; k < 4; ++k) s.s[k] = Vfp(r)[k];
        for (u32 a = 0; a < 3; ++a) {
            if (r[1 + a] >= 0x10000 && mem.IsValidVirtualAddressRange(r[1 + a], 96)) {
                s.has[a] = true;
                for (u32 k = 0; k < 24; ++k) s.d[a][k] = mem.Read32(r[1 + a] + 4 * k);
            }
        }
        v.push_back(s);
    }
}

void RecordSpec(const Spec& sp, u32* r, Memory::Memory& mem) {
    if (!mem.IsValidVirtualAddressRange(r[0], 24)) return;
    Rec rec = Capture(sp.op, r, mem);
    if (sp.ptr) Snapshot(rec, mem, r[sp.ptr], sp.words);
    switch (sp.op) {
    case Op::BindColor:
    case Op::BindVertexAttrib:
    case Op::BindVertexStream:
    case Op::BindBlend:
    case Op::BindPolygon:
    case Op::BindDepthStencil:
    case Op::BindChannelMask:
    case Op::SetDepthClamp:
        static const std::array<NvnShadow::NativeBuilder, kSpecCount> builders = [] {
            std::array<NvnShadow::NativeBuilder, kSpecCount> b{};
            for (size_t i = 0; i < kSpecCount; ++i) b[i] = NvnShadow::FindBuilder(kSpecs[i].name);
            return b;
        }();
        if (const auto build = builders[&sp - kSpecs]) {
            NvnShadow::NativeWords w;
            if (build(r, mem, w)) {
                rec.nwords = w.n;
                for (u32 k = 0; k < w.n && k < 24; ++k) rec.words[k] = w.v[k];
            }
        }
        break;
    default:
        break;
    }
    ListFor(r[0])->recs.push_back(rec);
    Sampled(sp.name, r, mem);
}

// nvnCommandBufferBindProgram(cmd, program, stages)
void RecordBindProgram(u32* r, Memory::Memory& mem) {
    if (!mem.IsValidVirtualAddressRange(r[0], 24)) return;
    Rec rec = Capture(Op::BindProgram, r, mem);
    if (mem.IsValidVirtualAddressRange(r[1], 48)) {
        for (u32 k = 0; k < 12; ++k) rec.pobj[k] = mem.Read32(r[1] + 4 * k);
        const u32 blk = rec.pobj[2], n = std::min<u32>(rec.pobj[4], 40);
        if (n && mem.IsValidVirtualAddressRange(blk, n * 4)) {
            for (u32 k = 0; k < n; ++k) rec.pblock[k] = mem.Read32(blk + 4 * k);
            rec.pblock_n = n;
        }
    }
    {
        Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
        if (const auto p = s_programs.find(r[1]); p != s_programs.end()) {
            rec.program[0] = p->second[0];
            rec.program[1] = p->second[1];
        }
    }
    ListFor(r[0])->recs.push_back(rec);
    Sampled("nvnCommandBufferBindProgram", r, mem);
}

// nvnCommandBufferSetRenderTargets(cmd, count, colors**, views**, depth [sp], depth view [sp+4]):
// snapshot = color texture pointers (up to 4), then the depth texture pointer.
void RecordRenderTargets(u32* r, Memory::Memory& mem) {
    if (!mem.IsValidVirtualAddressRange(r[0], 24)) return;
    Rec rec = Capture(Op::SetRenderTargets, r, mem);
    const u32 count = std::min<u32>(r[1], 4);
    if (count && mem.IsValidVirtualAddressRange(r[2], count * 4)) {
        for (u32 k = 0; k < count; ++k) rec.snap[k] = mem.Read32(r[2] + 4 * k);
    }
    rec.snap[4] = rec.sp[0];
    rec.snap_words = 5;
    ListFor(r[0])->recs.push_back(rec);
    Sampled("nvnCommandBufferSetRenderTargets", r, mem);
}

void RecordBegin(u32* r, Memory::Memory& mem) {
    if (!mem.IsValidVirtualAddressRange(r[0], 24)) return;
    ListFor(r[0])->recs.clear();
    Sampled("nvnCommandBufferBeginRecording", r, mem);
}

void RecordEnd(u32* r, Memory::Memory& mem) {
    if (!mem.IsValidVirtualAddressRange(r[0], 24)) return;
    // The handle the guest returns is not visible from here: register the candidates a submit
    // might present (command buffer object, chunk start, write pointer).
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    // Observed: the handle is the command buffer's control memory chunk + 0x10 (object word 11).
    for (u32 k = 0; k < 24; ++k) {
        const u32 w = mem.Read32(r[0] + 4 * k);
        if (w >= 0x10000) {
            s_handle_cmd[w] = r[0];
            s_handle_cmd[w + 0x10] = r[0];
        }
    }
    ++s_counts["nvnCommandBufferEndRecording"];
}

// nvnQueueSubmitCommands(queue, count, const CommandHandle* handles)
void RecordSubmit(u32* r, Memory::Memory& mem) {
    std::vector<std::unique_ptr<List>> out;
    Sink sink;
    {
        Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
        sink = s_sink;
        ++s_counts["nvnQueueSubmitCommands"];
        const u32 count = std::min<u32>(r[1], 64);
        for (u32 k = 0; k < count && mem.IsValidVirtualAddressRange(r[2] + 8 * k, 8); ++k) {
            const u64 h = Read64(mem, r[2] + 8 * k);
            const auto it = s_handle_cmd.find(static_cast<u32>(h));
            if (s_submit_log.size() < 40) {
                s_submit_log.push_back(fmt::format(
                    "submit handle {:x} -> {}", h,
                    it == s_handle_cmd.end()
                        ? std::string("no match")
                        : fmt::format("cmd {:x} ({} recs)", it->second,
                                      s_lists[it->second] ? s_lists[it->second]->recs.size() : 0)));
            }
            if (it == s_handle_cmd.end()) continue;
            ++s_submit_matches["match"];
            auto& l = s_lists[it->second];
            if (l && sink) {
                auto copy = std::make_unique<List>();
                copy->cmd = l->cmd;
                copy->recs = l->recs;
                out.push_back(std::move(copy));
            }
        }
    }
    if (sink && !out.empty()) {
        sink(mem, std::move(out), r[0]);
    }
}

// nvnMemoryPoolBuilderSetStorage(builder, memory, size)
void RecordPoolBuilderStorage(u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    s_builders[r[0]] = {r[1], r[2]};
    ++s_counts["nvnMemoryPoolBuilderSetStorage"];
}
// nvnMemoryPoolInitialize(pool, builder)
void RecordPoolInit(u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    const auto b = s_builders.find(r[1]);
    if (b != s_builders.end()) s_pools[r[0]] = {b->second.first, b->second.second};
    ++s_counts["nvnMemoryPoolInitialize"];
}
// nvnProgramSetShaders(program, count, stages)
void RecordSetShaders(u32* r, Memory::Memory& mem) {
    if (r[2] == 0 || r[1] == 0 || !mem.IsValidVirtualAddressRange(r[2], 16 * std::min<u32>(r[1], 2))) return;
    std::array<u64, 2> p{};
    for (u32 s = 0; s < std::min<u32>(r[1], 2); ++s) p[s] = Read64(mem, r[2] + 16 * s);
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    s_programs[r[0]] = p;
    ++s_counts["nvnProgramSetShaders"];
}

// nvnTexturePoolInitialize(pool, memory pool, offset, count) / nvnSamplerPoolInitialize likewise
void RecordTexPoolInit(u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    s_texpools[r[0]] = {r[1], r[2], r[3]};
    ++s_counts["nvnTexturePoolInitialize"];
}
void RecordSamplerPoolInit(u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    s_samplerpools[r[0]] = {r[1], r[2], r[3]};
    ++s_counts["nvnSamplerPoolInitialize"];
}
// nvnWindowBuilderSetTextures(builder, count, textures**)
void RecordWindowTextures(u32* r, Memory::Memory& mem) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    s_window_textures.clear();
    for (u32 k = 0; k < std::min<u32>(r[1], 8) && mem.IsValidVirtualAddressRange(r[2] + 4 * k, 4); ++k) {
        s_window_textures.push_back(mem.Read32(r[2] + 4 * k));
    }
    ++s_counts["nvnWindowBuilderSetTextures"];
}
// nvnQueuePresentTexture(queue, window, index)
void RecordPresent(u32* r, Memory::Memory& mem) {
    PresentSink sink;
    u32 tex = 0;
    if (Kernel::StallRing::g_present_probe) {
        Kernel::StallRing::g_present_probe(r[14], Kernel::StallRing::t_gtid); // Run M frame line
    }
    {
        Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
        ++s_counts["nvnQueuePresentTexture"];
        sink = s_present_sink;
        if (r[2] < s_window_textures.size()) tex = s_window_textures[r[2]];
    }
    if (sink && tex) sink(mem, tex);
}
// nvnTextureWriteTexels(texture, ...) / nvnTextureFlushTexels(texture, ...): texture object word 3 =
// pool, 4 = offset in it, 20 = byte size
void RecordTexelWrite(u32* r, Memory::Memory& mem) {
    if (!mem.IsValidVirtualAddressRange(r[0], 96)) return;
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    const auto pool = s_pools.find(mem.Read32(r[0] + 12));
    if (pool != s_pools.end()) {
        s_dirty.emplace_back(pool->second.cpu + mem.Read32(r[0] + 16), mem.Read32(r[0] + 80));
    }
    ++s_counts["nvnTextureWriteTexels"];
}

using RecFn = void (*)(u32*, Memory::Memory&);
struct Named {
    const char* name;
    RecFn fn;
};

template <size_t I>
void SpecFn(u32* r, Memory::Memory& mem) {
    RecordSpec(kSpecs[I], r, mem);
}
template <size_t... Is>
constexpr std::array<RecFn, sizeof...(Is)> SpecFns(std::index_sequence<Is...>) {
    return {SpecFn<Is>...};
}
const auto kSpecFns = SpecFns(std::make_index_sequence<kSpecCount>{});

template <size_t I>
void CountFn(u32* r, Memory::Memory& mem) {
    Sampled(kCountOnly[I], r, mem);
}
template <size_t... Is>
constexpr std::array<RecFn, sizeof...(Is)> CountFns(std::index_sequence<Is...>) {
    return {CountFn<Is>...};
}
const auto kCountFns = CountFns(std::make_index_sequence<kCountOnlyCount>{});

const Named kSpecials[] = {
    {"nvnCommandBufferBindProgram", RecordBindProgram},
    {"nvnCommandBufferSetRenderTargets", RecordRenderTargets},
    {"nvnCommandBufferBeginRecording", RecordBegin},
    {"nvnCommandBufferEndRecording", RecordEnd},
    {"nvnQueueSubmitCommands", RecordSubmit},
    {"nvnMemoryPoolBuilderSetStorage", RecordPoolBuilderStorage},
    {"nvnMemoryPoolInitialize", RecordPoolInit},
    {"nvnProgramSetShaders", RecordSetShaders},
    {"nvnTexturePoolInitialize", RecordTexPoolInit},
    {"nvnSamplerPoolInitialize", RecordSamplerPoolInit},
    {"nvnWindowBuilderSetTextures", RecordWindowTextures},
    {"nvnQueuePresentTexture", RecordPresent},
    {"nvnTextureWriteTexels", RecordTexelWrite},
    {"nvnTextureWriteTexelsStrided", RecordTexelWrite},
    {"nvnTextureFlushTexels", RecordTexelWrite},
};
constexpr size_t kSpecialCount = sizeof(kSpecials) / sizeof(kSpecials[0]);

// The Replacement functions: record, then let the guest run.
template <int Kind, size_t I>
bool Replacement(u32* r, Memory::Memory& mem) {
    if constexpr (Kind == 0) {
        kSpecFns[I](r, mem);
    } else if constexpr (Kind == 1) {
        kCountFns[I](r, mem);
    } else {
        kSpecials[I].fn(r, mem);
    }
    return false;
}

template <int Kind, size_t... Is>
void RegisterKind(std::index_sequence<Is...>, const char* const* names) {
    (((NvnTrace::Get().HasReplacement(names[Is])) ? void()
                                                   : NvnTrace::Get().Replace(names[Is], Replacement<Kind, Is>)),
     ...);
}

} // namespace

void SetSink(Sink sink) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    s_sink = sink;
}
void SetPresentSink(PresentSink sink) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    s_present_sink = sink;
}

bool PoolMemory(u32 pool_obj, u32& cpu, u32& size) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    const auto it = s_pools.find(pool_obj);
    if (it == s_pools.end()) return false;
    cpu = it->second.cpu;
    size = it->second.size;
    return true;
}

static bool PoolEntry(const std::unordered_map<u32, SimplePool>& pools, u32 pool, u32 id, u32& cpu) {
    const auto it = pools.find(pool);
    if (it == pools.end() || id >= it->second.count) return false;
    const auto mp = s_pools.find(it->second.mempool);
    if (mp == s_pools.end()) return false;
    cpu = mp->second.cpu + it->second.offset + id * 32;
    return true;
}
bool TextureDescriptor(u32 pool, u32 id, u32& cpu) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    return PoolEntry(s_texpools, pool, id, cpu);
}
bool SamplerDescriptor(u32 pool, u32 id, u32& cpu) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    return PoolEntry(s_samplerpools, pool, id, cpu);
}
void TakeDirty(std::vector<std::pair<u32, u32>>& out) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    out.insert(out.end(), s_dirty.begin(), s_dirty.end());
    s_dirty.clear();
}

namespace {
// Vertex attribute format codes the native table does not know: learned from the words the guest
// driver itself wrote for the previous BindVertexAttribState call.
struct AttribPending {
    bool valid{};
    u32 cmd{}, wp{};
    u32 codes[16]{};
};
AttribPending s_attr_pending;
std::map<u32, u32> s_learned_attrib;
} // namespace

bool LearnedAttrib(u32 code, u32& out) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    const auto it = s_learned_attrib.find(code);
    if (it == s_learned_attrib.end()) return false;
    out = it->second;
    return true;
}

void OnCall(const char* name, u32* regs, Memory::Memory& mem) {
    if (Kernel::StallRing::On()) {
        // Probe for the Wiggle Hitch: where one guest thread sits idle for a long time between two NVN calls.
        thread_local std::chrono::steady_clock::time_point last{};
        thread_local const char* last_name = "";
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0) {
            const double ms = std::chrono::duration<double, std::milli>(now - last).count();
            if (ms > 20.0) {
                Vulkan::NvnVkDumpLine("nvn_gaps.txt", fmt::format("thread {:x} {:.1f} ms between {} and {}",
                                      std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff, ms, last_name, name));
            }
        }
        last = now;
        last_name = name;
    }
    if (Kernel::StallRing::On() && (!std::strcmp(name, "nvnCommandBufferSetViewport") || !std::strcmp(name, "nvnCommandBufferSetScissor"))) {
        // [Debug] probes: who sets the viewport/scissor size (logged once per caller and size)
        static std::mutex vm;
        static std::map<std::tuple<std::string, u32, u32, u32>, u32> vseen;
        const u32 base = Core::GameSettings::g_main_base.load();
        std::scoped_lock vlk{vm};
        if (++vseen[{name, regs[3], regs[14], regs[1]}] == 1) {
            Vulkan::NvnVkDumpLine("nvn_sizes.txt", fmt::format("{} x {} y {} w {} caller {:#x} = main+{:#x}", name, regs[1], regs[2], regs[3], regs[14], regs[14] - base));
        }
    }
    if (s_attr_pending.valid && regs[0] == s_attr_pending.cmd) {
        s_attr_pending.valid = false;
        if (mem.IsValidVirtualAddressRange(regs[0], 24)) {
            const u32 now = mem.Read32(regs[0] + 12);
            if (now >= s_attr_pending.wp + 68 && mem.IsValidVirtualAddressRange(s_attr_pending.wp, 68) &&
                mem.Read32(s_attr_pending.wp) == 0x20100458) {
                Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
                for (u32 i = 0; i < 16; ++i) {
                    if (s_attr_pending.codes[i] == 0) continue;
                    s_learned_attrib[s_attr_pending.codes[i] >> 20] = mem.Read32(s_attr_pending.wp + 4 + 4 * i) >> 20;
                }
            }
        }
    }
    if (!std::strcmp(name, "nvnCommandBufferBindVertexAttribState") && regs[1] == 16 &&
        mem.IsValidVirtualAddressRange(regs[0], 24) && mem.IsValidVirtualAddressRange(regs[2], 64)) {
        s_attr_pending.valid = true;
        s_attr_pending.cmd = regs[0];
        s_attr_pending.wp = mem.Read32(regs[0] + 12);
        for (u32 i = 0; i < 16; ++i) s_attr_pending.codes[i] = mem.Read32(regs[2] + 4 * i);
    }
    for (size_t i = 0; i < kSpecCount; ++i) {
        if (!std::strcmp(kSpecs[i].name, name)) {
            kSpecFns[i](regs, mem);
            return;
        }
    }
    for (size_t i = 0; i < kSpecialCount; ++i) {
        if (!std::strcmp(kSpecials[i].name, name)) {
            kSpecials[i].fn(regs, mem);
            return;
        }
    }
}

namespace {
// [Debug] probes: where texture sizes come from. Logs each (call, value, caller) once to nvn_sizes.txt
// for the sizes the game uses for the screen (main-module offset of the caller).
bool SizeProbe(u32* r, Memory::Memory&, const char* what) {
    static std::mutex m;
    static std::map<std::tuple<std::string, u32, u32>, u32> seen;
    const u32 v = r[1];
    if (v != 1920 && v != 1080 && v != 3440 && v != 1440 && v != 1280 && v != 720) return false;
    const u32 base = Core::GameSettings::g_main_base.load();
    std::scoped_lock lk{m};
    if (++seen[{what, v, r[14]}] == 1) {
        Vulkan::NvnVkDumpLine("nvn_sizes.txt", fmt::format("{} {} caller {:#x} = main+{:#x}", what, v, r[14], r[14] - base));
    }
    return false;
}
bool SizeProbeW(u32* r, Memory::Memory& mem) { return SizeProbe(r, mem, "TextureBuilderSetWidth"); }
bool SizeProbeH(u32* r, Memory::Memory& mem) { return SizeProbe(r, mem, "TextureBuilderSetHeight"); }
} // namespace

void RegisterAll() {
    if (Kernel::StallRing::On()) {
        NvnTrace::Get().Replace("nvnTextureBuilderSetWidth", SizeProbeW);
        NvnTrace::Get().Replace("nvnTextureBuilderSetHeight", SizeProbeH);
    }
    std::array<const char*, kSpecCount> a{};
    for (size_t i = 0; i < kSpecCount; ++i) a[i] = kSpecs[i].name;
    RegisterKind<0>(std::make_index_sequence<kSpecCount>{}, a.data());
    RegisterKind<1>(std::make_index_sequence<kCountOnlyCount>{}, kCountOnly);
    std::array<const char*, kSpecialCount> b{};
    for (size_t i = 0; i < kSpecialCount; ++i) b[i] = kSpecials[i].name;
    RegisterKind<2>(std::make_index_sequence<kSpecialCount>{}, b.data());
}

namespace {
bool PresentProbeOnly(u32* r, Memory::Memory&) {
    if (Kernel::StallRing::g_present_probe) {
        Kernel::StallRing::g_present_probe(r[14], Kernel::StallRing::t_gtid);
    }
    return false;
}
} // namespace

void RegisterPresentProbeOnly() {
    NvnTrace::Get().Replace("nvnQueuePresentTexture", PresentProbeOnly);
}

bool GpuToCpu(Memory::Memory& mem, u64 gpu_va, u32& cpu, u32& avail) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    for (const auto& [obj, pool] : s_pools) {
        if (!mem.IsValidVirtualAddressRange(obj, 80)) continue;
        for (const u32 off : {40u, 56u, 72u, 80u}) {
            const u64 base = Read64(mem, obj + off);
            if (base != 0 && gpu_va >= base && gpu_va < base + pool.size) {
                cpu = pool.cpu + static_cast<u32>(gpu_va - base);
                avail = pool.size - static_cast<u32>(gpu_va - base);
                return true;
            }
        }
    }
    // Observed: a pool can also be viewed at the address just behind its +56 range (render targets sampled later).
    for (const auto& [obj, pool] : s_pools) {
        if (!mem.IsValidVirtualAddressRange(obj, 80)) continue;
        const u64 base = Read64(mem, obj + 56) + pool.size;
        if (base != pool.size && gpu_va >= base && gpu_va < base + pool.size) {
            cpu = pool.cpu + static_cast<u32>(gpu_va - base);
            avail = pool.size - static_cast<u32>(gpu_va - base);
            return true;
        }
    }
    return false;
}

void LogPools(Memory::Memory& mem, std::vector<std::string>& out) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    for (const auto& [obj, pool] : s_pools) {
        u64 w[9]{};
        if (mem.IsValidVirtualAddressRange(obj, 100)) {
            for (u32 i = 0; i < 9; ++i) w[i] = Read64(mem, obj + 24 + i * 8);
        }
        out.push_back(fmt::format("  pool obj {:x} cpu {:x} size {:x} | +24 {:x} +32 {:x} +40 {:x} +48 {:x} +56 {:x} +64 {:x} +72 {:x} +80 {:x} +88 {:x}",
                                  obj, pool.cpu, pool.size, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]));
    }
}

void Flush(const std::filesystem::path& file) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockRec};
    std::ofstream out(file, std::ios::trunc);
    out << "pools " << s_pools.size() << " programs " << s_programs.size() << " lists " << s_lists.size() << "\n";
    for (const auto& l : s_submit_log) out << l << "\n";
    for (const auto& [name, n] : s_counts) {
        out << fmt::format("{} calls {}\n", name, n);
        const auto it = s_samples.find(name);
        if (it == s_samples.end()) continue;
        for (const auto& s : it->second) {
            out << fmt::format("   r {:x} {:x} {:x} {:x} | sp {:x} {:x} {:x} {:x} | s {:x} {:x} {:x} {:x}\n", s.r[0],
                               s.r[1], s.r[2], s.r[3], s.sp[0], s.sp[1], s.sp[2], s.sp[3], s.s[0], s.s[1], s.s[2],
                               s.s[3]);
            for (u32 a = 0; a < 3; ++a) {
                if (!s.has[a]) continue;
                out << fmt::format("      r{} ->", a + 1);
                for (u32 k = 0; k < 24; ++k) out << fmt::format(" {:x}", s.d[a][k]);
                out << "\n";
            }
        }
    }
    for (const auto& [obj, pool] : s_pools) {
        out << fmt::format("pool {:x}: memory {:x} size {:x}\n", obj, pool.cpu, pool.size);
    }
}

} // namespace Core::NvnRec
