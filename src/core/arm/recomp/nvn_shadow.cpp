// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/gpu_probe.h"
#include "core/arm/recomp/game_settings.h"
#include "core/hle/kernel/stall_ring.h"
#include "core/arm/recomp/nvn_rec.h"
#include "core/arm/recomp/nvn_shadow.h"
#include "core/arm/recomp/nvn_trace.h"
#include "core/memory.h"

namespace Core::NvnShadow {

namespace {

using Draw = Common::GpuProbe::Draw;
using DrawState = Common::GpuProbe::DrawState;

struct DrawHash {
    size_t operator()(const Draw& d) const {
        u64 h = 0xcbf29ce484222325ull;
        const auto mix = [&](u64 v) { h = (h ^ v) * 0x100000001b3ull; };
        mix(d.topology), mix(d.indexed), mix(d.count), mix(d.first), mix(static_cast<u32>(d.base_vertex));
        mix(d.index_format), mix(d.instances), mix(d.index_addr), mix(d.vs), mix(d.ps);
        return static_cast<size_t>(h);
    }
};
struct DrawEq {
    bool operator()(const Draw& a, const Draw& b) const {
        return a.topology == b.topology && a.indexed == b.indexed && a.count == b.count &&
               a.first == b.first && a.base_vertex == b.base_vertex && a.index_format == b.index_format &&
               a.instances == b.instances && a.index_addr == b.index_addr && a.vs == b.vs && a.ps == b.ps;
    }
};
using DrawCounts = std::unordered_map<Draw, u64, DrawHash, DrawEq>;

// Distinct draws kept per side; later new ones are only counted (index addresses make most
// draws distinct per mesh, a few thousand per area).
constexpr size_t kMaxDistinct = 400000;

struct Side {
    u64 total{};
    u64 dropped{};
    DrawCounts draws;
};

std::mutex s_lock;
Side s_nvn, s_gpu; // what the game asked for / what the GPU emulation executed

// Shadow model
struct Program {
    u64 stage[2]{};
};
// Program code addresses as NVN hands them out are 0x30 bytes before what the GPU's program
// registers point at (measured: G1 run, every program).
constexpr u64 kProgramHeader = 0x30;

// Texture properties from the builder calls (textures are only ever created from a builder).
struct TexInfo {
    u32 width{}, height{}, depth{1}, format{}, target{}, levels{1}, pool{}, offset{};
};
// The game-side state a draw uses (bound per command buffer). NVN stages: 0 vertex, 1 fragment.
struct NvnState {
    static constexpr u32 kStages = 2;
    std::array<u64, DrawState::kStreams> vb{};
    std::array<std::array<u64, DrawState::kSlots>, kStages> cb_addr{};
    std::array<std::array<u32, DrawState::kSlots>, kStages> cb_size{};
    std::array<f32, 4> viewport{};
    bool viewport_set{};
    u32 rt_count{};
    u32 rt0_tex{};
    u32 zeta_tex{};
};
struct CmdState {
    u32 program{};
    NvnState st;
};

std::unordered_map<u32, Program> s_programs; // program object -> code GPU addresses
std::unordered_map<u32, CmdState> s_cmds;    // command buffer -> bound state
std::unordered_map<u32, TexInfo> s_builders; // texture builder -> pending properties
std::unordered_map<u32, TexInfo> s_textures; // texture object -> properties
u64 s_unknown_program_draws{};

// Pairing: each NVN draw waits (keyed by the full draw, shader addresses normalized) until the
// GPU emulation executes the same draw; then their state is compared field by field.
constexpr size_t kMaxPending = 200000;
std::unordered_map<Draw, std::deque<NvnState>, DrawHash, DrawEq> s_pending;
size_t s_pending_count{};

struct Stats {
    u64 paired{}, gpu_unpaired{}, pending_dropped{};
    std::array<u64, DrawState::kStreams> vb_total{}, vb_equal{};
    // NVN (stage, slot) -> GPU (stage, slot) whose address matched, packed one byte each
    std::map<u32, u64> cb_map;
    std::map<u32, u64> cb_unmatched; // NVN (stage, slot) with no GPU slot at that address
    u64 cb_total{}, cb_size_equal{};
    u64 vp_total{}, vp_equal{}, vp_size_equal{};
    u64 rt_total{}, rt_count_equal{}, rt_tex_known{}, rt_size_equal{};
    std::map<std::pair<u32, u32>, u64> rt_formats;  // (NVN format, GPU format)
    std::map<u32, std::map<u64, u64>> rt_pool_base; // pool -> (GPU address - offset) -> draws
    u64 zeta_both{}, zeta_nvn_only{}, zeta_gpu_only{};
    std::vector<std::string> examples; // first mismatches, for reading
} s_stats;

void Example(std::string line) {
    if (s_stats.examples.size() < 24) s_stats.examples.push_back(std::move(line));
}

void Compare(const NvnState& n, const DrawState& g) {
    auto& st = s_stats;
    ++st.paired;
    for (u32 k = 0; k < DrawState::kStreams; ++k) {
        if (!n.vb[k] && !g.vb[k]) continue;
        ++st.vb_total[k];
        if (n.vb[k] == g.vb[k]) ++st.vb_equal[k];
        else Example(fmt::format("vertex buffer {}: NVN {:x} GPU {:x}", k, n.vb[k], g.vb[k]));
    }
    for (u32 s = 0; s < NvnState::kStages; ++s) {
        for (u32 k = 0; k < DrawState::kSlots; ++k) {
            const u64 a = n.cb_addr[s][k];
            if (!a) continue;
            ++st.cb_total;
            bool found = false;
            for (u32 gs = 0; gs < DrawState::kStages && !found; ++gs) {
                for (u32 gk = 0; gk < DrawState::kSlots; ++gk) {
                    if (g.cb_addr[gs][gk] != a) continue;
                    ++st.cb_map[(s << 24) | (k << 16) | (gs << 8) | gk];
                    if (g.cb_size[gs][gk] == n.cb_size[s][k]) ++st.cb_size_equal;
                    found = true;
                    break;
                }
            }
            if (!found) ++st.cb_unmatched[(s << 8) | k];
        }
    }
    if (n.viewport_set) {
        ++st.vp_total;
        if (n.viewport == g.viewport) {
            ++st.vp_equal;
        } else if (st.vp_total - st.vp_equal < 4) {
            Example(fmt::format("viewport: NVN {} {} {} {} GPU {} {} {} {}", n.viewport[0], n.viewport[1],
                                n.viewport[2], n.viewport[3], g.viewport[0], g.viewport[1], g.viewport[2],
                                g.viewport[3]));
        }
        if (n.viewport[2] == g.viewport[2] && std::abs(n.viewport[3]) == std::abs(g.viewport[3])) ++st.vp_size_equal;
    }
    ++st.rt_total;
    if (n.rt_count == g.rt_count) ++st.rt_count_equal;
    if (n.rt_count && n.rt0_tex) {
        if (const auto t = s_textures.find(n.rt0_tex); t != s_textures.end()) {
            ++st.rt_tex_known;
            if (t->second.width == g.rt0_width && t->second.height == g.rt0_height) ++st.rt_size_equal;
            ++st.rt_formats[{t->second.format, g.rt0_format}];
            ++st.rt_pool_base[t->second.pool][g.rt0 - t->second.offset];
        }
    }
    const bool nz = n.zeta_tex != 0, gz = g.zeta != 0;
    if (nz && gz) ++st.zeta_both;
    else if (nz) ++st.zeta_nvn_only;
    else if (gz) ++st.zeta_gpu_only;
}

void Record(Side& side, const Draw& d) {
    ++side.total;
    if (const auto it = side.draws.find(d); it != side.draws.end()) {
        ++it->second;
    } else if (side.draws.size() < kMaxDistinct) {
        side.draws.emplace(d, 1);
    } else {
        ++side.dropped;
    }
}

void GpuDraw(const Draw& d, const DrawState& g) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    Record(s_gpu, d);
    const auto it = s_pending.find(d);
    if (it == s_pending.end() || it->second.empty()) {
        ++s_stats.gpu_unpaired;
        return;
    }
    Compare(it->second.front(), g);
    it->second.pop_front();
    --s_pending_count;
    if (it->second.empty()) s_pending.erase(it);
}

// nvnProgramSetShaders(program, count, stages): 16-byte stage entries, u64 code address first.
void SetShaders(const u32* r, Memory::Memory& mem) {
    if (r[2] == 0) {
        return;
    }
    Program p;
    for (u32 s = 0; s < std::min<u32>(r[1], 2); ++s) {
        const u32 e = r[2] + 16 * s;
        p.stage[s] = mem.Read32(e) | (u64{mem.Read32(e + 4)} << 32);
    }
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    s_programs[r[0]] = p;
}

// nvnCommandBufferBindProgram(cmd, program, stage bits)
void BindProgram(const u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    s_cmds[r[0]].program = r[1];
}

// Draw recording, under s_lock: fill the program, count the draw, queue it for pairing.
void RecordNvnDraw(u32 cmd, Draw& d) {
    const auto& c = s_cmds[cmd];
    if (const auto p = s_programs.find(c.program); p != s_programs.end()) {
        d.vs = p->second.stage[0];
        d.ps = p->second.stage[1];
    } else {
        ++s_unknown_program_draws;
    }
    Record(s_nvn, d);
    Draw key = d;
    if (key.vs) key.vs += kProgramHeader;
    if (key.ps) key.ps += kProgramHeader;
    if (s_pending_count >= kMaxPending) {
        s_stats.pending_dropped += s_pending_count;
        s_pending.clear();
        s_pending_count = 0;
    }
    s_pending[key].push_back(c.st);
    ++s_pending_count;
}


// Buffer object probe (G3c): what the guest keeps in a Buffer object, found by matching
// addresses the game later binds against fields of the objects handed to nvnBufferGetAddress
// (and of the object their pointer field leads to).
constexpr u32 kProbeWords = 16, kPoolWords = 32;
constexpr size_t kProbeMaxObjs = 20000, kProbeMaxBinds = 300000;
std::map<u32, std::array<u32, kProbeWords>> s_probe_objs;
std::map<u32, std::array<u32, kPoolWords>> s_probe_pools; // by the object's word 2
std::unordered_set<u64> s_probe_binds;
struct ProbePair {
    u64 addr;
    std::array<u32, kProbeWords> obj;
    std::array<u32, kProbeWords> target; // what object word 3 points at
};
std::vector<ProbePair> s_probe_pairs;
std::map<std::thread::id, ProbePair> s_probe_last; // last GetAddress per thread

void ProbeGetAddress(const u32* r, Memory::Memory& mem) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    if (s_probe_objs.size() >= kProbeMaxObjs && !s_probe_objs.count(r[0])) return;
    auto& o = s_probe_objs[r[0]];
    for (u32 k = 0; k < kProbeWords; ++k) o[k] = mem.Read32(r[0] + 4 * k);
    auto& last = s_probe_last[std::this_thread::get_id()];
    last = ProbePair{};
    last.obj = o;
    if (o[3] != 0 && mem.IsValidVirtualAddressRange(o[3], kProbeWords * 4)) {
        for (u32 k = 0; k < kProbeWords; ++k) last.target[k] = mem.Read32(o[3] + 4 * k);
    }
    const u32 pool = o[2];
    if (pool != 0 && !s_probe_pools.count(pool) && mem.IsValidVirtualAddressRange(pool, kPoolWords * 4)) {
        auto& pw = s_probe_pools[pool];
        for (u32 k = 0; k < kPoolWords; ++k) pw[k] = mem.Read32(pool + 4 * k);
    }
}

