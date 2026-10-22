// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "core/arm/debug.h"
#include "core/arm/dynarmic/arm_dynarmic_32.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/arm/recomp/arm_recomp32.h"
#include "core/arm/recomp/mod_host.h"
#include "core/arm/recomp/game_settings.h"
#include "core/arm/recomp/host_threads.h"
#include "core/arm/recomp/nvn_trace.h"
#include "core/arm/recomp/recomp_hooks.h"
#include "core/arm/recomp/timing_watch.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/fs_monitor.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/stall_ring.h"
#include "core/memory.h"

namespace Core {

namespace {

// Mirrors A32Context in the generated runtime (a32_runtime.h). Nothing links the
// two builds, so the layout is pinned on both sides.
struct A32ContextView {
    u32 r[16];
    u32 n, z, c, v, q;
    u32 ge;
    u32 thumb;
    u32 fpscr;
    union {
        u32 s[64];
        u64 d[32];
    } ext;
    u32 halted;
    u32 tpidruro;
    u32 tpidrurw;
    u32 pending_svc;
    const void* host;
};
static_assert(offsetof(A32ContextView, ext) == 96);
static_assert(offsetof(A32ContextView, halted) == 352);
static_assert(offsetof(A32ContextView, pending_svc) == 364);
static_assert(offsetof(A32ContextView, host) == 368);
// The mod loader sees the context's prefix as ModHostCpu (mod_host_api.h).
static_assert(offsetof(ModHostCpu, n) == offsetof(A32ContextView, n));
static_assert(offsetof(ModHostCpu, fpscr) == offsetof(A32ContextView, fpscr));
static_assert(offsetof(ModHostCpu, vfp) == offsetof(A32ContextView, ext));
static_assert(sizeof(ModHostCpu) == offsetof(A32ContextView, halted));

// Mirrors A32HostMem in a32_runtime.h.
struct A32HostMemView {
    void* user;
    u64 (*load)(void* user, u64 va, u32 size);
    void (*store)(void* user, u64 va, u32 size, u64 value);
    u64 (*excl_load)(void* user, u64 va, u32 size);
    u32 (*excl_store)(void* user, u64 va, u32 size, u64 value);
    void (*clear_excl)(void* user);
    u64 (*read_counter)(void* user);
    const void* page_entries;
    u64 page_entry_stride;
    u64 page_bits;
    u64 pointer_mask;
    u64 address_space_max;
    s32 chain_budget;             // block chaining: direct calls left before the dispatcher
    u32 hook_lo;                  // hooked guest addresses (RecompHooks), never chained to
    u32 hook_span;
    u32 chain_reserved;
    const volatile u8* hook_bits;
};
static_assert(offsetof(A32HostMemView, read_counter) == 48);
static_assert(offsetof(A32HostMemView, page_entries) == 56);
static_assert(offsetof(A32HostMemView, address_space_max) == 88);
static_assert(offsetof(A32HostMemView, chain_budget) == 96);
static_assert(sizeof(A32HostMemView) == 120);

// Static branches a block may follow directly before control returns to the dispatcher (which
// checks interrupts and runs the hooks). user/recomp_chain_off disables chaining (A/B tests).
constexpr s32 kChainBudget = 64;

constexpr u32 kNoPendingSvc = 0xFFFFFFFFu; // A32_NO_SVC
constexpr u32 kHaltUnhandled = 2;          // A32_HALT_UNHANDLED
// JIT instructions to single-step before leaving an uncovered region to run on the JIT.
constexpr u32 kMaxFallbackSteps = 4096;

// FPSCR <-> FPSR/FPCR, as in ArmDynarmic32, so a thread's FP state survives a
// switch between this backend, the JIT fallback and the kernel's ThreadContext.
std::pair<u32, u32> FpscrToFpsrFpcr(u32 fpscr) {
    const u32 fpsr = (fpscr & 0xf8000000) | (fpscr & 0x80) | (fpscr & 0x1f);
    const u32 fpcr = (fpscr & 0x7ff8000) | (fpscr & 0x1f00);
    return {fpsr, fpcr};
}
u32 FpsrFpcrToFpscr(u64 fpsr, u64 fpcr) {
    const auto [s, c] = FpscrToFpsrFpcr(static_cast<u32>(fpsr | fpcr));
    return s | c;
}

std::atomic<u64> g_miss{0}, g_unhandled{0}, g_thumb{0};

// Static blocks run and SVCs: counted per core instance (one writer each: the core's host thread), so the hot dispatch
// loop does a plain increment instead of a locked add on one shared cache line. Reports sum the live cores plus the
// counts of destroyed ones, so the totals mean what the old global counters meant (whole run, all cores).
struct HotCounters {
    alignas(64) std::atomic<u64> blocks{0};
    std::atomic<u64> svcs{0};
};
std::mutex g_hot_lock;
std::vector<const HotCounters*> g_hot_live;
u64 g_hot_retired_blocks{0}, g_hot_retired_svcs{0};
inline void Bump(std::atomic<u64>& c) {
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // single writer: no lock prefix
}
void HotRegister(const HotCounters* c) {
    std::scoped_lock lk{g_hot_lock};
    g_hot_live.push_back(c);
}
void HotRetire(const HotCounters* c) {
    std::scoped_lock lk{g_hot_lock};
    g_hot_live.erase(std::remove(g_hot_live.begin(), g_hot_live.end(), c), g_hot_live.end());
    g_hot_retired_blocks += c->blocks.load(std::memory_order_relaxed);
    g_hot_retired_svcs += c->svcs.load(std::memory_order_relaxed);
}
u64 TotalBlocks() {
    std::scoped_lock lk{g_hot_lock};
    u64 n = g_hot_retired_blocks;
    for (const HotCounters* c : g_hot_live) n += c->blocks.load(std::memory_order_relaxed);
    return n;
}
u64 TotalSvcs() {
    std::scoped_lock lk{g_hot_lock};
    u64 n = g_hot_retired_svcs;
    for (const HotCounters* c : g_hot_live) n += c->svcs.load(std::memory_order_relaxed);
    return n;
}
// JIT fallback episodes: instructions single-stepped, episodes that stepped back onto covered
// code, and episodes handed to a full JIT run (long or remembered regions, or a halt).
std::atomic<u64> g_fb_steps{0}, g_fb_returned{0}, g_fb_full_runs{0};

// Guest profile: a sampler thread reads, about every millisecond, the block each core is
// running (r15 holds a block's own address while it runs; every block returns to the
// dispatcher). Samples are counted per 4 KB page; the report maps pages to modules. A core
// is published only while RunThread runs guest code (not while its thread waits in the
// kernel).
constexpr size_t kProfileCores = 8;
std::array<std::atomic<const u32*>, kProfileCores> g_profile_pc{};
std::atomic<u64> g_profile_ticks{0};
std::mutex g_profile_lock;
std::unordered_map<u32, u64> g_profile_pages;        // guarded by g_profile_lock
std::unordered_map<u32, u64> g_profile_blocks;       // guarded by g_profile_lock; by block pc
std::string g_host_snapshot;                         // busiest host thread table so far
double g_host_snapshot_cpu = -1;
std::array<u64, kProfileCores> g_profile_busy{};      // guarded by g_profile_lock

// The last few seconds of samples, one row per sampler tick (pc per core, 0 = not running
// guest code), for the hitch probe. Guarded by g_profile_lock.
struct ProfileTick {
    std::chrono::steady_clock::time_point at;
    std::array<u32, kProfileCores> pc;
};
constexpr size_t kProfileRing = 8192;
std::vector<ProfileTick> g_profile_ring(kProfileRing);
size_t g_profile_ring_next = 0;

void ProfileLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        g_profile_ticks.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lk{g_profile_lock};
        ProfileTick& row = g_profile_ring[g_profile_ring_next++ % kProfileRing];
        row.at = std::chrono::steady_clock::now();
        for (size_t c = 0; c < kProfileCores; ++c) {
            const u32* const p = g_profile_pc[c].load(std::memory_order_acquire);
            row.pc[c] = 0;
            if (p == nullptr) {
                continue;
            }
            const u32 pc = *static_cast<const volatile u32*>(p);
            row.pc[c] = pc;
            ++g_profile_busy[c];
            ++g_profile_pages[pc >> 12];
            ++g_profile_blocks[pc];
        }
    }
}

