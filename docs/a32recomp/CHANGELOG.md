## 0.9.0-test

Date: 2026-10-21

### Added
- FSR 3.1 temporal upscaler in the replacement renderer (vendored FSR 3.1.4 Vulkan subset, MIT), with camera jitter, motion vectors from scene depth, object motion vectors (block matching and transform feedback), reactive masks for cloth, animated surfaces and ghost trails, and a HUD layer composited over the upscaled picture.
- Optional FSR 4 on AMD GPUs, using `amd_fidelityfx_dx12.dll` from the installed AMD driver or one placed in the game folder. Falls back to FSR 3.1 when it cannot load.
- Quality modes with separate render and output scale on the Direct renderer.
- Renderer setting: Emulated | Direct.
- Game menu (F10): Upscaler section, Advanced mode, "Restart now" button.

### Changed
- Render scale steps use the console size as 1X (1080 docked / 720 handheld). Output size comes from `resolution`, else the window, else the base size.
- `native_render` no longer widens the guest render size on top of the step scale.
- The picture fills the window layout instead of being pillarboxed when the window is wide.
- Presenting no longer waits for the GPU by default (`present_nodrain = false` restores the old behaviour).
- Software rendering is used when RivaTuner's hooks are detected.
- `game_settings.ini` is regrouped into one section each and migrated once, with a backup of the old file.
- Shader recompiler synced with Eden (quad ops, warp shuffles, int64 lowering, and more). Deferred clears, descriptor buffers and multi-range buffers ported from Eden.
- Performance: lock-free pool tables, per-frame render-target memo, cheaper pitch-linear hashing, slimmer NVN call recording, per-core counters instead of global atomics. Probes and diagnostics cost nothing when off.
- Pipeline cache is saved periodically on a worker thread.

### Fixed
- Fixed a bug that was causing performance problems in 0.8.
- Native Linux was tested to hit 200+ FPS without FSR in this build.
- VkResult is checked on every fence wait and queue submit.
- Crash records are written honestly (`nvn_vk_crash.txt`).
- A stale or cut-off shader cache no longer stops the launch.

### Privacy
- Paths, user and computer names and IP addresses are redacted from logs by default (`[Debug] log_redact_paths`).
- `tools/scrub-log.ps1` cleans older logs.
