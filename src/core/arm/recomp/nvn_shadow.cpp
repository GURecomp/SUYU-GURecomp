// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/gpu_probe.h"
#include "core/arm/recomp/nvn_shadow.h"
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
    std::scoped_lock lk{s_lock};
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
    std::scoped_lock lk{s_lock};
    s_programs[r[0]] = p;
}

// nvnCommandBufferBindProgram(cmd, program, stage bits)
void BindProgram(const u32* r, Memory::Memory&) {
    std::scoped_lock lk{s_lock};
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

// nvnCommandBufferBindVertexBuffer(cmd, index, u64 address, u64 size)
void BindVertexBuffer(const u32* r, Memory::Memory&) {
    if (r[1] >= DrawState::kStreams) return;
    std::scoped_lock lk{s_lock};
    s_cmds[r[0]].st.vb[r[1]] = r[2] | (u64{r[3]} << 32);
}

// nvnCommandBufferBindUniformBuffer(cmd, stage, slot, u64 address [sp], size [sp+8])
void BindUniformBuffer(const u32* r, Memory::Memory& mem) {
    if (r[1] >= NvnState::kStages || r[2] >= DrawState::kSlots) return;
    const u32 sp = r[13];
    const u64 addr = mem.Read32(sp) | (u64{mem.Read32(sp + 4)} << 32);
    const u32 size = mem.Read32(sp + 8);
    std::scoped_lock lk{s_lock};
    auto& st = s_cmds[r[0]].st;
    st.cb_addr[r[1]][r[2]] = addr;
    st.cb_size[r[1]][r[2]] = size;
}

// nvnCommandBufferSetViewports(cmd, first, count, const float* {x, y, w, h} per viewport)
void SetViewports(const u32* r, Memory::Memory& mem) {
    if (r[1] != 0 || r[2] == 0 || r[3] == 0) return;
    std::array<f32, 4> v;
    for (u32 k = 0; k < 4; ++k) {
        const u32 w = mem.Read32(r[3] + 4 * k);
        std::memcpy(&v[k], &w, 4);
    }
    std::scoped_lock lk{s_lock};
    auto& st = s_cmds[r[0]].st;
    st.viewport = v;
    st.viewport_set = true;
}

// nvnCommandBufferSetRenderTargets(cmd, count, colors**, color views**, depth [sp], depth view [sp+4])
void SetRenderTargets(const u32* r, Memory::Memory& mem) {
    const u32 rt0 = r[1] && r[2] ? mem.Read32(r[2]) : 0;
    const u32 depth = mem.Read32(r[13]);
    std::scoped_lock lk{s_lock};
    auto& st = s_cmds[r[0]].st;
    st.rt_count = r[1];
    st.rt0_tex = rt0;
    st.zeta_tex = depth;
}

// Texture builders: (builder, value) setters, snapshotted by nvnTextureInitialize(texture, builder).
template <u32 TexInfo::*Field>
void BuilderSet(const u32* r, Memory::Memory&) {
    std::scoped_lock lk{s_lock};
    s_builders[r[0]].*Field = r[1];
}
void BuilderDefaults(const u32* r, Memory::Memory&) {
    std::scoped_lock lk{s_lock};
    s_builders[r[0]] = TexInfo{};
}
// nvnTextureBuilderSetStorage(builder, pool, offset)
void BuilderStorage(const u32* r, Memory::Memory&) {
    std::scoped_lock lk{s_lock};
    auto& b = s_builders[r[0]];
    b.pool = r[1];
    b.offset = r[2];
}
void TextureInitialize(const u32* r, Memory::Memory&) {
    std::scoped_lock lk{s_lock};
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
    std::scoped_lock lk{s_lock};
    RecordNvnDraw(r[0], d);
}

// nvnCommandBufferDrawArrays(cmd, mode, first, count)
void DrawArrays(const u32* r, Memory::Memory&) {
    Draw d{};
    d.topology = r[1];
    d.first = r[2];
    d.count = r[3];
    d.instances = 1;
    std::scoped_lock lk{s_lock};
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

void Start() {
    Common::GpuProbe::draw_sink.store(GpuDraw, std::memory_order_relaxed);
}

void Flush(const std::filesystem::path& file) {
    Side nvn, gpu;
    u64 unknown_program;
    size_t programs, textures, pending;
    Stats st;
    {
        std::scoped_lock lk{s_lock};
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
