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
/// Current window size in pixels (called on window creation and resize events only).
void SetWindowSize(u32 width, u32 height);

/// Any game_settings.ini value ("" when absent), and writing one back (frontend options such as
/// [Multiplayer]; the ini is created with its defaults first if needed).
std::string Value(const std::string& section, const std::string& key);
void SetValue(const std::string& section, const std::string& key, const std::string& value);
/// Bumped by every SetValue (in-game menu): lets a caller cache a Value() and re-read it only after a change.
u64 SettingsGeneration();

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
    bool advanced{};     // Kind::Advanced: listed only while [Menu] show_advanced (Advanced mode) is on
};
std::vector<OptionInfo> Options();

/// Applies a changed option to the running game where that is possible (frame rate, the
/// title's FPS); false = it takes effect at the next start. The value is saved separately
/// (SetValue).
bool ApplyLive(const std::string& section, const std::string& key, const std::string& value);

/// ---- Upscaler (menu section "Upscaler"): the Upscaler / Quality / Render scale rows ------------------------------------
/// Upscaler choice as the menu shows it: Off | FSR 3.1 | FSR 4. Stored truth stays [Upscaler] nvn_fsr (bool) and
/// nvn_fsr_backend (3 | 4); read from the ini on each call (not cached) so the menu follows edits.
enum class UpscalerChoice { Off, Fsr31, Fsr4 };
UpscalerChoice UpscalerChoiceNow();
/// Writes nvn_fsr / nvn_fsr_backend for the choice (restart needed: the renderer reads them at start).
void SetUpscalerChoice(UpscalerChoice choice);

/// [Upscaler] nvn_fsr_quality: how far below the output the replacement renderer draws while an upscaler is on.
enum class FsrQuality { NativeAA, Quality, Balanced, Performance, UltraPerformance };
constexpr int kFsrQualityCount = 5;
const char* FsrQualityName(FsrQuality q); // the ini / menu text: "Native AA", "Quality", ...
FsrQuality FsrQualityNow();               // read live; absent / unknown = Native AA

/// The Render scale the replacement renderer uses (a Settings::ResolutionSetup index). saved_step = the user's saved
/// Graphics > Resolution scale. Equal to saved_step unless the Direct renderer runs an upscaler (nvn_fsr) without
/// [Upscaler] render_scale_override: then the Quality mode decides (see kFsrQualityRatios). *by_mode: that case.
/// The saved setting itself is never rewritten. *why: a one-line description for the log / menu.
int EffectiveResolutionStep(int saved_step, bool* by_mode = nullptr, std::string* why = nullptr);
/// "1X", "3/4X", "1/4X" ... for a Settings::ResolutionSetup index.
const char* ResolutionStepName(int step);
/// [Graphics] renderer = Direct (read live).
bool DirectRendererNow();
/// [Upscaler] render_scale_override (default false), read live.
bool RenderScaleOverrideNow();
/// [Upscaler] nvn_fsr_split_scale (default false), read live: kill switch of the split render / output scale below.
bool FsrSplitScaleNow();

/// Split render / output scale for the FSR Quality modes on the Direct renderer: the scene (every game render target)
/// is drawn at render_scale of the game's own render size, the present images and the HUD layer at 1X (the output),
/// and FSR upscales between the two. render_scale = up_scale >> down_shift (suyu's rescaling form, baked into the
/// translated shaders). Applies only when: an upscaler is on, renderer = Direct, render_scale_override is off,
/// nvn_fsr_split_scale is on, the mode is not Native AA, the output step Native AA would use is exactly 1X (the game
/// renders at the output itself, e.g. native_render with a resolution set, or the default 1080p) and the mode's scale
/// is below 1. Otherwise false and the single-factor path (EffectiveResolutionStep) is used unchanged.
struct FsrSplit {
    double render_scale{1.0}; // = up_scale / 2^down_shift
    u32 up_scale{1};
    u32 down_shift{0};
    std::string why; // one line for the log / menu (also filled when false: why the split is not used)
};
bool FsrSplitScale(FsrSplit* out);

/// game_settings.ini [Display] show_fps: the frame rate in the window title.
bool ShowFps();

/// game_settings.ini [Display] confirm_exit (default true): ask before closing the window.
bool ConfirmExit();