std::string ProbeDetail(double from_ms, double to_ms);
void FallbackCounters(u64 (&out)[4]);
void PresentProbe(u32 lr, u32 gtid);
void StartFrameWriter();
Core::System* g_probe_system = nullptr;

void StartProfiler() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread(ProfileLoop).detach();
        Kernel::StallRing::g_guest_probe = &ProbeDetail;
        Kernel::StallRing::g_fallback_counters = &FallbackCounters;
        Kernel::StallRing::g_present_probe = &PresentProbe;
        StartFrameWriter();
    });
}

/// Publishes a core's r15 to the sampler for the duration of a RunThread call.
struct ProfileScope {
    ProfileScope(size_t core, const u32* pc) : slot{core < kProfileCores ? core : kProfileCores} {
        if (slot < kProfileCores) {
            g_profile_pc[slot].store(pc, std::memory_order_release);
        }
    }
    ~ProfileScope() {
        if (slot < kProfileCores) {
            g_profile_pc[slot].store(nullptr, std::memory_order_release);
        }
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
    size_t slot;
};

// Everything that leaves recompiled code for the JIT is recorded per site, in the user dir:
//  - recomp_misses.txt: "<module index> <offset> <name>" for block misses (index = NSO load
//    order, the same order the base setter uses). The exporter reads it back on the next
//    export and adds those offsets as discovery roots, so every run that reaches new code
//    makes the next build more complete. Append-only across runs.
//  - recomp_unhandled.txt: "<module index> <offset> <name> <instruction word>" for each
//    instruction the lifter left to the JIT that a run actually executed. Deduplicated
//    across runs. Informational; the exporter doesn't read it.
//  - recomp_report.txt: this run only, rewritten every 30 s and at exit (the previous run's
//    is kept as recomp_report.prev.txt): totals plus every miss / unhandled site with hit
//    count, first-seen time and caller, most hits first. The caller is LR when the word
//    before it is a BL/BLX (the site was reached by a call), "-" otherwise (return,
//    indirect branch or fall-through: LR is unrelated then).
std::mutex g_miss_lock;
std::vector<std::pair<u64, std::string>> g_module_list; // load order

struct SiteStat {
    u64 hits{};
    u32 insn{};
    u32 lr{};
    double first_seen{};
};
std::map<u32, SiteStat> g_miss_sites, g_unhandled_sites;
std::set<std::pair<size_t, u32>> g_unhandled_listed; // already in recomp_unhandled.txt
bool g_unhandled_loaded{false};
std::chrono::steady_clock::time_point g_run_start{std::chrono::steady_clock::now()};
double g_last_report{0.0};
constexpr double kReportInterval = 30.0;

std::filesystem::path UserFile(const char* name) {
    return Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name;
}

double Uptime() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_run_start).count();
}

struct Location {
    size_t index;
    u32 offset;
    const std::string* name;
};
// Callers hold g_miss_lock and have checked that g_module_list isn't empty.
Location Locate(u32 addr) {
    size_t index = 0;
    for (size_t i = 0; i < g_module_list.size(); ++i) {
        if (g_module_list[i].first <= addr) {
            index = i;
        }
    }
    const auto& [base, name] = g_module_list[index];
    return {index, static_cast<u32>(addr - base), &name};
}

std::string Describe(u32 addr) {
    if (addr < g_module_list.front().first) {
        return fmt::format("{:08x}", addr);
    }
    const auto loc = Locate(addr);
    return fmt::format("[{}] {}+{:x}", loc.index, *loc.name, loc.offset);
}

void WriteReportLocked() {
    g_last_report = Uptime();
    std::ofstream out(UserFile("recomp_report.txt"), std::ios::trunc);
    if (!out) {
        return;
    }
    out << fmt::format("# a32recomp run report: this run only, rewritten every {:.0f} s and at exit\n",
                       kReportInterval);
    for (size_t i = 0; i < g_module_list.size(); ++i) {
        out << fmt::format("# module [{}] {} at {:#x}\n", i, g_module_list[i].second,
                           g_module_list[i].first);
    }
    out << fmt::format("# uptime {:.1f} s: {} static blocks, {} SVCs, JIT fallbacks: {} missing "
                       "block, {} unhandled instruction, {} Thumb\n",
                       g_last_report, TotalBlocks(), TotalSvcs(), g_miss.load(),
                       g_unhandled.load(), g_thumb.load());
    out << fmt::format("# fallback: {} JIT instructions stepped, {} returns to recompiled code, "
                       "{} full JIT runs\n",
                       g_fb_steps.load(), g_fb_returned.load(), g_fb_full_runs.load());

    const auto write_sites = [&](const char* title, const std::map<u32, SiteStat>& sites,
                                 bool with_insn) {
        std::vector<std::pair<u32, const SiteStat*>> sorted;
        for (const auto& [pc, s] : sites) {
            sorted.emplace_back(pc, &s);
        }
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const auto& a, const auto& b) { return a.second->hits > b.second->hits; });
        out << fmt::format("\n## {}: {} sites\n", title, sorted.size());
        out << (with_insn ? "# hits\tidx\toffset\tmodule\tinsn\tfirst_s\tcaller\n"
                          : "# hits\tidx\toffset\tmodule\tfirst_s\tcaller\n");
        for (const auto& [pc, s] : sorted) {
            const auto loc = Locate(pc);
            out << fmt::format("{}\t{}\t{:08x}\t{}\t", s->hits, loc.index, loc.offset, *loc.name);
            if (with_insn) {
                out << fmt::format("{:08x}\t", s->insn);
            }
            out << fmt::format("{:.1f}\t{}\n", s->first_seen, s->lr ? Describe(s->lr) : "-");
        }
    };
    write_sites("unhandled instructions (JIT ran from here)", g_unhandled_sites, true);
    write_sites("missing blocks (JIT ran from here)", g_miss_sites, false);

    // Guest profile (see ProfileLoop) and host threads.
    std::unordered_map<u32, u64> pages;
    std::unordered_map<u32, u64> blocks;
    std::array<u64, kProfileCores> busy{};
    {
        std::scoped_lock lk{g_profile_lock};
        pages = g_profile_pages;
        blocks = g_profile_blocks;
        busy = g_profile_busy;
    }
    const u64 ticks = g_profile_ticks.load(std::memory_order_relaxed);
    u64 total = 0;
    std::map<size_t, u64> by_module;
    u64 outside = 0;
    std::vector<std::pair<u64, u32>> top;
    for (const auto& [page, n] : pages) {
        total += n;
        const u32 addr = page << 12;
        if (addr < g_module_list.front().first) {
            outside += n;
        } else {
            by_module[Locate(addr).index] += n;
        }
        top.emplace_back(n, page);
    }
    std::sort(top.rbegin(), top.rend());
    out << fmt::format("\n## guest profile: {} samples over {} sampler ticks; per core, ticks spent "
                       "running guest code:",
                       total, ticks);
    for (size_t c = 0; c < kProfileCores; ++c) {
        if (busy[c] != 0) {
            out << fmt::format(" core{} {:.1f}%", c, ticks ? 100.0 * busy[c] / ticks : 0.0);
        }
    }
    out << "\n";
    const auto pct = [&](u64 n) { return total ? 100.0 * n / total : 0.0; };
    for (const auto& [index, n] : by_module) {
        out << fmt::format("{:6.2f}%  [{}] {}\n", pct(n), index, g_module_list[index].second);
    }
    if (outside != 0) {
        out << fmt::format("{:6.2f}%  outside the modules\n", pct(outside));
    }
    out << "# hottest 4 KB pages: samples\t%\tidx\tpage offset\tmodule\n";
    for (size_t i = 0; i < std::min<size_t>(top.size(), 60); ++i) {
        const u32 addr = top[i].second << 12;
        if (addr < g_module_list.front().first) {
            out << fmt::format("{}\t{:.2f}\t-\t{:08x}\t-\n", top[i].first, pct(top[i].first), addr);
            continue;
        }
        const auto loc = Locate(addr);
        out << fmt::format("{}\t{:.2f}\t{}\t{:08x}\t{}\n", top[i].first, pct(top[i].first),
                           loc.index, loc.offset, *loc.name);
    }
    std::vector<std::pair<u64, u32>> top_blocks;
    top_blocks.reserve(blocks.size());
    for (const auto& [pc, n] : blocks) {
        top_blocks.emplace_back(n, pc);
    }
    std::sort(top_blocks.rbegin(), top_blocks.rend());
    out << fmt::format("# hottest blocks ({} distinct): samples\t%\tidx\toffset\tmodule\n", blocks.size());
    for (size_t i = 0; i < std::min<size_t>(top_blocks.size(), 200); ++i) {
        const u32 pc = top_blocks[i].second;
        if (pc < g_module_list.front().first) {
            continue;
        }
        const auto loc = Locate(pc);
        out << fmt::format("{}\t{:.2f}\t{}\t{:08x}\t{}\n", top_blocks[i].first, pct(top_blocks[i].first),
                           loc.index, loc.offset, *loc.name);
    }
    // At exit the GPU and CPU threads have already ended: keep the snapshot that saw the
    // most CPU time (the latest one taken while everything still ran).
    double host_cpu = 0;
    std::string host = HostThreads::Describe(g_last_report, 32, &host_cpu);
    if (host_cpu >= g_host_snapshot_cpu) {
        g_host_snapshot = std::move(host);
        g_host_snapshot_cpu = host_cpu;
    }
    out << "\n## host threads (latest snapshot with every thread alive)\n" << g_host_snapshot;
}

