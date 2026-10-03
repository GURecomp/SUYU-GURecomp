// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <map>
#include <string>

#include "common/common_types.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/arm/recomp/mod_host_api.h"

namespace Core {

class System;

/**
 * Mod host: the exe's whole share of modding. Loads one installable loader DLL (Forge PC by
 * default: <exe dir>/mods/<title id>/Forge/forge.dll, game_settings.ini [Mods] loader) and
 * gives it the stable interface in mod_host_api.h: guest memory, block marks + a dispatch
 * callback, guest calls, the presented swapchain image, window input. Everything a mod does
 * lives in that DLL and in mod folders, so mods never need a re-export.
 *
 * Mods are runtime only (LayeredFS + plugins); the exporter never reads mods/.
 */
namespace ModHost {

/// Lets the loader call guest functions on the current core (set by the dispatcher per thread).
struct GuestCaller {
    void* impl;
    bool (*call)(void* impl, u32 function, const u32* args, u32 count, const float* fargs,
                 u32 fcount, u32* out_r, double* out_d0);
};

extern std::atomic<bool> g_active;
// RecompHooks' bitmap, cached at Start (it never moves).
extern u32 g_hook_lo;
extern u32 g_hook_span;
extern const volatile u8* g_hook_bits;

/// Once, with the process's modules (base -> name), before guest code runs: loads the loader.
void Start(System& system, const std::map<u64, std::string>& modules, RecompLookupFn lookup);

/// The dispatcher's GuestCaller for this host thread (null while it isn't running guest code).
void SetCaller(const GuestCaller* caller);

bool OnDispatchSlow(u32 pc, ModHostCpu* cpu, u32 thread_key);

/// Hot path, every dispatched block. True if r15 changed: the dispatcher starts over there.
inline bool OnDispatch(u32 pc, ModHostCpu* cpu, u32 thread_key) {
    if (!g_active.load(std::memory_order_relaxed)) {
        return false;
    }
    if (pc != MODHOST_RETURN_PC) {
        const u32 off = pc - g_hook_lo;
        if (off >= g_hook_span || (g_hook_bits[off >> 5] & (1u << ((off >> 2) & 7))) == 0) {
            return false;
        }
    }
    return OnDispatchSlow(pc, cpu, thread_key);
}

/// From the presenter, between the copy into the swapchain image and its final barrier.
/// True if the loader drew: the image is then in COLOR_ATTACHMENT_OPTIMAL.
bool OnPresent(const ModHostPresent& present);

/// From the window's event loop. True if the loader consumed the event.
bool OnInput(const ModHostInput& input);

/// Once per finished game frame (PerfStats::EndSystemFrame).
void OnFrame();

/// At exit.
void Shutdown();

/// Set by the loader: a menu has the controller (gate the game's pad input) / wants text input.
bool InputCaptured();
bool TextInputWanted();

} // namespace ModHost

} // namespace Core