/// game_settings.ini [Debug] diagnostics: the run reports beyond recomp_report.txt's misses
/// (profiler, NVN census/trace, timing watch, file monitor). On unless set to false.
bool DiagnosticsEnabled();
/// [Debug] gpu_diagnostics (default off): reports of the replacement renderer only (nvn_prof.txt timing
/// buckets). Independent of `diagnostics`, which is for every graphics mode.
bool GpuDiagnosticsEnabled();
/// [Debug] nvn_call_sampling (default off; on only when set to true / 1 / yes / on): per-call NVN recording
/// bookkeeping (call counts + the first raw argument samples per entry point, written to nvn_rec.txt). Costs a
/// global lock and two string-keyed map lookups per recorded NVN call on the guest thread. Read once at start.
bool NvnCallSampling();
/// [Debug] log_redact_paths (default true): logs and reports show %USERPROFILE% / <game> / <user> / <path>/<file>
/// instead of real paths, hide the Windows user and computer name, and replace IPv4 addresses by <ip-N class> (Common::Log::RedactText).
bool LogRedactPaths();
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

/// [Debug] nvn_upscaler_probe = on: the NVN Vulkan renderer writes user/nvn_upscaler_probe.txt (render targets, depth/HUD boundary). Off by default.
bool NvnUpscalerProbe();
/// [Debug] nvn_jitter_spike = 0|1|2: debug sub-pixel jitter on the main camera's render projection (1 = static +x offset,
/// 2 = Halton(2,3) 8 phases per present). F4 toggles at runtime, F3 cycles the amplitude {0.5, 2, 8} px. Log: user/nvn_jitter_spike.txt.
/// Off by default; unchanged behaviour when 0.
int NvnJitterSpike();
/// [Debug] nvn_mv_pass = on: the NVN Vulkan renderer computes a camera-reprojection motion vector image each frame
/// (F2 dumps it to user/upscaler_dump, log user/nvn_mv_pass.txt). Off by default; nothing changes when off.
/// Also true when nvn_fsr is on (FSR needs the camera capture and the motion vectors).
bool NvnMvPass();
/// [Debug] nvn_fsr = on: FSR 3.1 temporal upscaler in the NVN Vulkan renderer (implies the camera capture, the motion
/// vector pass and an FSR Halton jitter on the main camera projection). Off by default; nothing changes when off.
bool NvnFsr();
/// [Debug] nvn_fsr_reactive: start with the dynamic-object mask as FSR reactive mask (Numpad2 toggles). Default on.
bool NvnFsrReactive();
/// [Debug] nvn_fsr_sharpness = 0..1 (default 0.3; 0 = FSR sharpening off).
float NvnFsrSharpness();
/// [Debug] nvn_fsr_snapdraw = N: take the FSR snapshot (finished scene without HUD) right after draw N of the frame
/// (the numbering of the "snapshot after draw" field in user/nvn_fsr.txt); 0 = automatic (default). Read from the
/// ini on every call (the renderer polls it about once a second), so it can be changed while the game runs.
int NvnFsrSnapDraw();
/// [Debug] nvn_fsr_hudlayer = 1 (default): HUD draws after the snapshot go to a separate premultiplied HUD layer that is
/// composited over the FSR output; 0 = the old HUD-mask-by-image-difference composite. Numpad7 toggles it at run time.
bool NvnFsrHudLayer();
/// [Debug] nvn_fsr_motionmask = 1: frame-difference motion mask (current input outside the 3x3 colour box of
/// the previous input at the camera-reprojected texel) added to the FSR reactive mask; 0 = off (default). Numpad8 toggles.
bool NvnFsrMotionMask();
/// [Debug] nvn_fsr_jitter_conv = 0..3 (default 1): startup jitter sign convention (Numpad3 cycles it at run time).
int NvnFsrJitterConv();
/// [Debug] nvn_fsr_backend = 3 (default) | 4: 3 = FSR 3.1.4 (externals/fsr-vulkan, Vulkan); 4 = FSR 4 through the driver's
/// DX12 ffx-api loader (System32 amd_fidelityfx_dx12.dll, Windows only) on a D3D12 device sharing images with Vulkan.
/// Read at Vulkan device creation (enables VK_KHR_external_memory_win32 / VK_KHR_external_semaphore_win32 only for 4).
int NvnFsrBackend();
/// [Debug] nvn_fsr_objmv = 1 (default): local block-match object motion for dynamic texels (silhouettes) written into the
/// FSR motion vector image where the match is confident; 0 = camera motion vectors only. Numpad9 toggles.
bool NvnFsrObjMv();
/// [Debug] nvn_fsr_objmv_near = linear view depth (game units; default 3000, assumed cm) below which the low-rate
/// (cloth) change detection of nvn_fsr_objmv is allowed; farther texels (ropes, fine distant edges) are never flagged.
float NvnFsrObjMvNear();
/// [Debug] nvn_fsr_objvel = 0..4 (default 4; 4 = every depth-tested colour+depth scene draw, blended ones only when they
/// write depth): geometry object motion vectors (transform feedback of the scene draws'
/// clip positions, this frame against the previous frame's capture of the same draw). 0 = off (no capture shaders are
/// built), 1 = draws into the dynamic-object mask target only, 2 = mask draws + skinned (dynamically indexed constant
/// buffer) opaque scene draws, 3 = skinned opaque scene draws only. Numpad9 cycles it with nvn_fsr_objmv at run time.
int NvnFsrObjVel();
/// [Debug] nvn_fsr_objvel_maxverts (default 4000000): captured vertices per frame (draws past it keep camera vectors).
u32 NvnFsrObjVelMaxVerts();
/// [Debug] nvn_fsr_objvel_reactive = 0..1 (default 0.1): reactive mask cap on texels that have a valid object vector.
float NvnFsrObjVelReactive();
/// [Debug] nvn_fsr_objvel_maxpx (default 64): per-vertex object motion above this many render pixels is not trusted.
float NvnFsrObjVelMaxPx();
/// [Debug] nvn_fsr_reactive_strength = 0..1 (default 0.9): scale of the reactive mask outside object-vector texels.
float NvnFsrReactiveStrength();
/// [Debug] nvn_fsr_animsurf = 1 (default): 8x8 tiles of static surfaces (object vector = camera vector) whose 4x4 low-pass
/// colour changed against the reprojected previous input (texture / UV flipbook animations) get a high reactive in that
/// frame and the next. Numpad0 toggles. nvn_fsr_animsurf_thr = tile score threshold (default 0.03, colour 0..1).
bool NvnFsrAnimSurf();
float NvnFsrAnimSurfThr();
/// [Debug] nvn_fsr_signals = 1: per-draw pose-signal diagnostics (texture memory / texel / per constant slot hashing and the
/// 5 s tables). Default 0 (costs CPU on the render thread).
bool NvnFsrSignals();
/// [Debug] nvn_fsr_debug = true (or nvn_bench = 1, which needs the state toggles): the FSR debug / tuning hooks stay active
/// (Numpad keys, F2 mv dump, ini re-reads once a second, per-draw draw lists / target summaries, the FSR-stage timestamps
/// without gpu_diagnostics, the FSR jitter log). Default off: those are dropped and the startup values are used. Read once.
/// nvn_fsr_debug also keeps its older meaning (FFX debug checking for nvn_fsr_backend = 4).
bool NvnFsrDebugHooks();
/// Why NvnFsrDebugHooks() is on / off (startup log line).
const char* NvnFsrDebugHooksReason();
/// [Debug] nvn_fsr_animsurf_bypass = 1 (default): on animated-surface tiles the composite mixes the FSR output with the
/// current input upscaled directly (Catmull-Rom, jitter removed): 0.85 in the flag frame, 0.5 in the next.
bool NvnFsrAnimSurfBypass();
/// [Debug] nvn_fsr_objvel_static = 1 (default): mode 4 stops capturing draws whose object vectors matched the camera
/// vectors for 240 checked frames (re-checked after 600 frames).
bool NvnFsrObjVelStatic();
/// [Debug] nvn_fsr_static_frames = N (default 240, 1..100000): checked frames a draw's object vectors must equal the camera's
/// before nvn_fsr_objvel_static excludes it; after the 600-frame exclusion, max(N - 40, 0) frames are kept as credit
/// (default: 200, i.e. 40 more static frames exclude it again, as before).
u32 NvnFsrStaticFrames();
/// [Debug] nvn_fsr_menuskip = 1: the old menu skip (no FSR, no jitter on frames without a scene pass). Default 0: FSR runs
/// on every frame; frames without a scene pass are fed as a still picture (zero motion, the jitter that is really in it).
bool NvnFsrMenuSkip();
/// [Debug] nvn_fsr_menu3d = 1 (default): on frames without a scene pass whose HUD section draws opaque 3D meshes (save
/// screen / equipment previews), those meshes go into the FSR input (snapshot after the last one, camera jitter kept) so
/// they are anti-aliased; the UI after them stays in the HUD layer.
bool NvnFsrMenu3d();
/// [Debug] nvn_fsr_animsurf_edge_gate = 1 (default): animated-surface tiles must be flat in depth (< 5% range incl. a 1-texel
/// border), so thin lines / silhouettes are not flagged.
bool NvnFsrAnimSurfEdgeGate();
/// [Debug] nvn_fsr_indicator = 1 (default off): FSR state squares in the output's top-left corner; 0 = off.
bool NvnFsrIndicator();
/// [Debug] nvn_fsr_hud_lo / nvn_fsr_hud_hi: composite strong-HUD-mask smoothstep edges (defaults 0.08 / 0.25), not cached.
float NvnFsrHudLo();
float NvnFsrHudHi();
/// nvn_fsr runtime state (Insert key in the renderer): the jitter is only applied while FSR is active.
void FsrSetActive(bool on);
bool FsrActive();
/// The renderer publishes the real render target size (incl. render scale) and the output (present) size: the
/// jitter phase count is AMD's int(8 * (output_w / render_w)^2) and the offsets are +-0.5 render target pixels.
void FsrSetSizes(u32 render_w, u32 render_h, u32 output_w, u32 output_h);
/// Phase index / count of the last FSR jitter pick (logs).
void FsrJitterPhase(u32* index, u32* count);
/// [Debug] nvn_mv_lag = 0..3 (default 1): frame F's motion vectors use the camera record of guest present F - lag.
int NvnMvLag();