// FsMonitor::HitchProbe: per-core share of the span spent in guest code and the hottest
// blocks, e.g. "c0 98% c1 3% c2 41% | 120 1:bcf68c 40 3:3f9ac4".
std::string ProbeSpan(std::chrono::steady_clock::time_point from,
                      std::chrono::steady_clock::time_point to) {
    std::array<u64, kProfileCores> busy{};
    u64 ticks = 0;
    std::unordered_map<u32, u64> blocks;
    {
        std::scoped_lock lk{g_profile_lock};
        for (const ProfileTick& row : g_profile_ring) {
            if (row.at < from || row.at > to) {
                continue;
            }
            ++ticks;
            for (size_t c = 0; c < kProfileCores; ++c) {
                if (row.pc[c] != 0) {
                    ++busy[c];
                    ++blocks[row.pc[c]];
                }
            }
        }
    }
    if (ticks == 0) {
        return "no samples";
    }
    std::string out;
    for (size_t c = 0; c < kProfileCores; ++c) {
        if (busy[c] != 0) {
            out += fmt::format("c{} {:.0f}% ", c, 100.0 * busy[c] / ticks);
        }
    }
    std::vector<std::pair<u64, u32>> top;
    for (const auto& [pc, n] : blocks) {
        top.emplace_back(n, pc);
    }
    std::sort(top.rbegin(), top.rend());
    out += "|";
    std::scoped_lock lk{g_miss_lock};
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
        const u32 pc = top[i].second;
        if (g_module_list.empty() || pc < g_module_list.front().first) {
            out += fmt::format(" {} {:08x}", top[i].first, pc);
            continue;
        }
        const auto loc = Locate(pc);
        out += fmt::format(" {} {}:{:x}", top[i].first, loc.index, loc.offset);
    }
    return out;
}

std::string BlockName(u32 pc) {
    std::scoped_lock lk{g_miss_lock};
    if (g_module_list.empty() || pc < g_module_list.front().first) {
        return fmt::format("{:08x}", pc);
    }
    const auto loc = Locate(pc);
    return fmt::format("{}:{:x}", loc.index, loc.offset);
}

// Wiggle-hitch probe: what each core ran in [from, to] (steady_clock ms), from the 1 ms profile
// ring. Verdict: a handful of blocks covering 90% of the samples is a tight loop (spin-wait),
// many blocks is real work. Also the top block per 10 ms bin as a timeline.
std::string ProbeDetail(double from_ms, double to_ms) {
    using Clock = std::chrono::steady_clock;
    const auto tp = [](double ms) {
        return Clock::time_point{std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(ms))};
    };
    const auto from = tp(from_ms), to = tp(to_ms);
    std::array<u64, kProfileCores> busy{};
    std::array<std::unordered_map<u32, u64>, kProfileCores> blocks;
    std::array<std::map<int, std::unordered_map<u32, u64>>, kProfileCores> bins;
    u64 ticks = 0;
    {
        std::scoped_lock lk{g_profile_lock};
        for (const ProfileTick& row : g_profile_ring) {
            if (row.at < from || row.at > to) {
                continue;
            }
            ++ticks;
            const int bin = static_cast<int>(
                std::chrono::duration<double, std::milli>(row.at - from).count() / 10.0);
            for (size_t c = 0; c < kProfileCores; ++c) {
                if (row.pc[c] != 0) {
                    ++busy[c];
                    ++blocks[c][row.pc[c]];
                    ++bins[c][bin][row.pc[c]];
                }
            }
        }
    }
    if (ticks == 0) {
        return "guest profile: no samples\n";
    }
    static u32 gaps_seen = 0;
    static std::map<std::string, u32> recurring;
    ++gaps_seen;
    std::string out = fmt::format("guest profile: {} samples (1 ms)\n", ticks);
    for (size_t c = 0; c < kProfileCores; ++c) {
        if (busy[c] == 0) {
            continue;
        }
        std::vector<std::pair<u64, u32>> top;
        for (const auto& [pc, n] : blocks[c]) {
            top.emplace_back(n, pc);
        }
        std::sort(top.rbegin(), top.rend());
        u64 cum = 0;
        size_t cover90 = 0;
        for (; cover90 < top.size() && cum * 10 < busy[c] * 9; ++cover90) {
            cum += top[cover90].first;
        }
        out += fmt::format(" core {}: in guest code {:.0f}% ({} samples), {} distinct blocks, {} blocks cover 90% -> {}\n   top:",
                           c, 100.0 * busy[c] / ticks, busy[c], top.size(), cover90,
                           cover90 <= 12 && busy[c] >= 20 ? "TIGHT LOOP / SPIN-WAIT" : "spread: real work");
        for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
            out += fmt::format(" {}x{}", top[i].first, BlockName(top[i].second));
        }
        {
            out += " | recurring across gaps:";
            for (size_t i = 0; i < std::min<size_t>(top.size(), 3); ++i) {
                const std::string nm = BlockName(top[i].second);
                out += fmt::format(" {} in {}/{}", nm, ++recurring[nm], gaps_seen);
            }
        }
        out += "\n   10 ms bins (top block):";
        for (const auto& [bin, m] : bins[c]) {
            u32 best = 0;
            u64 bn = 0, tot = 0;
            for (const auto& [pc, n] : m) {
                tot += n;
                if (n > bn) {
                    bn = n;
                    best = pc;
                }
            }
            out += fmt::format(" [{}:{}/{} {}]", bin * 10, bn, tot, BlockName(best));
        }
        out += "\n";
    }
    return out;
}

