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
 * Game options for recompiled exports, read at startup from game_settings.ini beside the exported
 * exe (created with the defaults and comments when missing). Every option is applied at run time,
 * so changing one needs a restart, never a re-export. Defaults are the original game's behaviour.
 *
 * MHGU 1.4.0 (0100770008DD8000):
 *   [Display] fps: MT Framework's target frame rate, a float in the engine object that the global
 *   at main+0x18A6210 points to, at +0x243C (30.0 in the original game; the engine derives its
 *   timing from it). Checked every few presented frames and kept at the configured value, writing
 *   only while the field holds a plausible frame rate; at 30 it is never written. Above 60 the
 *   emulated display refreshes faster too (DisplayRateScale), or presents would stay at 60.
 *   fps = auto follows the measured frame rate (drop to it when missed, +5 per second while met)
 *   up to the monitor's refresh rate.
 *   [Display] resolution (auto = desktop mode, or WxH): exact view shape, output stretched to
 *   the window; [Graphics] native_render also replaces the docked 1920x1080 render size that
 *   main+0xBBCD34 stores at [r5] when main+0xBBBDE0 (render target creation) is entered.
 *   [Display] aspect: the camera projection function main+0x1C358 reads the aspect ratio from
 *   [r1+0x38] (then near/far at +0x30/+0x34, FOV in degrees at +0x3C). On entry, a 16:9 value
 *   there is replaced with the configured one; other values (render-to-texture views) are left
 *   alone. suyu's output aspect is set to match.
 *   The HUD stays stretched on wide screens; the fix is the standalone HudFix Forge plugin.
 *
 * Locations came from public 1.4.0 patches used as references only (nothing of them is shipped):
 * 60 FPS by masagrator, the 90/120 FPS cheat by minderrx, the aspect ratio and resolution
 * patches by Fl4sh9174 (16:10 update by TLin-Y).
 */
namespace GameSettings {

/// game_settings.ini [Display] fullscreen = true (read before the window opens).
bool StartFullscreen();

/// Any game_settings.ini value ("" when absent), and writing one back (frontend options such as
/// [Multiplayer]; the ini is created with its defaults first if needed).
std::string Value(const std::string& section, const std::string& key);
void SetValue(const std::string& section, const std::string& key, const std::string& value);

/// game_settings.ini [Debug] diagnostics: the run reports beyond recomp_report.txt's misses
/// (profiler, NVN census/trace, timing watch, file monitor). On unless set to false.
bool DiagnosticsEnabled();

/// Called once with the process's modules (base -> name) before guest code runs.
void Start(System& system, const std::map<u64, std::string>& modules);

/// Every presented frame (applies the options that live in guest memory).
void OnFrame();

/// How much faster than 60 Hz the emulated display refreshes (1 unless fps > 60).
float DisplayRateScale();

// Hooked guest entries (0 = unused): camera projection (aspect, far plane), and the render size
// setup (main+0xBBCD34 records where the docked size goes, main+0xBBBDE0 replaces it).
extern std::atomic<u32> g_aspect_pc;
extern std::atomic<u32> g_render_default_pc;
extern std::atomic<u32> g_render_create_pc;
void OnHookedEntry(u32 pc, u32* regs);

/// Hot path: every block the dispatcher is about to run.
inline void OnDispatch(u32 pc, u32* regs) {
    if (pc == g_aspect_pc.load(std::memory_order_relaxed) ||
        pc == g_render_default_pc.load(std::memory_order_relaxed) ||
        pc == g_render_create_pc.load(std::memory_order_relaxed)) {
        OnHookedEntry(pc, regs);
    }
}

} // namespace GameSettings

} // namespace Core