/// Main camera state of one guest present interval (nvn_mv_pass). Matrices: 16 floats row-major, row-vector
/// convention (v_view = v_world * view, clip = v_view * proj), as the game writes them.
struct MvCam {
    u64 present{};            // guest present (1-based count) that closed the interval the camera was computed in
    u32 ptr{};                // camera object
    float view[16]{};
    float proj[16]{};         // unjittered (read before the jitter spike writes m[8]/m[9])
    float jitter_px[2]{};     // jitter applied to that projection, render pixels of MvRenderSize (+x right, +y up); 0 if none
    float eye[3]{};           // from the matrix: eye = -t * R^T
    u32 calls{};              // main-camera projection calls in the interval (expected 1)
    u32 thread{};             // hash of the host thread that captured it
    bool reset{};             // camera pointer changed or eye moved > 100 units since the previous record
    bool valid{};
};
/// Guest thread, every nvnQueuePresentTexture that reaches the NVN Vulkan renderer: closes the interval. Returns its index.
u64 MvOnGuestPresent();
/// Latest record with present <= target (the still open interval counts as present + 1). False if none.
bool MvCamFor(u64 target, MvCam* out);
/// Counters for the log.
std::string MvCaptureStats();
/// The pixel grid the jitter values refer to (game render size).
void MvRenderSize(u32* w, u32* h);
/// [Debug] descriptor_buffer = on: suyu's Vulkan renderer may use VK_EXT_descriptor_buffer for guest graphics
/// pipelines that cannot use push descriptors (port of Eden's path; read once at Vulkan device creation). Off by default.
bool DescriptorBuffer();
/// [Debug] multi_range_buffers = on: suyu's Vulkan buffer cache gathers read-only storage buffers whose GPU range maps
/// to several separate device ranges into one buffer (port of Eden's multi-range buffers). Off by default.
bool MultiRangeBuffers();
/// [Debug] suyu_render = false (only with nvn_vulkan = present): suyu's own renderer skips draws and clears, since the
/// native renderer draws the frame. Default true.
bool SuyuRender();
/// [Debug] present_nodrain (present mode): Present does not wait for the GPU; the blit for frame N is published one
/// present later. Default true; present_nodrain = false restores the old draining present.
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

