// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "core/arm/recomp/nvn_trace.h"
#include "core/arm/recomp/recomp_hooks.h"
#include "core/arm/recomp/timing_watch.h"
#include "core/core.h"
#include "core/memory.h"

namespace Core::TimingWatch {

std::atomic<u32> g_lo{0};
std::atomic<u32> g_span{0};

namespace {

enum class Arg {
    None,      // count only
    Ns01,      // TimeSpan / s64 nanoseconds in r0:r1
    Ptr,       // object pointer in r0
    PtrNs23,   // object pointer in r0, TimeSpan in r2:r3
    VsyncOut,  // r0 = the SystemEventType the vsync event is written to
};

struct Watched {
    const char* name;  // exported (mangled) name in nnSdk
    const char* label; // for the report
    Arg arg;
    u32 addr{};
    u64 calls{};
    std::unordered_map<u32, u64> callers;
    std::unordered_map<std::string, u64> args;
};

std::vector<Watched> g_fns{
    {"_ZN2nn2os11SleepThreadENS_8TimeSpanE", "nn::os::SleepThread(ns)", Arg::Ns01},
    {"nnosSleepThread", "nnosSleepThread(ns)", Arg::Ns01},
    {"_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE", "nn::os::WaitSystemEvent(event)",
     Arg::Ptr},
    {"_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE",
     "nn::os::TryWaitSystemEvent(event)", Arg::Ptr},
    {"_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE",
     "nn::os::TimedWaitSystemEvent(event, ns)", Arg::PtrNs23},
    {"nnosWaitSystemEvent", "nnosWaitSystemEvent(event)", Arg::Ptr},
    {"nnosTimedWaitSystemEvent", "nnosTimedWaitSystemEvent(event, ns)", Arg::PtrNs23},
    {"_ZN2nn2os9WaitEventEPNS0_9EventTypeE", "nn::os::WaitEvent(event)", Arg::Ptr},
    {"_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE",
     "nn::os::TimedWaitEvent(event, ns)", Arg::PtrNs23},
    {"nnosTimedWaitEvent", "nnosTimedWaitEvent(event, ns)", Arg::PtrNs23},
    {"_ZN2nn2os13GetSystemTickEv", "nn::os::GetSystemTick()", Arg::None},
    {"nnosGetSystemTick", "nnosGetSystemTick()", Arg::None},
    {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",
     "nn::vi::GetDisplayVsyncEvent(out event, display)", Arg::VsyncOut},
};

std::mutex g_lock;
std::map<u64, std::string> g_modules;
u32 g_vsync_event{};
const auto g_start = std::chrono::steady_clock::now();

std::string Where(u32 addr) {
    const std::pair<const u64, std::string>* best = nullptr;
    for (const auto& m : g_modules) {
        if (m.first <= addr) {
            best = &m;
        }
    }
    if (best == nullptr) {
        return fmt::format("{:08x}", addr);
    }
    return fmt::format("{}+{:x}", best->second, addr - static_cast<u32>(best->first));
}

std::string Ns(u32 lo, u32 hi) {
    const s64 ns = static_cast<s64>((u64{hi} << 32) | lo);
    if (ns < 0) {
        return "infinite";
    }
    return fmt::format("{:.2f} ms", ns / 1e6);
}

std::string Event(u32 ptr) {
    return ptr == g_vsync_event && ptr != 0 ? fmt::format("{:08x} (vsync)", ptr)
                                            : fmt::format("{:08x}", ptr);
}

void WriteLocked() {
    const auto path =
        Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "timing_watch.txt";
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    const double up =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    out << fmt::format("# timing watch (G4-0): SDK sleep / wait / tick / vsync calls, {:.1f} s\n",
                       up);
    for (const Watched& w : g_fns) {
        if (w.addr == 0) {
            out << fmt::format("\n## {}: not found\n", w.label);
            continue;
        }
        out << fmt::format("\n## {} at {}: {} calls, {:.1f}/s\n", w.label, Where(w.addr), w.calls,
                           up > 0 ? w.calls / up : 0.0);
        std::vector<std::pair<u64, u32>> callers;
        for (const auto& [lr, n] : w.callers) {
            callers.emplace_back(n, lr);
        }
        std::sort(callers.rbegin(), callers.rend());
        for (size_t i = 0; i < std::min<size_t>(callers.size(), 12); ++i) {
            out << fmt::format("  caller {:>10} {:.1f}/s  {}\n", callers[i].first,
                               up > 0 ? callers[i].first / up : 0.0, Where(callers[i].second));
        }
        std::vector<std::pair<u64, std::string>> args;
        for (const auto& [a, n] : w.args) {
            args.emplace_back(n, a);
        }
        std::sort(args.rbegin(), args.rend());
        for (size_t i = 0; i < std::min<size_t>(args.size(), 16); ++i) {
            out << fmt::format("  args   {:>10} {:.1f}/s  {}\n", args[i].first,
                               up > 0 ? args[i].first / up : 0.0, args[i].second);
        }
    }
}

} // namespace

void Attach(System& system, const std::map<u64, std::string>& modules) {
    static std::atomic<bool> attached{false};
    if (attached.exchange(true)) {
        return; // every core registers the process; once is enough
    }
    auto& mem = system.ApplicationMemory();
    u32 lo = ~0u;
    u32 hi = 0;
    {
        std::scoped_lock lk{g_lock};
        g_modules = modules;
        for (Watched& w : g_fns) {
            for (const auto& [base, name] : modules) {
                if (const u32 addr = FindModuleExport(mem, base, w.name); addr != 0) {
                    w.addr = addr;
                    break;
                }
            }
            if (w.addr != 0) {
                RecompHooks::Mark(w.addr);
                lo = std::min(lo, w.addr);
                hi = std::max(hi, w.addr);
            }
        }
    }
    if (hi == 0) {
        LOG_WARNING(Core_ARM, "timing watch: no SDK timing functions found");
        return;
    }
    LOG_INFO(Core_ARM, "timing watch: {:#x}-{:#x}", lo, hi);
    g_lo.store(lo);
    g_span.store(hi - lo + 1);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::scoped_lock lk{g_lock};
            WriteLocked();
        }
    }).detach();
}

void OnEntry(u32 pc, const u32* regs) {
    // The range also holds unrelated hot code: match first (addresses are fixed after Attach),
    // lock only for a watched entry.
    for (Watched& w : g_fns) {
        if (w.addr != pc) {
            continue;
        }
        std::scoped_lock lk{g_lock};
        ++w.calls;
        ++w.callers[regs[14]];
        switch (w.arg) {
        case Arg::None:
            break;
        case Arg::Ns01:
            ++w.args[Ns(regs[0], regs[1])];
            break;
        case Arg::Ptr:
            ++w.args[Event(regs[0])];
            break;
        case Arg::PtrNs23:
            ++w.args[Event(regs[0]) + ", " + Ns(regs[2], regs[3])];
            break;
        case Arg::VsyncOut:
            g_vsync_event = regs[0];
            ++w.args[fmt::format("{:08x}", regs[0])];
            break;
        }
        return;
    }
}

void Flush() {
    if (g_span.load() == 0) {
        return;
    }
    std::scoped_lock lk{g_lock};
    WriteLocked();
}

} // namespace Core::TimingWatch
