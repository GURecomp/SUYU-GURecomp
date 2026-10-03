// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <string>

#include "common/common_types.h"

namespace Core {

class System;
namespace Memory {
class Memory;
}

/**
 * NVN call census for 32-bit recompiled titles.
 *
 * Games reach NVN through a single export, nvnBootstrapLoader(name), and then
 * nvn*GetProcAddress(obj, name); every other entry point is a function pointer
 * handed back by those. The recompiled dispatcher sees every block it enters,
 * so watching those lookups (name in, pointer out) names each NVN entry point,
 * and counting dispatches to them gives which functions the game uses and how
 * often - the map for replacing the NVN layer.
 *
 * Output: <user dir>/nvn_trace.txt (calls per entry point) and nvn_args.txt (the distinct
 * argument tuples each one received, for the state and shader census), rewritten every few
 * seconds.
 * Disable with the file <user dir>/nvn_trace_off.
 */
/// Address of an exported function in a loaded module (MOD0 -> .dynamic), 0 if absent.
u32 FindModuleExport(Memory::Memory& mem, u64 base, const std::string& name);

class NvnTrace {
public:
    static NvnTrace& Get();

    /// Called once with the process's modules (base -> name) before guest code runs.
    void Attach(System& system, const std::map<u64, std::string>& modules);

    /// Native stand-in for an NVN entry point (G3): gets r0-r3/sp as the call starts and
    /// leaves the return value in r0 (r0:r1 for 64-bit results). The guest function is
    /// skipped; the dispatcher continues at the caller's return address.
    using Replacement = void (*)(u32* regs, Memory::Memory& mem);

    /// Registers a replacement for an entry point by name, before guest code runs.
    void Replace(const std::string& name, Replacement fn);

    /// Hot path: called for every block the dispatcher is about to run. Returns true when a
    /// replacement ran instead: regs[15] then holds the address to continue at.
    bool OnDispatch(u32 pc, u32* regs) {
        if (!enabled.load(std::memory_order_acquire)) {
            return false;
        }
        if (pending_returns.load(std::memory_order_relaxed) != 0) {
            OnMaybeReturn(pc, regs);
        }
        const u32 off = pc - watch_lo;
        if (off < watch_span && (bits[off >> 5].load(std::memory_order_relaxed) >> ((off >> 2) & 7) & 1)) {
            return OnHookedEntry(pc, regs);
        }
        return false;
    }

    /// final: the process is exiting (always writes every report).
    void Flush(bool final = false);

private:
    NvnTrace() = default;
    void OnMaybeReturn(u32 pc, const u32* regs);
    bool OnHookedEntry(u32 pc, u32* regs);
    void Watch(u32 addr);

    std::atomic<bool> enabled{false};
    bool attach_tried{false};
    u32 watch_lo{};
    u32 watch_span{};
    std::atomic<u8>* bits{}; // one bit per word of the watched range
    std::atomic<int> pending_returns{0};
    System* system{};
};

} // namespace Core