// ---- Run M: per-present frame probe (identical in every render mode) ----------------------------
// One line per nvnQueuePresentTexture call into user\nvn_frames.txt: wall time and guest-visible
// tick since the previous present, the guest thread and caller, the hottest core 0 blocks since the
// previous present, and what the guest could see from suyu's GPU thread in that span (host1x
// syncpoint increments, report/semaphore writes, nvhost event waits), each with its offset from this
// present call. A gap frame (> 60 ms) also gets the previous four frame times and its top 10 blocks.
std::mutex g_frame_lock;
std::string g_frame_buf;

void StartFrameWriter() {
    std::thread([] {
        bool first = true;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::string out;
            {
                std::scoped_lock lk{g_frame_lock};
                out.swap(g_frame_buf);
            }
            if (out.empty()) {
                continue;
            }
            std::ofstream f(UserFile("nvn_frames.txt"), first ? std::ios::trunc : std::ios::app);
            first = false;
            f << out;
        }
    }).detach();
}

std::string TopBlocks(double from_ms, double to_ms, size_t core, size_t n, u64& busy, u64& total) {
    std::unordered_map<u32, u32> blocks;
    busy = total = 0;
    {
        std::scoped_lock lk{g_profile_lock};
        for (size_t k = 0; k < kProfileRing; ++k) {
            const ProfileTick& row =
                g_profile_ring[(g_profile_ring_next + kProfileRing - 1 - k) % kProfileRing];
            const double at =
                std::chrono::duration<double, std::milli>(row.at.time_since_epoch()).count();
            if (at < from_ms) {
                break;
            }
            if (at > to_ms) {
                continue;
            }
            ++total;
            if (row.pc[core] != 0) {
                ++busy;
                ++blocks[row.pc[core]];
            }
        }
    }
    std::vector<std::pair<u32, u32>> top;
    for (const auto& [pc, c] : blocks) {
        top.emplace_back(c, pc);
    }
    std::sort(top.rbegin(), top.rend());
    std::string out;
    for (size_t i = 0; i < std::min(top.size(), n); ++i) {
        out += fmt::format(" {}x{}", top[i].first, BlockName(top[i].second));
    }
    return out;
}

void PresentProbe(u32 lr, u32 gtid) {
    namespace SR = Kernel::StallRing;
    static std::mutex m;
    static double prev_ms = 0;
    static u64 prev_tick = 0;
    static u64 frame = 0;
    static std::array<double, 8> hist{};
    static std::array<double, 8> hist_tick{};
    std::scoped_lock lk{m};
    const double now = SR::NowMs();
    const u64 tick = g_probe_system ? g_probe_system->CoreTiming().GetClockTicks() : 0;
    ++frame;
    SR::Add(SR::Present, gtid, 0, 0, lr, static_cast<u32>(frame), now, now);
    if (prev_ms == 0) {
        prev_ms = now;
        prev_tick = tick;
        std::scoped_lock fl{g_frame_lock};
        g_frame_buf += fmt::format("# nvn_vulkan mode {} (NvnVulkanMode value of this run)\n",
                                   static_cast<int>(GameSettings::NvnVulkanMode()));
        return;
    }
    const double dt = now - prev_ms;
    const double dtick = static_cast<double>(tick - prev_tick) / 19200.0; // CNTFREQ 19.2 MHz
    u64 busy = 0, total = 0;
    const std::string top = TopBlocks(prev_ms, now, 0, dt > 60.0 ? 10 : 5, busy, total);
    std::string line = fmt::format("F{} t{:.3f} dt {:.2f} tick {:.2f} g{} lr {} | c0 {}/{}:{}", frame, now, dt,
                                   dtick, gtid, BlockName(lr), busy, total, top);

    // Guest-visible events since the previous present.
    struct Agg {
        u32 n = 0, nz = 0, maxv = 0;
        double first = 1e18, last = -1e18; // offsets from this present call (ms, <= 0)
        u32 lastv = 0;
    };
    std::map<u32, Agg> hs, gs, rp;
    std::array<Agg, 5> cw{};
    double stall_ms = 0;
    const u64 head = SR::g_index.load(std::memory_order_acquire);
    for (u64 k = 0; k < std::min<u64>(head, SR::kSize); ++k) {
        const SR::Event& e = SR::g_events[(head - 1 - k) & (SR::kSize - 1)];
        if (e.t1 < prev_ms - 20.0) {
            break;
        }
        if (e.t1 < prev_ms || e.t1 > now) {
            continue;
        }
        const double off = e.t1 - now;
        const auto put = [&](Agg& a, u32 v) {
            ++a.n;
            a.nz += v != 0;
            a.maxv = std::max(a.maxv, v);
            a.first = std::min(a.first, off);
            if (off > a.last) {
                a.last = off;
                a.lastv = v;
            }
        };
        switch (e.kind) {
        case SR::Sync:
            put(e.core == 0 ? hs[e.a] : gs[e.a], e.b);
            break;
        case SR::Report:
            put(rp[e.a], e.b);
            break;
        case SR::CtrlWait:
            if (e.core < 5) {
                put(cw[e.core], e.a);
                if (e.core == 3) {
                    stall_ms += e.t1 - e.t0;
                }
            }
            break;
        default:
            break;
        }
    }
    const auto dump = [&](const char* name, const std::map<u32, Agg>& mp, bool vals) {
        if (mp.empty()) {
            return;
        }
        line += fmt::format(" | {}", name);
        size_t shown = 0;
        for (const auto& [id, a] : mp) {
            if (shown++ >= 4) {
                line += " ...";
                break;
            }
            line += fmt::format(" id{}:n{}", id, a.n);
            if (vals) {
                line += fmt::format(" nz{} max{} last{}", a.nz, a.maxv, a.lastv);
            } else {
                line += fmt::format(" v{}", a.lastv);
            }
            line += fmt::format(" [{:.1f}..{:.1f}ms]", a.first, a.last);
        }
    };
    dump("hsync", hs, false);
    dump("gsync", gs, false);
    dump("report", rp, true);
    {
        static const char* const kNames[5] = {"signalled", "registered", "timeout0", "stall", "fired"};
        std::string w;
        for (size_t i = 0; i < 5; ++i) {
            if (cw[i].n) {
                w += fmt::format(" {}:{}[{:.1f}..{:.1f}ms]", kNames[i], cw[i].n, cw[i].first, cw[i].last);
            }
        }
        if (!w.empty()) {
            line += " | ctrlwait" + w;
        }
        if (stall_ms > 0) {
            line += fmt::format(" STALLWAIT {:.2f} ms", stall_ms);
        }
    }
    if (dt > 60.0) {
        std::array<double, 8> sorted = hist;
        std::sort(sorted.begin(), sorted.end());
        const double med = sorted[4];
        const double p1 = hist[(frame - 1) % 8];
        line += fmt::format(
            " <<< GAP prev4 wall {:.1f} {:.1f} {:.1f} {:.1f} tick {:.1f} {:.1f} {:.1f} {:.1f} median8 {:.1f} -> {}",
            hist[(frame - 1) % 8], hist[(frame - 2) % 8], hist[(frame - 3) % 8], hist[(frame - 4) % 8],
            hist_tick[(frame - 1) % 8], hist_tick[(frame - 2) % 8], hist_tick[(frame - 3) % 8],
            hist_tick[(frame - 4) % 8], med, p1 > 1.5 * med ? "FOLLOWS A LONGER FRAME" : "OUT OF NOWHERE");
    }
    hist[frame % 8] = dt;
    hist_tick[frame % 8] = dtick;
    prev_ms = now;
    prev_tick = tick;
    line += "\n";
    std::scoped_lock fl{g_frame_lock};
    g_frame_buf += line;
}

