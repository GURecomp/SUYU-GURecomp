// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>

#include "core/arm/arm_interface.h"
#include "core/arm/recomp/arm_recomp.h"

namespace Kernel {
class KProcess;
}

namespace Core {

class System;
class DynarmicExclusiveMonitor;

/**
 * CPU backend that executes statically recompiled AArch32 (ARM state) code.
 *
 * The 32-bit counterpart of ArmRecomp. Blocks come from the a32recomp lifter
 * (tools/a32recomp, core/recompiler/a32_to_c.h) and operate on an A32Context;
 * the image registers itself through the same SetRecompLookup /
 * SetRecompBaseSetter hooks as an AArch64 image, so suyu-cmd's module
 * registration is shared.
 *
 * Execution model, as in ArmRecomp: blocks run back to back from a dispatch
 * loop. A block that reaches an SVC parks with pending_svc set and returns;
 * this reports HaltReason::SupervisorCall and the kernel services the call
 * through GetSvcArguments/SetSvcArguments. Anything the static image cannot
 * run - a PC with no block, an instruction the lifter did not translate, or
 * Thumb code - is handed to a dynarmic A32 JIT built on first use, with the
 * guest state marshalled across through ThreadContext.
 */
class ArmRecomp32 final : public ArmInterface {
public:
    ArmRecomp32(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                Kernel::KProcess* process, DynarmicExclusiveMonitor* exclusive_monitor,
                std::size_t core_index);
    ~ArmRecomp32() override;

    HaltReason RunThread(Kernel::KThread* thread) override;
    HaltReason StepThread(Kernel::KThread* thread) override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, std::size_t size) override;

    Architecture GetArchitecture() const override {
        return Architecture::AArch32;
    }

    void GetContext(Kernel::Svc::ThreadContext& ctx) const override;
    void SetContext(const Kernel::Svc::ThreadContext& ctx) override;
    void SetTpidrroEl0(u64 value) override;

    void GetSvcArguments(std::span<uint64_t, 8> args) const override;
    void SetSvcArguments(std::span<const uint64_t, 8> args) override;
    u32 GetSvcNumber() const override;

    void SignalInterrupt(Kernel::KThread* thread) override;

    const Kernel::DebugWatchpoint* HaltedWatchpoint() const override;
    void RewindBreakpointInstruction() override;

private:
    bool EnterFallback();
    HaltReason RunFallback(Kernel::KThread* thread, bool step);

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Core