void ProbeBind(u64 addr) {
    if (s_probe_binds.size() < kProbeMaxBinds) s_probe_binds.insert(addr);
    const auto it = s_probe_last.find(std::this_thread::get_id());
    if (it != s_probe_last.end() && s_probe_pairs.size() < 3000) {
        it->second.addr = addr;
        s_probe_pairs.push_back(it->second);
    }
}

void WriteProbe(const std::filesystem::path& file) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    if (s_probe_objs.empty()) return;
    std::ofstream out(file, std::ios::trunc);
    out << fmt::format("# buffer objects: {}, pools: {}, distinct bound addresses: {}\n", s_probe_objs.size(),
                       s_probe_pools.size(), s_probe_binds.size());
    std::map<std::string, u32> hits;
    for (const auto& [obj, w] : s_probe_objs) {
        for (u32 k = 0; k + 1 < kProbeWords; ++k) {
            const u64 v = w[k] | (u64{w[k + 1]} << 32);
            if (s_probe_binds.count(v)) ++hits[fmt::format("obj u64 at word {} is a bound address", k)];
        }
        const auto pit = s_probe_pools.find(w[2]);
        if (pit == s_probe_pools.end()) continue;
        const auto& pw = pit->second;
        for (u32 j = 0; j + 1 < kPoolWords; ++j) {
            const u64 base = pw[j] | (u64{pw[j + 1]} << 32);
            if (base == 0) continue;
            for (u32 k = 0; k < kProbeWords; ++k) {
                if (s_probe_binds.count(base + w[k]))
                    ++hits[fmt::format("pool u64 word {} + obj u32 word {} is a bound address", j, k)];
            }
            for (u32 k = 0; k + 1 < kProbeWords; ++k) {
                if (s_probe_binds.count(base + (w[k] | (u64{w[k + 1]} << 32))))
                    ++hits[fmt::format("pool u64 word {} + obj u64 word {} is a bound address", j, k)];
            }
        }
    }
    for (const auto& [name, n] : hits) out << fmt::format("{:7} x  {}\n", n, name);
    out << "\n# pairs: bound addr | object words | words at object word 3's target\n";
    for (const auto& pr : s_probe_pairs) {
        out << fmt::format("P {:x} |", pr.addr);
        for (const u32 v : pr.obj) out << fmt::format(" {:x}", v);
        out << " |";
        for (const u32 v : pr.target) out << fmt::format(" {:x}", v);
        out << "\n";
    }
    out << "\n# first pools (ptr then 32 words)\n";
    u32 shown = 0;
    for (const auto& [ptr, pw] : s_probe_pools) {
        if (shown++ >= 8) break;
        out << fmt::format("{:08x}:", ptr);
        for (const u32 v : pw) out << fmt::format(" {:08x}", v);
        out << "\n";
    }
    out << "\n# first objects (addr then 16 words)\n";
    shown = 0;
    for (const auto& [obj, w] : s_probe_objs) {
        if (shown++ >= 6) break;
        out << fmt::format("{:08x}:", obj);
        for (const u32 v : w) out << fmt::format(" {:08x}", v);
        out << "\n";
    }
}