void FallbackCounters(u64 (&out)[4]) {
    out[0] = g_miss.load(std::memory_order_relaxed);
    out[1] = g_fb_steps.load(std::memory_order_relaxed);
    out[2] = g_fb_returned.load(std::memory_order_relaxed);
    out[3] = g_fb_full_runs.load(std::memory_order_relaxed);
}

void MaybeWriteReportLocked() {
    if (Uptime() - g_last_report >= kReportInterval) {
        WriteReportLocked();
    }
}

// LR if the ARM instruction before it is a BL/BLX, i.e. the current code was reached by a
// call from there; 0 otherwise.
u32 CallSite(Core::Memory::Memory& memory, u32 lr) {
    if ((lr & 3) != 0 || lr < 4 || !memory.IsValidVirtualAddress(lr - 4)) {
        return 0;
    }
    const u32 w = memory.Read32(lr - 4);
    const bool bl = (w >> 28) != 0xF && (w & 0x0F000000u) == 0x0B000000u;
    const bool blx_imm = (w & 0xFE000000u) == 0xFA000000u;
    const bool blx_reg = (w >> 28) != 0xF && (w & 0x0FFFFFF0u) == 0x012FFF30u;
    return bl || blx_imm || blx_reg ? lr : 0;
}

void RecordMiss(u32 pc, u32 lr) {
    std::scoped_lock lk{g_miss_lock};
    if (g_module_list.empty()) {
        return;
    }
    auto [it, is_new] = g_miss_sites.try_emplace(pc);
    ++it->second.hits;
    if (is_new) {
        it->second.lr = lr;
        it->second.first_seen = Uptime();
        const auto loc = Locate(pc);
        std::ofstream out(UserFile("recomp_misses.txt"), std::ios::app);
        if (out) {
            out << fmt::format("{} {:08x} {}\n", loc.index, loc.offset, *loc.name);
        }
    }
    MaybeWriteReportLocked();
}

void RecordUnhandled(u32 pc, u32 insn, u32 lr) {
    std::scoped_lock lk{g_miss_lock};
    if (g_module_list.empty()) {
        return;
    }
    auto [it, is_new] = g_unhandled_sites.try_emplace(pc);
    ++it->second.hits;
    if (is_new) {
        it->second.insn = insn;
        it->second.lr = lr;
        it->second.first_seen = Uptime();
        const auto loc = Locate(pc);
        LOG_WARNING(Core_ARM, "recomp32: unhandled instruction {:08x} at {}; running on JIT", insn,
                    Describe(pc));
        if (!g_unhandled_loaded) {
            g_unhandled_loaded = true;
            std::ifstream in(UserFile("recomp_unhandled.txt"));
            size_t index;
            std::string offset;
            std::string rest;
            while (in >> index >> offset && std::getline(in, rest)) {
                g_unhandled_listed.emplace(
                    index, static_cast<u32>(std::strtoul(offset.c_str(), nullptr, 16)));
            }
        }
        if (g_unhandled_listed.emplace(loc.index, loc.offset).second) {
            std::ofstream out(UserFile("recomp_unhandled.txt"), std::ios::app);
            if (out) {
                out << fmt::format("{} {:08x} {} {:08x}\n", loc.index, loc.offset, *loc.name, insn);
            }
        }
    }
    MaybeWriteReportLocked();
}

// Misses and unhandled sites rewrite the report as they come; a run without any (and a run
// whose exit skips the destructor) still gets the periodic rewrite from here.
void StartReportTimer() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::scoped_lock lk{g_miss_lock};
                if (!g_module_list.empty()) {
                    MaybeWriteReportLocked();
                }
            }
        }).detach();
    });
}

} // namespace

struct ArmRecomp32::Impl {
    Impl(System& system_, RecompLookupFn lookup_) : system{system_}, lookup{lookup_} {
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.pending_svc = kNoPendingSvc;
        bridge.user = this;
        bridge.load = &Impl::HostLoad;
        bridge.store = &Impl::HostStore;
        bridge.excl_load = &Impl::HostExclusiveLoad;
        bridge.excl_store = &Impl::HostExclusiveStore;
        bridge.clear_excl = &Impl::HostClearExclusive;
        bridge.read_counter = &Impl::HostReadCounter;
        bridge.page_entries = nullptr; // until RefreshPageTable: every access takes the callbacks
        ctx.host = &bridge;
        HotRegister(&hot);
    }
    ~Impl() {
        HotRetire(&hot);
    }

