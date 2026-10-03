// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <map>
#include <string>

#include "common/common_types.h"

namespace Core {

class System;

/**
 * Timing watch (G4-0): counts the guest's calls to the SDK's sleep, event-wait, tick and
 * vsync-event functions, per caller (LR) and per argument (sleep length, event, timeout), to
 * find how the game paces its frames. The functions are found by their exported names in
 * nnSdk. Output: <user dir>/timing_watch.txt, rewritten every 10 s and at exit.
 */
namespace TimingWatch {

/// Resolves the watched functions in the loaded modules (base -> name), before guest code runs.
void Attach(System& system, const std::map<u64, std::string>& modules);

void OnEntry(u32 pc, const u32* regs);

extern std::atomic<u32> g_lo;
extern std::atomic<u32> g_span;

/// Hot path: every block the dispatcher is about to run.
inline void OnDispatch(u32 pc, const u32* regs) {
    if (pc - g_lo.load(std::memory_order_relaxed) < g_span.load(std::memory_order_relaxed)) {
        OnEntry(pc, regs);
    }
}

void Flush();

} // namespace TimingWatch

} // namespace Core