/// Diagnostic log only ([PIAPROBE]): MHGU probe request handler path (main+0x1311010..0x131F400).
extern std::atomic<u32> g_pia_lo;   // first guest address of the hooked range
extern std::atomic<u32> g_pia_hi;   // its span in bytes (0 = off)
void OnPiaProbe(u32 pc, u32* regs);

/// [Debug] nvn_upscaler_probe: camera matrix log (pre-hook on main+0x1C358 / 0x1C3A8 projection and 0x1C33C / 0x1C380 view; F5 = 1800-present camera trace to nvn_upscaler_probe_camtrace.txt; CAM_JUMP detector; plus a return-address post-hook;
/// never writes guest memory except the return address of the hooked call) and filter constructor hit counters.
extern std::atomic<bool> g_probe_on;
void OnProbeDispatch(u32 pc, u32* regs);
/// Text of the camera calls logged since the last drain (cap 64 lines); used by the F6 burst capture.
std::string CamProbeDrain();

/// Hot path: every block the dispatcher is about to run.
inline void OnDispatch(u32 pc, u32* regs) {
    if (pc == g_aspect_pc.load(std::memory_order_relaxed) ||
        pc == g_render_default_pc.load(std::memory_order_relaxed) ||
        pc == g_render_create_pc.load(std::memory_order_relaxed) ||
        pc == g_motion_blur_pc.load(std::memory_order_relaxed) ||
        pc == g_vp_probe_pc.load(std::memory_order_relaxed)) {
        OnHookedEntry(pc, regs);
    }
    if (pc - g_pia_lo.load(std::memory_order_relaxed) < g_pia_hi.load(std::memory_order_relaxed)) {
        OnPiaProbe(pc, regs);
    }
    if (g_probe_on.load(std::memory_order_relaxed)) {
        OnProbeDispatch(pc, regs);
    }
}

} // namespace GameSettings

} // namespace Core