// ---- G3c native entry points. Layouts are from the buffer probe (observation only) ----------
// Buffer object (guest memory): word 3 = pointer to its memory pool object, word 4 = offset in
// the pool, word 5 = size. Pool object: u64 at words 10-11 = the pool's GPU address.
// nvnBufferGetAddress(buffer) -> u64 pool GPU address + offset.
std::atomic<u64> s_native_get_address{0};
bool NativeBufferGetAddress(u32* r, Memory::Memory& mem) {
    s_native_get_address.fetch_add(1, std::memory_order_relaxed);
    const u32 pool = mem.Read32(r[0] + 12);
    const u64 base = mem.Read32(pool + 40) | (u64{mem.Read32(pool + 44)} << 32);
    const u64 addr = base + mem.Read32(r[0] + 16);
    r[0] = static_cast<u32>(addr);
    r[1] = static_cast<u32>(addr >> 32);
    return true;
}

// nvnCommandBufferBindVertexBuffer(cmd, index, u64 address, u64 size)
void BindVertexBuffer(const u32* r, Memory::Memory&) {
    if (r[1] >= DrawState::kStreams) return;
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    s_cmds[r[0]].st.vb[r[1]] = r[2] | (u64{r[3]} << 32);
    ProbeBind(r[2] | (u64{r[3]} << 32));
}

// nvnCommandBufferBindUniformBuffer(cmd, stage, slot, u64 address [sp], size [sp+8])
void BindUniformBuffer(const u32* r, Memory::Memory& mem) {
    if (r[1] >= NvnState::kStages || r[2] >= DrawState::kSlots) return;
    const u32 sp = r[13];
    const u64 addr = mem.Read32(sp) | (u64{mem.Read32(sp + 4)} << 32);
    const u32 size = mem.Read32(sp + 8);
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    auto& st = s_cmds[r[0]].st;
    st.cb_addr[r[1]][r[2]] = addr;
    st.cb_size[r[1]][r[2]] = size;
    ProbeBind(addr);
}

// nvnCommandBufferSetViewports(cmd, first, count, const float* {x, y, w, h} per viewport)
void SetViewports(const u32* r, Memory::Memory& mem) {
    if (r[1] != 0 || r[2] == 0 || r[3] == 0) return;
    std::array<f32, 4> v;
    for (u32 k = 0; k < 4; ++k) {
        const u32 w = mem.Read32(r[3] + 4 * k);
        std::memcpy(&v[k], &w, 4);
    }
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    auto& st = s_cmds[r[0]].st;
    st.viewport = v;
    st.viewport_set = true;
}

// nvnCommandBufferSetRenderTargets(cmd, count, colors**, color views**, depth [sp], depth view [sp+4])
void SetRenderTargets(const u32* r, Memory::Memory& mem) {
    const u32 rt0 = r[1] && r[2] ? mem.Read32(r[2]) : 0;
    const u32 depth = mem.Read32(r[13]);
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    auto& st = s_cmds[r[0]].st;
    st.rt_count = r[1];
    st.rt0_tex = rt0;
    st.zeta_tex = depth;
}

// Texture builders: (builder, value) setters, snapshotted by nvnTextureInitialize(texture, builder).
template <u32 TexInfo::*Field>
void BuilderSet(const u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    s_builders[r[0]].*Field = r[1];
}
void BuilderDefaults(const u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    s_builders[r[0]] = TexInfo{};
}
// nvnTextureBuilderSetStorage(builder, pool, offset)
void BuilderStorage(const u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    auto& b = s_builders[r[0]];
    b.pool = r[1];
    b.offset = r[2];
}
void TextureInitialize(const u32* r, Memory::Memory&) {
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    s_textures[r[0]] = s_builders[r[1]];
}

