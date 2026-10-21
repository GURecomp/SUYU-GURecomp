// SPDX-License-Identifier: GPL-2.0-or-later
// Wiggle-hitch diagnostics: lock-free ring of guest thread activity (run episodes, SVCs, context
// switches, host heartbeat gaps). Dumped by the renderer when a present-to-present gap is long.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "common/common_types.h"
#include "core/arm/recomp/game_settings.h"

#ifdef _WIN32
// Not <windows.h>: its DrawState macro breaks unrelated headers.
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void* thread, unsigned long long* cycles);
#endif

namespace Kernel::StallRing {

/// [Debug] probes = true. Everything in this file is a no-op (and costs a branch) when off.
inline bool On() {
    static const bool on = Core::GameSettings::ProbesEnabled();
    return on;
}

// Run M kinds, the values the guest can see come from suyu's GPU thread:
//   Sync    host1x syncpoint increment (core 0 = host/completion side, 1 = guest side; a = id, b = new value)
//   CtrlWait nvhost-ctrl event wait (core = mode: 0 already signalled, 1 registered wait, 2 timeout 0,
//           3 stall-wait (t0..t1), 4 wait signalled; a = syncpoint id, b = threshold)
//   Report  query/semaphore result written to guest memory (core bit0 = fence, bit1 = timestamp;
//           a = query stream id, b = value written)
//   Present nvnQueuePresentTexture entered by the guest (a = caller LR, b = frame number)
enum Kind : u8 {
    Run = 1, Svc = 2, Switch = 3, Heartbeat = 4, Hook = 5, LockWait = 6,
    Sync = 7, CtrlWait = 8, Report = 9, Present = 10
};

struct Event {
    double t0;
    double t1;
    u32 gtid;
    u32 a;
    u32 b;
    u16 prio;
    u8 core;
    u8 kind;
};

constexpr u32 kSize = 1u << 18;

inline std::array<Event, kSize> g_events{};
inline std::atomic<u64> g_index{0};

inline double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// NowMs() with probes on, 0 with probes off: for timestamps that only feed Add() (no clock read when off).
inline double NowIfOn() {
    return On() ? NowMs() : 0.0;
}

/// CPU cycles this host thread has consumed (0 off Windows). Compared with wall time it tells
/// running from descheduled / page-faulting.
inline u64 ThreadCycles() {
#ifdef _WIN32
    if (!On()) return 0;
    unsigned long long c = 0;
    QueryThreadCycleTime(::GetCurrentThread(), &c);
    return c;
#else
    return 0;
#endif
}

inline void Add(u8 kind, u32 gtid, u8 core, u16 prio, u32 a, u32 b, double t0, double t1) {
    if (!On()) return;
    const u64 i = g_index.fetch_add(1, std::memory_order_relaxed);
    g_events[i & (kSize - 1)] = Event{t0, t1, gtid, a, b, prio, core, kind};
}

// Native NVN hook statistics per (hook function, guest thread). Cumulative; the renderer
// snapshots them at every present and diffs them for a long gap.
struct HookStat {
    std::atomic<u64> key{0}; // fn | gtid << 48
    std::atomic<u64> calls{0};
    std::atomic<u64> ns{0};
};
constexpr u32 kHookSlots = 2048;
inline std::array<HookStat, kHookSlots> g_hooks{};

/// Resolves a hook function pointer to the NVN entry names it serves (set by NvnTrace).
inline std::string (*g_name_of)(uintptr_t fn) = nullptr;
/// Per-span guest profile text (set by the recompiler backend): busy cores, hottest blocks.
inline std::string (*g_guest_probe)(double from_ms, double to_ms) = nullptr;
/// Cumulative JIT-fallback counters: missing-block entries, steps, returns, full runs.
inline void (*g_fallback_counters)(u64 (&out)[4]) = nullptr;
/// Run M: called by the guest thread on every nvnQueuePresentTexture (caller LR, guest thread id).
inline void (*g_present_probe)(u32 lr, u32 gtid) = nullptr;
/// Run N: every syncpoint increment (guest = 1 guest side, 0 host side) with its time, for the present-path log.
inline void (*g_sync_probe)(u32 id, u32 value, u32 guest, double t) = nullptr;

inline void HookDone(uintptr_t fn, u32 gtid, u8 core, double t0, double t1) {
    if (!On()) return;
    const u64 key = static_cast<u64>(fn) | (static_cast<u64>(gtid & 0xffff) << 48);
    u32 h = static_cast<u32>((key >> 4) * 2654435761u ^ (key >> 40)) & (kHookSlots - 1);
    for (u32 n = 0; n < kHookSlots; ++n, h = (h + 1) & (kHookSlots - 1)) {
        u64 cur = g_hooks[h].key.load(std::memory_order_acquire);
        if (cur == 0 && g_hooks[h].key.compare_exchange_strong(cur, key)) {
            cur = key;
        }
        if (cur == key) {
            g_hooks[h].calls.fetch_add(1, std::memory_order_relaxed);
            g_hooks[h].ns.fetch_add(static_cast<u64>((t1 - t0) * 1e6), std::memory_order_relaxed);
            break;
        }
    }
    if (t1 - t0 > 1.0) {
        Add(Hook, gtid, core, 0, static_cast<u32>(fn), static_cast<u32>(static_cast<u64>(fn) >> 32), t0,
            t1);
    }
}

// Guest thread running on this host thread (set by PhysicalCore::RunThread; 0 on other threads).
inline thread_local u32 t_gtid = 0;

// Mutex wait statistics for the locks the guest thread takes inside our hooks.
enum LockId : u8 { LockRec = 0, LockShadow = 1, LockRenderer = 2, LockNote = 3, LockDump = 4, kLockCount = 5 };
inline constexpr const char* kLockNames[kLockCount] = {"nvn_rec s_lock", "nvn_shadow s_lock",
                                                       "renderer lock", "note_lock", "dump_lock"};
struct LockStat {
    std::atomic<u64> calls{0};
    std::atomic<u64> wait_ns{0};
    std::atomic<u64> hold_ns{0};
};
inline std::array<LockStat, kLockCount> g_locks{};

/// std::scoped_lock that times the wait (and the hold) and logs waits over 0.5 ms.
class TimedGuard {
public:
    TimedGuard(std::mutex& m, LockId id) : mtx{m}, lock_id{id} {
        if (!On()) {
            mtx.lock();
            return;
        }
        const double t0 = NowMs();
        mtx.lock();
        acquired = NowMs();
        auto& st = g_locks[id];
        st.calls.fetch_add(1, std::memory_order_relaxed);
        st.wait_ns.fetch_add(static_cast<u64>((acquired - t0) * 1e6), std::memory_order_relaxed);
        if (acquired - t0 > 0.5) {
            Add(LockWait, t_gtid, 0, 0, id, static_cast<u32>((acquired - t0) * 1000.0), t0, acquired);
        }
    }
    ~TimedGuard() {
        if (!On()) {
            mtx.unlock();
            return;
        }
        g_locks[lock_id].hold_ns.fetch_add(static_cast<u64>((NowMs() - acquired) * 1e6),
                                           std::memory_order_relaxed);
        mtx.unlock();
    }
    TimedGuard(const TimedGuard&) = delete;
    TimedGuard& operator=(const TimedGuard&) = delete;

private:
    std::mutex& mtx;
    LockId lock_id;
    double acquired{};
};

/// Host heartbeat: a thread that sleeps 1 ms in a loop and records any wake-up that is very late
/// (a host-wide stall: allocation, page faults, the process being descheduled).
inline void StartHeartbeat() {
    if (!On()) return;
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    std::thread([] {
        for (;;) {
            const double t0 = NowMs();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const double t1 = NowMs();
            if (t1 - t0 > 25.0) {
                Add(Heartbeat, 0, 0, 0, static_cast<u32>((t1 - t0) * 10.0), 0, t0, t1);
            }
        }
    }).detach();
}

} // namespace Kernel::StallRing