    static u64 HostLoad(void* user, u64 va, u32 size) {
        auto& memory = static_cast<Impl*>(user)->system.ApplicationMemory();
        switch (size) {
        case 1: return memory.Read8(va);
        case 2: return memory.Read16(va);
        case 4: return memory.Read32(va);
        default: return memory.Read64(va);
        }
    }
    static void HostStore(void* user, u64 va, u32 size, u64 value) {
        auto& memory = static_cast<Impl*>(user)->system.ApplicationMemory();
        switch (size) {
        case 1: memory.Write8(va, static_cast<u8>(value)); break;
        case 2: memory.Write16(va, static_cast<u16>(value)); break;
        case 4: memory.Write32(va, static_cast<u32>(value)); break;
        default: memory.Write64(va, value); break;
        }
    }
    // Exclusives go through the kernel's per-core monitor so recompiled code,
    // the JIT fallback and other cores contend on the same reservations.
    static u64 HostExclusiveLoad(void* user, u64 va, u32 size) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            return HostLoad(user, va, size);
        }
        auto& m = *self->exclusive_monitor;
        switch (size) {
        case 1: return m.ExclusiveRead8(self->core_index, va);
        case 2: return m.ExclusiveRead16(self->core_index, va);
        case 4: return m.ExclusiveRead32(self->core_index, va);
        default: return m.ExclusiveRead64(self->core_index, va);
        }
    }
    /// 0 on success, 1 when the reservation was lost (STREX's status sense).
    static u32 HostExclusiveStore(void* user, u64 va, u32 size, u64 value) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            HostStore(user, va, size, value);
            return 0;
        }
        auto& m = *self->exclusive_monitor;
        bool ok = false;
        switch (size) {
        case 1: ok = m.ExclusiveWrite8(self->core_index, va, static_cast<u8>(value)); break;
        case 2: ok = m.ExclusiveWrite16(self->core_index, va, static_cast<u16>(value)); break;
        case 4: ok = m.ExclusiveWrite32(self->core_index, va, static_cast<u32>(value)); break;
        default: ok = m.ExclusiveWrite64(self->core_index, va, value); break;
        }
        return ok ? 0u : 1u;
    }
    static void HostClearExclusive(void* user) {
        auto* self = static_cast<Impl*>(user);
        if (self->exclusive_monitor) {
            self->exclusive_monitor->ClearExclusive(self->core_index);
        }
    }
    /// Same clock the JIT's CNTPCT reads, so a thread moving between engines
    /// sees one monotonic counter.
    static u64 HostReadCounter(void* user) {
        return static_cast<Impl*>(user)->system.CoreTiming().GetClockTicks();
    }

    void RefreshPageTable() {
        const auto view = system.ApplicationMemory().GetPageTableView();
        bridge.page_entries = view.entries;
        bridge.page_entry_stride = view.entry_stride;
        bridge.page_bits = view.page_bits;
        bridge.pointer_mask = view.pointer_mask;
        bridge.address_space_max = view.address_space_max;
    }

    /// Tell every recompiled image where the loader put its module. Addresses
    /// in the image are module-relative (A32_PC); this is what makes them real.
    void RegisterBases(Kernel::KThread* thread) {
        if (bases_registered) {
            return;
        }
        bases_registered = true;
        g_probe_system = &system;
        auto* process = thread->GetOwnerProcess();
        const auto setter = GetRecompBaseSetter();
        if (!process || !setter) {
            return;
        }
        size_t index = 0;
        const auto modules = FindModules(process);
        for (const auto& [base, name] : modules) {
            LOG_INFO(Core_ARM, "recomp32: module [{}] {} at {:#x}", index, name, base);
            setter(index++, name.c_str(), base);
        }
        if (!modules.empty()) {
            // Hooked addresses live in the loaded modules: cover them all (+64 MB past the last
            // module's base for its own size).
            const u32 lo = static_cast<u32>(modules.begin()->first);
            const u32 hi = static_cast<u32>(modules.rbegin()->first) + 0x4000000u;
            RecompHooks::Init(lo, hi - lo);
            static const bool chain_off = [] {
                std::error_code ec;
                const bool off = std::filesystem::exists(UserFile("recomp_chain_off"), ec);
                LOG_INFO(Core_ARM, "recomp32: block chaining {}", off ? "off" : "on");
                return off;
            }();
            chain_budget = chain_off ? 0 : kChainBudget;
        }
        {
            std::scoped_lock lk{g_miss_lock};
            if (g_module_list.empty() && !modules.empty()) {
                g_module_list.assign(modules.begin(), modules.end());
                std::error_code ec;
                std::filesystem::rename(UserFile("recomp_report.txt"),
                                        UserFile("recomp_report.prev.txt"), ec);
                g_run_start = std::chrono::steady_clock::now();
                WriteReportLocked();
                StartReportTimer();
                if (GameSettings::DiagnosticsEnabled()) {
                    FsMonitor::SetHitchProbe(&ProbeSpan);
                    Kernel::StallRing::g_guest_probe = &ProbeDetail;
                    Kernel::StallRing::g_fallback_counters = &FallbackCounters;
                    FsMonitor::Enable();
                }
            }
        }
        if (GameSettings::DiagnosticsEnabled()) {
            NvnTrace::Get().Attach(system, modules);
            TimingWatch::Attach(system, modules);
        } else {
            if (GameSettings::NativeNvn() || GameSettings::NvnVulkanMode() != 0) {
                NvnTrace::Get().Attach(system, modules, true);
            }
            LOG_INFO(Core_ARM, "recomp32: diagnostics off (game_settings.ini [Debug])");
        }
        GameSettings::Start(system, modules);
        ModHost::Start(system, modules, lookup);
        bridge.hook_lo = RecompHooks::Lo();
        bridge.hook_span = RecompHooks::Span();
        bridge.hook_bits = RecompHooks::Bits();
    }

    /// CallGuest: runs the JIT instruction by instruction from ctx until the pc is on a block
    /// again or at MODHOST_CALL_RETURN_PC. False (ctx holds the state reached) on a system call
    /// or after kCallJitSteps instructions.
    bool StepCallOnJit() {
        constexpr u32 kCallJitSteps = 2'000'000;
        if (!self->EnterFallback()) {
            return false;
        }
        in_fallback = false; // a mod call never leaves the JIT armed for the game's own run
        ctx.halted = 0;
        Kernel::Svc::ThreadContext tctx{};
        self->GetContext(tctx);
        fallback->SetContext(tctx);
        fallback->SetTpidrroEl0(ctx.tpidruro);
        HaltReason hr = HaltReason::StepThread;
        for (u32 n = 0; n < kCallJitSteps && hr == HaltReason::StepThread; ++n) {
            hr = n == 0 ? fallback->StepThread(nullptr) : fallback->StepInstruction();
            fallback->GetContext(tctx);
            if (hr != HaltReason::StepThread) {
                break;
            }
            const bool thumb = (tctx.pstate & 0x20) != 0;
            if (static_cast<u32>(tctx.pc) == MODHOST_CALL_RETURN_PC ||
                (!thumb && lookup(static_cast<u32>(tctx.pc)))) {
                break;
            }
            if (n + 1 == kCallJitSteps) {
                hr = HaltReason::BreakLoop; // runaway
            }
        }
        self->SetContext(tctx);
        return hr == HaltReason::StepThread;
    }

    /// Runs a guest function to its return for the mod loader, on this core and guest thread.
    /// Recompiled blocks run natively; code without blocks (a module the mod loaded, an
    /// untranslated instruction) runs on the JIT. A system call or a runaway call fails the
    /// call (the registers are restored; memory it wrote stays written). Hooks don't fire.
    bool CallGuest(u32 function, const u32* args, u32 count, const float* fargs, u32 fcount,
                   u32* out_r, double* out_d0) {
        constexpr u32 kMaxDepth = 4;
        constexpr u64 kMaxBlocks = 50'000'000; // a runaway call fails instead of hanging
        if (call_depth >= kMaxDepth || (function & 1) != 0) {
            return false;
        }
        const A32ContextView saved = ctx;
        for (u32 i = 0; i < 16; ++i) {
            ctx.ext.s[i] = i < fcount ? std::bit_cast<u32>(fargs[i]) : 0;
        }
        u32 sp = ctx.r[13];
        if (count > 4) {
            sp = (sp - (count - 4) * 4) & ~7u; // AAPCS: 8-byte aligned at the call
            auto& memory = system.ApplicationMemory();
            for (u32 i = 4; i < count; ++i) {
                memory.Write32(sp + (i - 4) * 4, args[i]);
            }
        }
        for (u32 i = 0; i < 4; ++i) {
            ctx.r[i] = i < count ? args[i] : 0;
        }
        ctx.r[13] = sp & ~7u;
        ctx.r[14] = MODHOST_CALL_RETURN_PC;
        ctx.r[15] = function;
        ctx.thumb = 0;
        ++call_depth;
        bool ok = false;
        for (u64 n = 0; n < kMaxBlocks; ++n) {
            const u32 pc = ctx.r[15];
            if (pc == MODHOST_CALL_RETURN_PC) {
                ok = true;
                break;
            }
            RecompBlockFn block{};
            if (!ctx.thumb) {
                auto& cached = lookup_cache[(pc >> 2) & (kLookupCache - 1)];
                block = cached.pc == pc ? cached.fn : lookup(pc);
                if (block) {
                    cached = {pc, block};
                    bridge.chain_budget = chain_budget;
                    block(&ctx);
                    if (ctx.halted == 0 && ctx.pending_svc == kNoPendingSvc) {
                        continue;
                    }
                    if (ctx.pending_svc != kNoPendingSvc) {
                        break;
                    }
                    // untranslated instruction: ctx.r[15] is at it, the JIT runs it below
                }
            }
            // No block (code without a recompiled image, Thumb) or an untranslated instruction:
            // single-step the JIT until the pc is back on a block or the call returns.
            if (!StepCallOnJit()) {
                break;
            }
        }
        --call_depth;
        if (ok) {
            out_r[0] = ctx.r[0];
            out_r[1] = ctx.r[1];
            *out_d0 = std::bit_cast<double>(ctx.ext.d[0]);
        } else {
            LOG_WARNING(Core_ARM, "[mods] guest call to {:#x} failed at {:#x} ({})", function,
                        ctx.r[15],
                        ctx.pending_svc != kNoPendingSvc ? "system call"
                        : ctx.halted != 0               ? "untranslated instruction"
                                                        : "no block / runaway");
        }
        ctx = saved;
        return ok;
    }

    static bool CallGuestThunk(void* self, u32 function, const u32* args, u32 count,
                               const float* fargs, u32 fcount, u32* out_r, double* out_d0) {
        return static_cast<Impl*>(self)->CallGuest(function, args, count, fargs, fcount, out_r,
                                                   out_d0);
    }

    System& system;
    RecompLookupFn lookup{};
    ModHost::GuestCaller mod_caller{this, &Impl::CallGuestThunk};
    ArmRecomp32* self{};
    u32 call_depth{0};
    A32ContextView ctx{};
    A32HostMemView bridge{};
    std::atomic<bool> interrupted{false};
    bool bases_registered{false};
    s32 chain_budget{0}; // refilled into bridge.chain_budget before every block
    // Recently dispatched blocks (direct-mapped by address): returns and indirect branches land
    // on few targets, and each lookup otherwise binary-searches the module's whole block table.
    // Recompiled blocks never change, so entries never go stale.
    static constexpr size_t kLookupCache = 4096;
    struct CachedBlock {
        u32 pc{~0u};
        RecompBlockFn fn{};
    };
    std::array<CachedBlock, kLookupCache> lookup_cache{};

    Kernel::KProcess* owner_process{};
    DynarmicExclusiveMonitor* exclusive_monitor{};
    std::size_t core_index{};
    bool uses_wall_clock{};
    std::unique_ptr<ArmDynarmic32> fallback{};
    bool in_fallback{false};
    // Fallback entry PCs whose uncovered region outlasted kMaxFallbackSteps: run them on the
    // JIT directly next time instead of stepping again.
    std::unordered_set<u32> long_regions;
    bool fallback_unavailable{false};
    HotCounters hot{}; // static blocks / SVCs run on this core (summed by TotalBlocks / TotalSvcs)
};