// nvnCommandBufferDrawElementsBaseVertex(cmd, mode, index type, count, u64 index address, base vertex)
void DrawElementsBaseVertex(const u32* r, Memory::Memory& mem) {
    const u32 sp = r[13];
    Draw d{};
    d.topology = r[1];
    d.indexed = 1;
    d.index_format = r[2];
    d.count = r[3];
    d.index_addr = mem.Read32(sp) | (u64{mem.Read32(sp + 4)} << 32);
    d.base_vertex = static_cast<s32>(mem.Read32(sp + 8));
    d.instances = 1;
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    RecordNvnDraw(r[0], d);
}

// nvnCommandBufferDrawArrays(cmd, mode, first, count)
void DrawArrays(const u32* r, Memory::Memory&) {
    Draw d{};
    d.topology = r[1];
    d.first = r[2];
    d.count = r[3];
    d.instances = 1;
    Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
    RecordNvnDraw(r[0], d);
}

struct Projection {
    const char* name;
    Draw (*key)(const Draw&);
};

// Each projection keeps more of the draw; agreement drops at the first field that decodes wrong.
const std::array<Projection, 4> kProjections{{
    {"indexed, count", [](const Draw& d) { return Draw{0, d.indexed, d.count}; }},
    {"+ topology", [](const Draw& d) { return Draw{d.topology, d.indexed, d.count}; }},
    {"+ first vertex, index format, instances",
     [](const Draw& d) {
         Draw k{d.topology, d.indexed, d.count, d.first};
         k.index_format = d.index_format;
         k.instances = d.instances;
         return k;
     }},
    {"+ index address, base vertex",
     [](const Draw& d) {
         Draw k = d;
         k.vs = k.ps = 0;
         return k;
     }},
}};

DrawCounts Project(const DrawCounts& in, Draw (*key)(const Draw&)) {
    DrawCounts out;
    for (const auto& [d, n] : in) {
        out[key(d)] += n;
    }
    return out;
}

u64 Matched(const DrawCounts& a, const DrawCounts& b) {
    u64 m = 0;
    for (const auto& [d, n] : a) {
        if (const auto it = b.find(d); it != b.end()) {
            m += std::min(n, it->second);
        }
    }
    return m;
}

std::string Describe(const Draw& d) {
    return fmt::format("topo {} {} count {} first {} fmt {} inst {} idx {:x} base {} vs {:x} ps {:x}", d.topology,
                       d.indexed ? "indexed" : "arrays", d.count, d.first, d.index_format, d.instances,
                       d.index_addr, d.base_vertex, d.vs, d.ps);
}

// Most common (gpu - nvn) difference between two address sets, and how many addresses it explains.
std::pair<s64, size_t> BestDelta(const std::unordered_set<u64>& nvn, const std::unordered_set<u64>& gpu) {
    std::unordered_map<s64, size_t> hist;
    size_t i = 0;
    for (const u64 a : nvn) {
        if (++i > 800) break;
        size_t j = 0;
        for (const u64 b : gpu) {
            if (++j > 800) break;
            ++hist[static_cast<s64>(b - a)];
        }
    }
    std::pair<s64, size_t> best{0, 0};
    for (const auto& [delta, n] : hist) {
        if (n > best.second) best = {delta, n};
    }
    return best;
}

} // namespace

Observer Find(const std::string& name) {
    if (name == "nvnBufferGetAddress") return ProbeGetAddress;
    if (name == "nvnProgramSetShaders") return SetShaders;
    if (name == "nvnCommandBufferBindProgram") return BindProgram;
    if (name == "nvnCommandBufferDrawElementsBaseVertex") return DrawElementsBaseVertex;
    if (name == "nvnCommandBufferDrawArrays") return DrawArrays;
    if (name == "nvnCommandBufferBindVertexBuffer") return BindVertexBuffer;
    if (name == "nvnCommandBufferBindUniformBuffer") return BindUniformBuffer;
    if (name == "nvnCommandBufferSetViewports") return SetViewports;
    if (name == "nvnCommandBufferSetRenderTargets") return SetRenderTargets;
    if (name == "nvnTextureBuilderSetDefaults") return BuilderDefaults;
    if (name == "nvnTextureBuilderSetWidth") return BuilderSet<&TexInfo::width>;
    if (name == "nvnTextureBuilderSetHeight") return BuilderSet<&TexInfo::height>;
    if (name == "nvnTextureBuilderSetDepth") return BuilderSet<&TexInfo::depth>;
    if (name == "nvnTextureBuilderSetFormat") return BuilderSet<&TexInfo::format>;
    if (name == "nvnTextureBuilderSetTarget") return BuilderSet<&TexInfo::target>;
    if (name == "nvnTextureBuilderSetLevels") return BuilderSet<&TexInfo::levels>;
    if (name == "nvnTextureBuilderSetStorage") return BuilderStorage;
    if (name == "nvnTextureInitialize") return TextureInitialize;
    return nullptr;
}


