// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

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

/// game_settings.ini [Display] console_mode = handheld: the game runs as on the Switch's own
/// screen (1280x720 render). Read once at start, before the game boots.
bool StartHandheld();

/// The desktop's mode for fps = auto / resolution = auto, given by the frontend where the OS
/// can't be asked directly (Linux: SDL knows it once the window exists). Windows asks the OS.
void SetDesktopMode(u32 width, u32 height, u32 refresh_hz);

/// Any game_settings.ini value ("" when absent), and writing one back (frontend options such as
/// [Multiplayer]; the ini is created with its defaults first if needed).
std::string Value(const std::string& section, const std::string& key);
void SetValue(const std::string& section, const std::string& key, const std::string& value);

/// Default [Mods] loader, relative to mods/<title id>/: Forge PC's library for this platform.
#ifdef _WIN32
inline constexpr const char* kDefaultModLoader = "Forge/forge.dll";
#else
inline constexpr const char* kDefaultModLoader = "Forge/forge.so";
#endif

/// One option as game_settings.ini documents it (the game menu's Settings tab lists these).
struct OptionInfo {
    std::string section;
    std::string key;
    std::string default_value;
    std::string comment; // the ini comment without its "; " prefixes
};
std::vector<OptionInfo> Options();

/// Applies a changed option to the running game where that is possible (frame rate, the
/// title's FPS); false = it takes effect at the next start. The value is saved separately
/// (SetValue).
bool ApplyLive(const std::string& section, const std::string& key, const std::string& value);

/// game_settings.ini [Display] show_fps: the frame rate in the window title.
bool ShowFps();

/// game_settings.ini [Debug] diagnostics: the run reports beyond recomp_report.txt's misses
/// (profiler, NVN census/trace, timing watch, file monitor). On unless set to false.
bool DiagnosticsEnabled();
/// [Debug] gpu_diagnostics (default off): reports of the replacement renderer only (nvn_prof.txt timing
/// buckets, NVN call recording samples). Independent of `diagnostics`, which is for every graphics mode.
bool GpuDiagnosticsEnabled();
/// [Debug] probes (default off): the wiggle-hitch probes (stall ring, SVC/HLE/hook timing, buffer-queue log, run K/L/M frame lines).
bool ProbesEnabled();

/// game_settings.ini [Debug] fence_thread_sleep: the GPU fencing thread polls the GPU with short
/// sleeps instead of blocking in the Vulkan driver (which spins a core). Off unless set to true.
bool FenceThreadSleep();

/// game_settings.ini [Debug] native_nvn: G3c native NVN entry points replace the guest driver's.
/// Off unless set to true.
bool NativeNvn();
/// Width the game is made to render at ([Display] resolution + native_render), 0 = the game's own.
u32 RenderWidth();

/// game_settings.ini [Debug] nvn_vulkan: 0 off, 1 record (NVN recorder only), 2 shadow (recorder + our
/// Vulkan renderer beside the guest driver).
int NvnVulkanMode();
/// [Debug] vulkan_validation = true turns the Vulkan validation layer and its log messages on.
bool VulkanValidation();
/// [Debug] nvn_vk_debug bit flags for the NVN Vulkan renderer: 1 no blend, 2 full write mask, 4 no alpha test, 8 no depth test, 16 no cull.
int NvnVkDebug();
/// [Debug] suyu_render = false (only with nvn_vulkan = present): suyu's own renderer skips draws and clears, since the
/// native renderer draws the frame. Default true.
bool SuyuRender();
/// [Debug] present_nodrain = true (present mode, TEST): Present does not wait for the GPU; the blit for frame N is
/// published one present later. Default false.
bool PresentNoDrain();

/// [Debug] pres_ring = N (present mode): number of images in the ring suyu's compositor reads. Default 3.
int PresRing();
/// [Debug] fence_hold = true (present mode): guest-visible fence completion waits for our replay of that submit.
bool FenceHold();
/// [Debug] present_fifo = true (present mode): suyu's compositor takes our finished copies in order, one per composite
/// (repeat the last one if none is ready; drop the oldest beyond 3), instead of "the latest copy". Default false.
bool PresentFifo();
/// [Debug] order_probe = true (present mode): F8 logs 60 presents' order and image signature to nvn_present.txt.
bool OrderProbe();

/// game_settings.ini [Debug] probe_pcs: nnSdk offsets (hex, comma separated) the trace observes.
std::string ProbePcs();

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
extern std::atomic<u32> g_main_base; // guest address of the main module (0 until known)
extern std::atomic<u32> g_motion_blur_pc; // [Debug] motion_blur = probe|force: uMotionBlurFilter render function
extern std::atomic<u32> g_vp_probe_pc; // [Debug] probes: viewport setter, after its first call (main+0xBC1204)
void OnHookedEntry(u32 pc, u32* regs);

/// Hot path: every block the dispatcher is about to run.
inline void OnDispatch(u32 pc, u32* regs) {
    if (pc == g_aspect_pc.load(std::memory_order_relaxed) ||
        pc == g_render_default_pc.load(std::memory_order_relaxed) ||
        pc == g_render_create_pc.load(std::memory_order_relaxed) ||
        pc == g_motion_blur_pc.load(std::memory_order_relaxed) ||
        pc == g_vp_probe_pc.load(std::memory_order_relaxed)) {
        OnHookedEntry(pc, regs);
    }
}

} // namespace GameSettings

} // namespace Core