ArmRecomp32::ArmRecomp32(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                         Kernel::KProcess* process, DynarmicExclusiveMonitor* exclusive_monitor,
                         std::size_t core_index)
    : ArmInterface{uses_wall_clock}, impl{std::make_unique<Impl>(system, lookup)} {
    impl->self = this;
    impl->owner_process = process;
    impl->exclusive_monitor = exclusive_monitor;
    impl->core_index = core_index;
    impl->uses_wall_clock = uses_wall_clock;
}

ArmRecomp32::~ArmRecomp32() {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        LOG_INFO(Core_ARM,
                 "recomp32 execution: {} static blocks, {} SVCs, JIT fallbacks: {} missing block, "
                 "{} unhandled instruction, {} Thumb",
                 TotalBlocks(), TotalSvcs(), g_miss.load(), g_unhandled.load(), g_thumb.load());
        {
            std::scoped_lock lk{g_miss_lock};
            if (!g_module_list.empty()) {
                WriteReportLocked();
            }
        }
        NvnTrace::Get().Flush(true);
        FsMonitor::Flush();
        TimingWatch::Flush();
        ModHost::Shutdown();
    }
}

bool ArmRecomp32::EnterFallback() {
    if (impl->fallback_unavailable) {
        return false;
    }
    if (!impl->fallback) {
        if (!impl->owner_process || !impl->exclusive_monitor) {
            impl->fallback_unavailable = true;
            return false;
        }
        impl->fallback = std::make_unique<ArmDynarmic32>(impl->system, impl->uses_wall_clock,
                                                         impl->owner_process,
                                                         *impl->exclusive_monitor, impl->core_index);
        LOG_WARNING(Core_ARM, "recomp32: created dynarmic A32 fallback");
    }
    impl->in_fallback = true;
    return true;
}

HaltReason ArmRecomp32::RunFallback(Kernel::KThread* thread, bool step) {
    // The recompiled context stays the source of truth: load the JIT from it,
    // run, and drain the JIT back, so every accessor on this interface works
    // the same whichever engine ran.
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;

    Kernel::Svc::ThreadContext tctx{};
    this->GetContext(tctx);
    impl->fallback->SetContext(tctx);
    impl->fallback->SetTpidrroEl0(impl->ctx.tpidruro);

    HaltReason hr{};
    if (step) {
        hr = impl->fallback->StepThread(thread);
    } else {
        // Single-step only until the PC is back on recompiled code. The JIT then runs just the
        // instructions the image lacks, and calls from there into covered code go through the
        // dispatcher (so the NVN trace sees them) instead of staying on the JIT until the next
        // SVC. A region still uncovered after kMaxFallbackSteps runs on the JIT until it halts.
        const u32 entry_pc = impl->ctx.r[15];
        hr = HaltReason::StepThread;
        for (u32 n = 0; hr == HaltReason::StepThread; ++n) {
            if (n == kMaxFallbackSteps || impl->long_regions.contains(entry_pc)) {
                impl->long_regions.insert(entry_pc);
                g_fb_full_runs.fetch_add(1, std::memory_order_relaxed);
                hr = impl->fallback->RunThread(thread);
                break;
            }
            g_fb_steps.fetch_add(1, std::memory_order_relaxed);
            hr = n == 0 ? impl->fallback->StepThread(thread) : impl->fallback->StepInstruction();
            impl->fallback->GetContext(tctx);
            const bool thumb = (tctx.pstate & 0x20) != 0;
            if (hr == HaltReason::StepThread && !thumb &&
                impl->lookup(static_cast<u32>(tctx.pc))) {
                g_fb_returned.fetch_add(1, std::memory_order_relaxed);
                break; // covered again: StepThread tells RunThread to carry on natively
            }
            if (hr == HaltReason::StepThread && impl->interrupted.load(std::memory_order_relaxed)) {
                hr = HaltReason::BreakLoop;
            }
        }
    }

    impl->fallback->GetContext(tctx);
    this->SetContext(tctx);
    if (True(hr & HaltReason::SupervisorCall)) {
        impl->ctx.pending_svc = impl->fallback->GetSvcNumber();
        Bump(impl->hot.svcs);
    }
    // Back to recompiled code as soon as the PC is covered again.
    if (!impl->ctx.thumb && impl->lookup && impl->lookup(impl->ctx.r[15])) {
        impl->in_fallback = false;
    }
    return hr;
}