namespace {

std::atomic<u64> s_native_served[32];
std::atomic<u64> s_native_declined[32];

// A builder produces the Maxwell words the guest driver records for one call, or declines
// (anything not yet observed). The same builder serves native_nvn (written straight into the
// command buffer) and the verify census (compared word for word with what the guest wrote).
bool BuildUniformBuffer(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 sp = r[13];
    const u32 size = mem.Read32(sp + 8);
    if (r[1] > 1 || r[2] > 8 || size == 0 || size > 0x200) return false;
    w.n = 5;
    w.v[0] = 0x200308e0;
    w.v[1] = (size + 255) & ~255u;
    w.v[2] = mem.Read32(sp + 4);
    w.v[3] = mem.Read32(sp);
    w.v[4] = 0x80000000u | (((r[2] + 3) << 4 | 1) << 16) | (0x904 + 0x20 * r[1]);
    return true;
}

bool BuildTexture(const u32* r, Memory::Memory& mem, NativeWords& w) {
    if (r[1] > 1 || r[2] > 7) return false;
    w.n = 5;
    w.v[0] = 0x80000e0eu | (r[1] << 18);
    w.v[1] = 0x8000068b;
    w.v[2] = 0x200208e3;
    w.v[3] = 0x20 + 8 * r[2];
    w.v[4] = mem.Read32(r[13]);
    return true;
}

bool BuildVertexBuffer(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 size = mem.Read32(r[13]);
    if (r[1] != 0 || size == 0) return false;
    const u64 limit = (r[2] | (u64{r[3]} << 32)) + size - 1;
    w.n = 6;
    w.v[0] = 0x20020701;
    w.v[1] = r[3];
    w.v[2] = r[2];
    w.v[3] = 0x200207c0;
    w.v[4] = static_cast<u32>(limit >> 32);
    w.v[5] = static_cast<u32>(limit);
    return true;
}

// nvnCommandBufferDrawElementsBaseVertex(cmd, mode, index type, count, u64 index address [sp],
// base vertex [sp+8]): only mode 5, type 1, base vertex 0 observed.
bool BuildDrawElements(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 sp = r[13];
    if (r[1] != 5 || r[2] != 1 || mem.Read32(sp + 8) != 0) return false;
    w.n = 12;
    w.v[0] = 0x20010446;
    w.v[1] = 0;
    w.v[2] = 0x2001050d;
    w.v[3] = 0;
    w.v[4] = 0xa0050e36;
    w.v[5] = 5;
    w.v[6] = mem.Read32(sp + 4);
    w.v[7] = mem.Read32(sp);
    w.v[8] = 1;
    w.v[9] = r[3];
    w.v[10] = 0x8000050d;
    w.v[11] = 0x80000446;
    return true;
}

// nvnCommandBufferBindColorState(state object): hypothesis from samples, checked by verify.
bool BuildColorState(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 o = mem.Read32(r[1]);
    w.v[0] = 0x20010e10;
    w.v[1] = o;
    if ((o >> 24) == 0x08) {
        w.n = 3;
        w.v[2] = 0x800004bb;
    } else if ((o >> 24) == 0x05) {
        w.n = 4;
        w.v[2] = 0x800104bb;
        w.v[3] = 0x800504c5;
    } else {
        return false;
    }
    return true;
}

// nvnCommandBufferBindVertexStreamState(count, states): hypothesis from samples, checked by verify.
bool BuildVertexStream(const u32* r, Memory::Memory& mem, NativeWords& w) {
    if (r[1] != 4 || r[2] == 0) return false;
    const u32 stride = mem.Read32(r[2]);
    w.n = 20;
    static const u32 tail[20] = {0, 0x80000620, 0x90000704, 0x80000621, 0x90000708, 0x80000622,
                                 0x9000070c, 0x80000623, 0x80000710, 0x80000714, 0x80000718,
                                 0x8000071c, 0x80000720, 0x80000724, 0x80000728, 0x8000072c,
                                 0x80000730, 0x80000734, 0x80000738, 0x8000073c};
    for (u32 k = 0; k < 20; ++k) w.v[k] = tail[k];
    w.v[0] = 0x90000700u | (stride << 16);
    return true;
}


bool BuildDrawArrays(const u32* r, Memory::Memory&, NativeWords& w) {
    if (r[1] != 5) return false;
    w.n = 4;
    w.v[0] = 0xa0030e30;
    w.v[1] = 5;
    w.v[2] = r[2];
    w.v[3] = r[3];
    return true;
}

bool BuildDepthClamp(const u32* r, Memory::Memory&, NativeWords& w) {
    if (r[1] > 1) return false;
    w.n = 2;
    w.v[0] = 0x2001064f;
    w.v[1] = r[1] ? 0x101a : 0x181d;
    return true;
}

u32 FloatBits(f32 f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

void ViewportWords(f32 x, f32 y, f32 w, f32 h, NativeWords& o) {
    o.n = 9;
    o.v[0] = 0x20020280;
    o.v[1] = FloatBits(w * 0.5f);
    o.v[2] = FloatBits(-h * 0.5f);
    o.v[3] = 0x20020283;
    o.v[4] = FloatBits(x + w * 0.5f);
    o.v[5] = FloatBits(y + h * 0.5f);
    o.v[6] = 0x20020300;
    o.v[7] = static_cast<u32>(x) | (static_cast<u32>(w) << 16);
    o.v[8] = static_cast<u32>(y) | (static_cast<u32>(h) << 16);
}

// nvnCommandBufferSetViewport(cmd, x, y, w, h): integers, as observed.
bool BuildViewport(const u32* r, Memory::Memory& mem, NativeWords& w) {
    if (r[1] > 0xffff || r[2] > 0xffff || r[3] > 0xffff) return false;
    const u32 h = mem.Read32(r[13]);
    if (h > 0xffff) return false;
    ViewportWords(static_cast<f32>(r[1]), static_cast<f32>(r[2]), static_cast<f32>(r[3]),
                  static_cast<f32>(h), w);
    return true;
}

// nvnCommandBufferSetViewports(cmd, first, count, const float* {x, y, w, h}).
bool BuildViewports(const u32* r, Memory::Memory& mem, NativeWords& w) {
    if (r[1] != 0 || r[2] != 1 || r[3] == 0) return false;
    f32 v[4];
    for (u32 k = 0; k < 4; ++k) {
        const u32 b = mem.Read32(r[3] + 4 * k);
        std::memcpy(&v[k], &b, 4);
    }
    if (!(v[0] >= 0 && v[1] >= 0 && v[2] >= 0 && v[3] >= 0 && v[2] < 65536 && v[3] < 65536)) return false;
    ViewportWords(v[0], v[1], v[2], v[3], w);
    return true;
}

bool BuildScissor(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 h = mem.Read32(r[13]);
    if (r[1] > 0xffff || r[2] > 0xffff || r[3] > 0xffff || h > 0xffff) return false;
    w.n = 3;
    w.v[0] = 0x20020381;
    w.v[1] = r[1] | ((r[1] + r[3]) << 16);
    w.v[2] = r[2] | ((r[2] + h) << 16);
    return true;
}

bool BuildScissors(const u32* r, Memory::Memory& mem, NativeWords& w) {
    if (r[1] != 0 || r[2] != 1 || r[3] == 0) return false;
    u32 v[4];
    for (u32 k = 0; k < 4; ++k) v[k] = mem.Read32(r[3] + 4 * k);
    if (v[0] > 0xffff || v[1] > 0xffff || v[2] > 0xffff || v[3] > 0xffff) return false;
    w.n = 3;
    w.v[0] = 0x20020381;
    w.v[1] = v[0] | ((v[0] + v[2]) << 16);
    w.v[2] = v[1] | ((v[1] + v[3]) << 16);
    return true;
}

// nvnCommandBufferBindChannelMaskState(state): one word per byte of the object's mask, each bit a nibble.
bool BuildChannelMask(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 o = mem.Read32(r[1]);
    w.n = 5;
    w.v[0] = 0xa0040e12;
    for (u32 c = 0; c < 4; ++c) {
        u32 word = 0;
        for (u32 t = 0; t < 8; ++t) word |= ((o >> (c * 8 + t)) & 1u) << (t * 4);
        w.v[1 + c] = word;
    }
    return true;
}

// nvnCommandBufferBindBlendState(state): state = {0x110c0000 | target, packed equation bytes}.
bool BuildBlendState(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 head = mem.Read32(r[1]);
    const u32 q = mem.Read32(r[1] + 4);
    if ((head & 0xfffffff8u) != 0x110c0000u) return false;
    w.n = 10;
    w.v[0] = 0x80030d35;
    w.v[1] = 0x80000374;
    w.v[2] = 0x800104b9;
    w.v[3] = 0x20060781 + 8 * (head & 7);
    w.v[4] = 1;
    w.v[5] = q & 0xff;
    w.v[6] = (q >> 8) & 0xff;
    w.v[7] = 1;
    w.v[8] = (q >> 16) & 0xff;
    w.v[9] = q >> 24;
    return true;
}

// nvnCommandBufferBindDepthStencilState(state): flags word 0 (test, write, stencil, function in bits 4+).
bool BuildDepthStencilState(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 f = mem.Read32(r[1]);
    if (f & 0x4) return false;  // stencil: not decoded yet
    if (!(f & 1)) {
        if (f & 0x2) return false;
        w.n = 2;
        w.v[0] = 0x800004b3;
        w.v[1] = 0x800004e0;
        return true;
    }
    w.n = 4;
    w.v[0] = 0x80000000u | (((f >> 1) & 1u) << 16) | 0x4ba;
    w.v[1] = 0x800104b3;
    w.v[2] = 0x80000000u | (((f >> 4) & 0xfu) << 16) | 0x4c3;
    w.v[3] = 0x800004e0;
    return true;
}

// nvnCommandBufferBindPolygonState(state): bit 1 cull, bit 2 counter-clockwise, bit 17 offset, bit 4 always.
bool BuildPolygonState(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 f = mem.Read32(r[1]);
    if ((f & ~0x2001eu) != 0 || !(f & 0x10)) return false;
    const bool cull = (f & 0x2) != 0;
    w.n = 0;
    w.v[w.n++] = 0x80000000u | ((0x900u | ((f >> 2) & 1u)) << 16) | 0x647;
    w.v[w.n++] = 0x80000000u | ((cull ? 1u : 0u) << 16) | 0x646;
    if (cull) w.v[w.n++] = 0x84050648;
    w.v[w.n++] = 0x9b02036b;
    w.v[w.n++] = 0x9b02036c;
    w.v[w.n++] = 0x80000000u | (((f >> 17) & 1u) << 16) | 0x372;
    return true;
}

// nvnCommandBufferBindVertexAttribState(16, attribs): one word per slot, format enum translated by table.
bool BuildVertexAttribState(const u32* r, Memory::Memory& mem, NativeWords& w) {
    if (r[1] != 16 || !mem.IsValidVirtualAddressRange(r[2], 64)) return false;
    static const struct {
        u32 in, out;
    } kFormats[] = {{0x044, 0x384}, {0x04a, 0x114}, {0x02c, 0x388}, {0x056, 0x086}, {0x04c, 0x094},
                    {0x04e, 0x214}, {0x052, 0x386}, {0x022, 0x39e}, {0x058, 0x19e}, {0x050, 0x194},
                    {0x05c, 0x382}, {0x07a, 0x160}};
    w.n = 17;
    w.v[0] = 0x20100458;
    for (u32 i = 0; i < 16; ++i) {
        const u32 o = mem.Read32(r[2] + 4 * i);
        if (o == 0) {
            w.v[1 + i] = 0x3a400040;
            continue;
        }
        bool found = false;
        for (const auto& f : kFormats) {
            if (f.in == (o >> 20)) {
                w.v[1 + i] = (f.out << 20) | (o & 0xfffbf);
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

// nvnCommandBufferBindProgram(program, stages): the program object keeps its command words
// ready-made (object word 2 -> block, word 4 = word count, word 5 = stage mask).
bool BuildProgram(const u32* r, Memory::Memory& mem, NativeWords& w) {
    const u32 prog = r[1];
    if (!mem.IsValidVirtualAddressRange(prog, 24)) return false;
    const u32 block = mem.Read32(prog + 8);
    const u32 count = mem.Read32(prog + 16);
    if (r[2] != mem.Read32(prog + 20) || count == 0 || count > 32 ||
        !mem.IsValidVirtualAddressRange(block, count * 4)) {
        return false;
    }
    w.n = count;
    for (u32 i = 0; i < count; ++i) w.v[i] = mem.Read32(block + 4 * i);
    return true;
}

// nvnCommandBufferSetRenderTargets(1, colors**, views**, depth [sp]): one color target, no depth.
// Texture object: word 3 = pool, 4 = offset, 7/8 = size, 11 = format, 20 = byte size; the pool
// holds its texture mapping address at words 18-19.
bool BuildRenderTargets(const u32* r, Memory::Memory& mem, NativeWords& w) {
    if (r[1] != 1 || r[2] == 0 || mem.Read32(r[13]) != 0) return false;
    if (r[3] != 0 && mem.Read32(r[3]) != 0) return false;  // a view: not decoded
    const u32 tex = mem.Read32(r[2]);
    if (!mem.IsValidVirtualAddressRange(tex, 96)) return false;
    const u32 pool = mem.Read32(tex + 12);
    if (!mem.IsValidVirtualAddressRange(pool, 80) || mem.Read32(tex + 44) != 0x25) return false;
    const u64 va = (mem.Read32(pool + 72) | (u64{mem.Read32(pool + 76)} << 32)) + mem.Read32(tex + 16);
    const u32 width = mem.Read32(tex + 28);
    const u32 height = mem.Read32(tex + 32);
    u32 gobs = 1;
    while (gobs < 16 && gobs * 2 <= (height + 7) / 8) gobs *= 2;
    u32 log2 = 0;
    while ((1u << log2) < gobs) ++log2;
    w.n = 24;
    w.v[0] = 0x20010487;
    w.v[1] = 0x0fac6881;
    w.v[2] = 0x20080200;
    w.v[3] = static_cast<u32>(va >> 32);
    w.v[4] = static_cast<u32>(va);
    w.v[5] = (width + 15) & ~15u;
    w.v[6] = height;
    w.v[7] = 0xd5;
    w.v[8] = log2 << 4;
    w.v[9] = 1;
    w.v[10] = mem.Read32(tex + 80) >> 2;
    for (u32 i = 0; i < 7; ++i) w.v[11 + i] = 0x80000214 + 0x10 * i;
    w.v[18] = 0x8000054e;
    w.v[19] = 0x80000d28;
    w.v[20] = 0x200203fd;
    w.v[21] = width << 16;
    w.v[22] = height << 16;
    w.v[23] = 0x80000574;
    return true;
}

struct NativeCmd {
    const char* name;
    NativeBuilder build;
    bool proven;  // verified word for word against the guest: only these are switched on
};
const NativeCmd kNativeCmds[] = {
    {"nvnCommandBufferBindUniformBuffer", BuildUniformBuffer, true},
    {"nvnCommandBufferBindTexture", BuildTexture, true},
    {"nvnCommandBufferBindVertexBuffer", BuildVertexBuffer, true},
    {"nvnCommandBufferDrawElementsBaseVertex", BuildDrawElements, true},
    {"nvnCommandBufferBindColorState", BuildColorState, true},
    {"nvnCommandBufferBindVertexStreamState", BuildVertexStream, true},
    {"nvnCommandBufferDrawArrays", BuildDrawArrays, true},
    {"nvnCommandBufferSetDepthClamp", BuildDepthClamp, true},
    {"nvnCommandBufferSetViewport", BuildViewport, true},
    {"nvnCommandBufferSetViewports", BuildViewports, true},
    {"nvnCommandBufferSetScissor", BuildScissor, true},
    {"nvnCommandBufferSetScissors", BuildScissors, true},
    {"nvnCommandBufferBindChannelMaskState", BuildChannelMask, true},
    {"nvnCommandBufferBindBlendState", BuildBlendState, true},
    {"nvnCommandBufferBindDepthStencilState", BuildDepthStencilState, true},
    {"nvnCommandBufferBindPolygonState", BuildPolygonState, true},
    {"nvnCommandBufferBindVertexAttribState", BuildVertexAttribState, true},
    {"nvnCommandBufferBindProgram", BuildProgram, true},
    {"nvnCommandBufferSetRenderTargets", BuildRenderTargets, true},
};
constexpr size_t kNativeCount = sizeof(kNativeCmds) / sizeof(kNativeCmds[0]);

template <size_t I>
bool NativeCommand(u32* r, Memory::Memory& mem) {
    if (GameSettings::NvnVulkanMode() != 0) {
        static const int rec_index = NvnRec::ResolveCall(kNativeCmds[I].name); // name -> recorder entry, once
        NvnRec::OnCallResolved(rec_index, kNativeCmds[I].name, r, mem);
    }
    const u32 cmd = r[0];
    const u32 wp = mem.Read32(cmd + 12);
    const u32 end = mem.Read32(cmd + 16);
    const u32 start = mem.Read32(cmd + 20);
    NativeWords w;
    // Near the end of a chunk the guest driver (which owns chunk switching) runs the call.
    if ((wp & 3) != 0 || wp < start || end < wp || end - wp < 0x2000 ||
        !kNativeCmds[I].build(r, mem, w)) {
        s_native_declined[I].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (u32 k = 0; k < w.n; ++k) {
        mem.Write32(wp + 4 * k, w.v[k]);
    }
    mem.Write32(cmd + 12, wp + 4 * w.n);
    s_native_served[I].fetch_add(1, std::memory_order_relaxed);
    return true;
}

template <size_t... Is>
void RegisterAll(std::index_sequence<Is...>) {
    ((kNativeCmds[Is].proven ? NvnTrace::Get().Replace(kNativeCmds[Is].name, NativeCommand<Is>) : void()), ...);
}

} // namespace

NativeBuilder FindBuilder(const std::string& name) {
    for (const auto& c : kNativeCmds) {
        if (name == c.name) return c.build;
    }
    return nullptr;
}
u64 NativeCalls() {
    u64 n = s_native_get_address.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kNativeCount; ++i) n += s_native_served[i].load(std::memory_order_relaxed);
    return n;
}

void RegisterNative() {
    NvnTrace::Get().Replace("nvnBufferGetAddress", NativeBufferGetAddress);
    RegisterAll(std::make_index_sequence<kNativeCount>{});
}

void Start() {
    Common::GpuProbe::draw_sink.store(GpuDraw, std::memory_order_relaxed);
}

void Flush(const std::filesystem::path& file) {
    WriteProbe(file.parent_path() / "nvn_bufprobe.txt");
    Side nvn, gpu;
    u64 unknown_program;
    size_t programs, textures, pending;
    Stats st;
    {
        Kernel::StallRing::TimedGuard lk{s_lock, Kernel::StallRing::LockShadow};
        nvn = s_nvn;
        gpu = s_gpu;
        unknown_program = s_unknown_program_draws;
        programs = s_programs.size();
        textures = s_textures.size();
        pending = s_pending_count;
        st = s_stats;
    }
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# NVN shadow layer vs GPU emulation: every draw, as the game issued it (NVN) and as the "
           "emulated GPU executed it\n";
    out << fmt::format("draws: NVN {} ({} distinct{}), GPU {} ({} distinct{})\n", nvn.total, nvn.draws.size(),
                       nvn.dropped ? fmt::format(", {} past cap", nvn.dropped) : "", gpu.total,
                       gpu.draws.size(), gpu.dropped ? fmt::format(", {} past cap", gpu.dropped) : "");
    out << fmt::format("programs seen: {}; NVN draws with no known program: {}\n", programs, unknown_program);
    out << fmt::format("native entry points: native_nvn {}, nvnBufferGetAddress served natively {} times\n",
                       GameSettings::NativeNvn() ? "on" : "off", s_native_get_address.load());
    for (size_t i = 0; i < kNativeCount; ++i) {
        out << fmt::format("  {} served natively {} times, declined (guest ran) {}{}", kNativeCmds[i].name,
                           s_native_served[i].load(), s_native_declined[i].load(), char(10));
    }
    if (nvn.total == 0 || gpu.total == 0) {
        return;
    }
    const double denom = static_cast<double>(std::max(nvn.total, gpu.total));

    out << "\n## agreement (draws that match on these fields / larger total)\n";
    DrawCounts last_n, last_g;
    for (const auto& p : kProjections) {
        last_n = Project(nvn.draws, p.key);
        last_g = Project(gpu.draws, p.key);
        out << fmt::format("{:6.2f}%  {}\n", 100.0 * static_cast<double>(Matched(last_n, last_g)) / denom, p.name);
    }

    // Shader addresses: NVN hands the driver code addresses, Maxwell gets region + offset; find
    // the constant between them for each stage (and check the stage order).
    std::unordered_set<u64> n0, n1, gv, gp;
    for (const auto& [d, c] : nvn.draws) {
        if (d.vs) n0.insert(d.vs);
        if (d.ps) n1.insert(d.ps);
    }
    for (const auto& [d, c] : gpu.draws) {
        if (d.vs) gv.insert(d.vs);
        if (d.ps) gp.insert(d.ps);
    }
    const auto [dvs, nvs] = BestDelta(n0, gv);
    const auto [dps, nps] = BestDelta(n1, gp);
    const auto [dx, nx] = BestDelta(n0, gp);
    out << fmt::format("\n## shader addresses (distinct: NVN stage0 {}, stage1 {}; GPU vertex {}, pixel {})\n",
                       n0.size(), n1.size(), gv.size(), gp.size());
    out << fmt::format("stage0 -> vertex: offset {:#x} explains {}\n", dvs, nvs);
    out << fmt::format("stage1 -> pixel:  offset {:#x} explains {}\n", dps, nps);
    out << fmt::format("stage0 -> pixel:  offset {:#x} explains {} (stage order check)\n", dx, nx);
    DrawCounts shifted;
    for (const auto& [d, c] : nvn.draws) {
        Draw k = d;
        if (k.vs) k.vs += dvs;
        if (k.ps) k.ps += dps;
        shifted[k] += c;
    }
    out << fmt::format("{:6.2f}%  + vertex and pixel program (with the offsets above)\n",
                       100.0 * static_cast<double>(Matched(shifted, gpu.draws)) / denom);

    // Biggest disagreements on the full draw, both directions.
    const auto top = [&](const DrawCounts& a, const DrawCounts& b, const char* title) {
        std::vector<std::pair<u64, Draw>> rows;
        for (const auto& [d, n] : a) {
            const auto it = b.find(d);
            const u64 other = it == b.end() ? 0 : it->second;
            if (n > other) rows.emplace_back(n - other, d);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        out << "\n## " << title << " (top 16)\n";
        for (size_t i = 0; i < std::min<size_t>(rows.size(), 16); ++i) {
            out << rows[i].first << '\t' << Describe(rows[i].second) << '\n';
        }
    };
    top(shifted, gpu.draws, "only in NVN (shader offsets applied)");
    top(gpu.draws, shifted, "only in GPU emulation");

    // ---- draw state, compared on paired draws
    const auto pct = [](u64 a, u64 b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
    out << fmt::format("\n## draw state on paired draws: {} paired, {} GPU draws unpaired, {} waiting, {} dropped; "
                       "{} textures known\n",
                       st.paired, st.gpu_unpaired, pending, st.pending_dropped, textures);
    for (u32 k = 0; k < DrawState::kStreams; ++k) {
        if (st.vb_total[k]) {
            out << fmt::format("{:6.2f}%  vertex buffer {} address ({} draws)\n", pct(st.vb_equal[k], st.vb_total[k]),
                               k, st.vb_total[k]);
        }
    }
    u64 cb_matched = 0;
    for (const auto& [key, n] : st.cb_map) cb_matched += n;
    out << fmt::format("{:6.2f}%  uniform buffer bound at the same address in some GPU slot ({} bindings); "
                       "sizes equal for {:.2f}% of those\n",
                       pct(cb_matched, st.cb_total), st.cb_total, pct(st.cb_size_equal, cb_matched));
    out << "   NVN stage/slot -> GPU stage/slot: count\n";
    for (const auto& [key, n] : st.cb_map) {
        out << fmt::format("   {}/{} -> {}/{}: {}\n", key >> 24, (key >> 16) & 0xFF, (key >> 8) & 0xFF, key & 0xFF, n);
    }
    for (const auto& [key, n] : st.cb_unmatched) {
        out << fmt::format("   {}/{} -> no GPU slot: {}\n", key >> 8, key & 0xFF, n);
    }
    out << fmt::format("{:6.2f}%  viewport 0 exact ({} draws), {:.2f}% width/height\n", pct(st.vp_equal, st.vp_total),
                       st.vp_total, pct(st.vp_size_equal, st.vp_total));
    out << fmt::format("{:6.2f}%  render target count ({} draws)\n", pct(st.rt_count_equal, st.rt_total),
                       st.rt_total);
    out << fmt::format("{:6.2f}%  render target 0 size, on {} draws whose texture is known\n",
                       pct(st.rt_size_equal, st.rt_tex_known), st.rt_tex_known);
    out << "   render target 0 format, NVN -> GPU: count\n";
    for (const auto& [f, n] : st.rt_formats) out << fmt::format("   {:#x} -> {:#x}: {}\n", f.first, f.second, n);
    out << "   render target 0 storage: pool -> (GPU address - offset), the pool's GPU base if constant\n";
    for (const auto& [pool, bases] : st.rt_pool_base) {
        u64 total = 0, best = 0, best_base = 0;
        for (const auto& [b, n] : bases) {
            total += n;
            if (n > best) best = n, best_base = b;
        }
        out << fmt::format("   pool {:08x}: base {:x} for {:.2f}% of {} draws ({} distinct)\n", pool, best_base,
                           pct(best, total), total, bases.size());
    }
    out << fmt::format("   depth: both {}, NVN only {}, GPU only {}\n", st.zeta_both, st.zeta_nvn_only,
                       st.zeta_gpu_only);
    if (!st.examples.empty()) {
        out << "\n## first mismatches\n";
        for (const auto& e : st.examples) out << e << '\n';
    }
}

} // namespace Core::NvnShadow