HaltReason ArmRecomp32::RunThread(Kernel::KThread* thread) {
    if (!impl->lookup) {
        LOG_ERROR(Core_ARM, "recomp32: no recompiled image registered");
        return HaltReason::BreakLoop;
    }
    impl->RefreshPageTable();
    impl->RegisterBases(thread);
    if (Kernel::StallRing::On() || GameSettings::DiagnosticsEnabled()) StartProfiler(); // the stall dump needs the guest profile
    const ProfileScope profile_scope{impl->core_index, &impl->ctx.r[15]};
    ModHost::SetCaller(&impl->mod_caller);
    const struct ModCallerReset {
        ~ModCallerReset() {
            ModHost::SetCaller(nullptr);
        }
    } mod_caller_reset;

    // The kernel has serviced the SVC that parked us last time.
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;
    impl->interrupted.store(false, std::memory_order_relaxed);

    if (impl->in_fallback) {
        const HaltReason hr = RunFallback(thread, false);
        if (hr != HaltReason::StepThread || impl->in_fallback) {
            return hr;
        }
    }

    for (;;) {
        if (impl->interrupted.load(std::memory_order_relaxed)) {
            return HaltReason::BreakLoop;
        }
        RecompBlockFn block = nullptr;
        if (impl->ctx.thumb) {
            // Thumb code is not recompiled (MHGU's modules are ARM state).
            g_thumb.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (ModHost::OnDispatch(impl->ctx.r[15], reinterpret_cast<ModHostCpu*>(&impl->ctx),
                                  impl->ctx.tpidruro)) {
                continue; // the mod loader moved r15 (skipped call, or a hooked function returned)
            }
            TimingWatch::OnDispatch(impl->ctx.r[15], impl->ctx.r);
            GameSettings::OnDispatch(impl->ctx.r[15], impl->ctx.r);
            if (NvnTrace::Get().OnDispatch(impl->ctx.r[15], impl->ctx.r)) {
                continue; // a native replacement ran; r15 is the caller's return address
            }
            const u32 pc = impl->ctx.r[15];
            auto& cached = impl->lookup_cache[(pc >> 2) & (Impl::kLookupCache - 1)];
            if (cached.pc == pc) {
                block = cached.fn;
            } else {
                block = impl->lookup(pc);
                if (block) {
                    cached = {pc, block};
                }
            }
            if (!block) {
                g_miss.fetch_add(1, std::memory_order_relaxed);
                RecordMiss(impl->ctx.r[15],
                           CallSite(impl->system.ApplicationMemory(), impl->ctx.r[14]));
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                    LOG_WARNING(Core_ARM, "recomp32: no block at {:#x}; running on JIT",
                                impl->ctx.r[15]);
                }
            }
        }
        if (!block) {
            if (!EnterFallback()) {
                LOG_CRITICAL(Core_ARM, "recomp32: cannot run {:#x} and no JIT fallback",
                             impl->ctx.r[15]);
                return HaltReason::PrefetchAbort;
            }
            // StepThread with in_fallback cleared: the JIT stepped back onto covered code.
            if (const HaltReason hr = RunFallback(thread, false);
                hr != HaltReason::StepThread || impl->in_fallback) {
                return hr;
            }
            continue;
        }

        Bump(impl->hot.blocks);
        impl->bridge.chain_budget = impl->chain_budget;
        block(&impl->ctx);

        if (impl->ctx.halted == kHaltUnhandled) {
            // r15 is parked on the instruction the lifter could not translate.
            g_unhandled.fetch_add(1, std::memory_order_relaxed);
            impl->ctx.halted = 0;
            const u32 pc = impl->ctx.r[15];
            auto& memory = impl->system.ApplicationMemory();
            RecordUnhandled(pc, memory.Read32(pc), CallSite(memory, impl->ctx.r[14]));
            if (!EnterFallback()) {
                return HaltReason::PrefetchAbort;
            }
            // StepThread with in_fallback cleared: the JIT stepped back onto covered code.
            if (const HaltReason hr = RunFallback(thread, false);
                hr != HaltReason::StepThread || impl->in_fallback) {
                return hr;
            }
            continue;
        }
        if (impl->ctx.pending_svc != kNoPendingSvc) {
            Bump(impl->hot.svcs);
            impl->ctx.halted = 0;
            return HaltReason::SupervisorCall;
        }
    }
}

HaltReason ArmRecomp32::StepThread(Kernel::KThread* thread) {
    // Recompiled blocks have no per-instruction entry points; single-stepping
    // (debugger) goes through the JIT.
    if (!EnterFallback()) {
        return HaltReason::BreakLoop;
    }
    return RunFallback(thread, true);
}

void ArmRecomp32::ClearInstructionCache() {
    if (impl->fallback) {
        impl->fallback->ClearInstructionCache();
    }
}

void ArmRecomp32::InvalidateCacheRange(u64 addr, std::size_t size) {
    // Recompiled code is fixed at build time; only the fallback caches anything.
    if (impl->fallback) {
        impl->fallback->InvalidateCacheRange(addr, size);
    }
}

void ArmRecomp32::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    const auto& c = impl->ctx;
    ctx = {};
    for (size_t i = 0; i < 16; ++i) {
        ctx.r[i] = c.r[i];
    }
    ctx.fp = c.r[11];
    ctx.sp = c.r[13];
    ctx.lr = c.r[14];
    ctx.pc = c.r[15];
    ctx.pstate = (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28) | (c.q << 27) | (c.ge << 16) |
                 (c.thumb << 5) | 0x10u; // user mode
    static_assert(sizeof(c.ext) <= sizeof(ctx.v));
    std::memcpy(ctx.v.data(), &c.ext, sizeof(c.ext));
    const auto [fpsr, fpcr] = FpscrToFpsrFpcr(c.fpscr);
    ctx.fpsr = fpsr;
    ctx.fpcr = fpcr;
    ctx.tpidr = c.tpidrurw;
}

void ArmRecomp32::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    auto& c = impl->ctx;
    for (size_t i = 0; i < 16; ++i) {
        c.r[i] = static_cast<u32>(ctx.r[i]);
    }
    const u32 p = ctx.pstate;
    c.n = (p >> 31) & 1;
    c.z = (p >> 30) & 1;
    c.c = (p >> 29) & 1;
    c.v = (p >> 28) & 1;
    c.q = (p >> 27) & 1;
    c.ge = (p >> 16) & 0xF;
    c.thumb = (p >> 5) & 1;
    std::memcpy(&c.ext, ctx.v.data(), sizeof(c.ext));
    c.fpscr = FpsrFpcrToFpscr(ctx.fpsr, ctx.fpcr);
    // Only the guest-owned thread pointer travels in ThreadContext; TPIDRURO is
    // republished through SetTpidrroEl0 on every switch-in.
    c.tpidrurw = static_cast<u32>(ctx.tpidr);
}

void ArmRecomp32::SetTpidrroEl0(u64 value) {
    impl->ctx.tpidruro = static_cast<u32>(value);
}

void ArmRecomp32::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; ++i) {
        args[i] = impl->ctx.r[i];
    }
}

void ArmRecomp32::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; ++i) {
        impl->ctx.r[i] = static_cast<u32>(args[i]);
    }
}

u32 ArmRecomp32::GetSvcNumber() const {
    return impl->ctx.pending_svc;
}

void ArmRecomp32::SignalInterrupt(Kernel::KThread* thread) {
    impl->interrupted.store(true, std::memory_order_relaxed);
    if (impl->fallback) {
        impl->fallback->SignalInterrupt(thread);
    }
}

const Kernel::DebugWatchpoint* ArmRecomp32::HaltedWatchpoint() const {
    return nullptr;
}

void ArmRecomp32::RewindBreakpointInstruction() {}

} // namespace Core
