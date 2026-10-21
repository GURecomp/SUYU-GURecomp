# FSR 4 on native Linux for the NVN->Vulkan renderer: research and plan (2026-10-20)

Scope: research and design only. No source was edited. Nothing below was run; every "works" claim is marked VERIFIED
(seen on this machine or in a primary source), REPORTED (secondary source) or UNVERIFIED (my inference, needs a test).

## 1. Answer in one paragraph

There is no native Linux / Vulkan build of FSR 4 from AMD. FSR 4 is only shipped as signed Windows DLLs that issue D3D12
compute work and use AMD "AGS magic" opcodes. The only way FSR 4 runs on Linux today is: AMD's PE DLLs inside Wine, on
top of vkd3d-proton (which turns the WMMA/FP8 opcodes into `VK_KHR_cooperative_matrix` + `VK_KHR_shader_float8`) on RADV.
So a "native Linux process" cannot load it directly; the only realistic native route is a **Wine helper process** that
hosts the existing D3D12 half of `nvn_fsr4.cpp`, with the native renderer talking to it over a pipe plus shared memory.
Zero-copy GPU sharing between a native process and a Wine process has no known precedent and is the one big unknown.
Recommended: first prove the DLL chain under Wine with the existing `fsr4_probe.exe` (about half a day), then build the
helper with a host-memory transport (works with certainty, costs about 2 ms of PCIe/CPU per frame), then optionally try dma-buf zero-copy.

## 2. Findings

### (a) Does AMD ship a Vulkan or Linux FSR 4?  No.
- FidelityFX SDK latest I could find: v2.3.0 (24 Jun 2026). The only asset named is `FidelityFX-Samples-v2.3.0-prebuilt.zip`
  ("pre-built, signed DLLs"); prerequisites list Visual Studio 2022 + Windows 10 SDK; SDK v1 effects (the Vulkan ones) are
  deprecated. VERIFIED (github releases page via WebFetch). Coverage of 2.0 -> 2.3 mentions DirectX 12 only; I found no
  AMD statement on Vulkan or Linux FSR 4 (searched; absence is not proof).
- OptiScaler 0.9.0-pre10 made FSR 4 usable in Vulkan games by translating Vulkan->DX12; AMD's driver FSR 4 override does not
  work for Vulkan titles. REPORTED (PCGH, Igor's Lab, HotHardware, wccftech). On Linux it is recommended with current Mesa +
  Proton-CachyOS. This confirms the architecture "Vulkan app -> DX12 FSR 4" but it all runs inside Wine.
- Local check: `amd_fidelityfx_vk.dll` (RDR2, OptiScaler_0.7.9) contains no FSR4/amdxc strings: it is the FSR 3.1 Vulkan
  backend, not FSR 4. VERIFIED (strings).
- SDK licence (`docs/license.md`): binary-only, redistribution of binaries allowed in binary form with notices, "no reverse
  engineering, decompilation or disassembly". VERIFIED (WebFetch). Consequence: never extract the model/weights to write
  our own Vulkan FSR 4 (themaister's blog says the weights are one big buffer; the FP8 path needs Mesa ACO changes anyway).

### (b) vkd3d-proton / Wine route
- vkd3d-proton 3.0 added FSR 4 for RDNA4 using `VK_KHR_cooperative_matrix` + `VK_KHR_shader_float8` (native FP8 only on
  RDNA4, where the default build exposes it). REPORTED (gamingonlinux, guru3d). Older GPUs need an int8 emulation build.
- AMD's FSR 4 code builds ordinary D3D12 compute shaders on the supplied `ID3D12Device`, hides WMMA in atomic compare-exchange
  on a magic UAV (AGS opcodes), and calls back into `amdxc64.dll` for configuration (themaister, May 2025; VERIFIED page).
  Proton ships its own `amdxc64.dll` shim; with `FSR4_UPGRADE=1` it loads `amdxcffx64.dll` (CachyOS forum, REPORTED).
- Important local finding: the SDK 2.x `amd_fidelityfx_upscaler_dx12.dll` (28.7 MB) embeds the FSR 4 provider itself
  (strings: `ffxProvider_FSR4`, `ffxProvider_FSR4_Int8`, `FSR4-i8`, `fsr4_model_v07_fp8_...`, imports `amdxc64.dll`), and the
  tiny `amd_fidelityfx_loader_dx12.dll` (26 KB) exports `ffxCreateContext`/`ffxQuery` and loads the sibling
  `amd_fidelityfx_upscaler_dx12.dll`. VERIFIED (strings). So under Wine the chain should be
  loader + upscaler DLLs (SDK 2.x) -> Proton's `amdxc64.dll` shim -> vkd3d-proton. `amdxcffx64.dll` is then not needed
  (it is the driver's copy that the Windows driver loader uses). UNVERIFIED that a bare ffx-api host (not a game) works like
  this under Wine, but it is exactly what Proton does for games.
- Can a native process load vkd3d-proton or Wine's vkd3d directly?  No, and it would not help:
  - vkd3d-proton is Windows-API code (PE DLLs `d3d12.dll`/`d3d12core.dll`); Proton ships only the PE builds (checked:
    `files/lib/wine/vkd3d-proton/x86_64-windows/`). I know of no supported native-Linux build. UNVERIFIED but irrelevant:
  - the consumer is AMD's PE DLLs, which need a PE loader + Win32 + the amdxc64 shim = Wine.
  - Wine's own vkd3d (`libvkd3d`, `libvkd3d-shader`; only PE copies exist here, native `vkd3d` package is not installed)
    has no AGS/WMMA-to-cooperative-matrix path, so it cannot run FSR 4.
- Wine's winevulkan does not give a native process access to its memory. The CodeWeavers Vulkan shared-memory patches
  were reported never merged to mainline (REPORTED, phoronix/forum thread); Proton/Wine-GE carry their own. Cross-process
  Windows-handle sharing between a Wine process and a *non-Wine* process has no supported mechanism that I found.

### (c) This machine (VERIFIED unless noted)
- pacman: `wine 11.20-1.1`, `wine-cachyos-opt 2:10.0.20260425-1` (/opt/wine-cachyos), `proton-cachyos-slr 1:11.0.20261014-1`,
  `umu-launcher 1.4.3`, `winetricks`, `protontricks`, `mesa 3:26.2.4-1`, `vulkan-radeon 3:26.2.4-1`, lib32 variants.
  No `vkd3d`, no `fidelityfx` package.
- Proton-CachyOS at `/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/` (not in `~/.steam/.../compatibilitytools.d`).
  vkd3d-proton version file: `vkd3d-1.1-5656-g0bb64c5` (post-3.0). Has `x86_64-windows/{d3d12.dll,d3d12core.dll,dxgi.dll,
  amdxc64.dll,amd_ags_x64.dll}` under `files/lib/wine/`, a ready `files/share/default_pfx`, and the protonfixes verb
  `amdxcffx64.verb` (downloads `https://download.amd.com/dir/bin/amdxcffx64.dll/67D435F7d97000/amdxcffx64.dll`, sha256
  2bc7a8f0...1399e, FSR 4.0.1; REPORTED later removed/delisted by AMD).
- No Wine prefix yet (`~/.wine`, umu dirs absent). Steam is present (`~/.local/share/Steam`).
- AMD DLLs the user already owns (on the Main drive, game folders; read-only for us, copy only for local testing):
  - `.../FarFarWest/FarFarWest/Plugins/FSR/Source/fidelityfx-sdk/Kits/FidelityFX/signedbin/`
    `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` (28.7 MB), `amd_fidelityfx_framegeneration_dx12.dll`
    (Aug 31 build, SDK 2.x layout). Also Dead as Disco / Satisfactory carry `upscaler` + `framegeneration` only (no loader).
  - `amd_fidelityfx_dx12.dll` (the *old* v1.x single-file ffx-api, game-bundled: RDR2, MHW, Spider-Man 2, Elden Ring ...).
    These do not contain the FSR 4 provider (FSR 4 came from the Windows driver). Do not use them for FSR 4.
  - No `amdxcffx64.dll` anywhere on the visible drives.
- `ptrace_scope = 1` (relevant for the dma-buf experiment, section 6).
- `fsr4_probe.exe` + source exist in `/run/media/linxu/Main/GUU test/fsr4-probe/` (Windows PE; usable under Wine as test).

### (d) Licence / distribution
- Cannot ship: any AMD DLL (`amd_fidelityfx_*_dx12.dll`, `amdxcffx64.dll`, `amdxc64.dll` from a driver). Proton downloads
  `amdxcffx64.dll` at run time (verb above); the Proton `amdxc64.dll` shim is Valve's/CachyOS's, shipped with Proton.
- The SDK licence text does permit binary redistribution with notices, but that is the user's lawyer's call; the plan below
  keeps the existing design stance: ship nothing of AMD's; the user points a setting at their own DLL folder (or a Proton).
- Do not read or take code from SDK v2.x sources/headers (binary-only licence, no RE). Keep using the MIT v1.1.4 headers in
  `externals/ffx-api`. Whether 2.x loader/upscaler accept the 1.1.4 struct ABI is a milestone-0 test (same stance as the
  Windows design doc).

## 3. Recommended route: Wine helper process ("fsr4_host.exe") + pipe + shared memory

```
suyu (native Linux, RADV)                               Wine/Proton process (vkd3d-proton -> RADV, same GPU)
  render thread                                           fsr4_host.exe  (PE, built from nvn_fsr4.cpp's D3D12 half)
   Bridge (nvn_fsr4_linux.cpp) --stdin/stdout pipe-->     command loop: INIT / ENSURE / DISPATCH / SHUTDOWN
   host-visible VkBuffers  <==== shared mmap file ====>   file mapping -> upload/readback buffers of D3D12 resources
                                                          LoadLibrary(loader) -> ffxDispatch  (FSR 4 inside)
```

### 3.1 Required external files
1. Proton-CachyOS (already installed) or any Proton with vkd3d-proton >= 3.0 and its `amdxc64.dll` shim. Launch through
   `umu-run` (installed) or `proton run` with `PROTONPATH=/usr/share/steam/compatibilitytools.d/proton-cachyos-slr`.
   Fallback to a plain Wine prefix (the CachyOS forum thread, REPORTED, says plain Wine works if you copy vkd3d-proton
   `d3d12.dll`/`d3d12core.dll`, `dxgi.dll` and `amdxc64.dll` into the prefix and set `WINEDLLOVERRIDES`).
2. User-supplied AMD DLLs (not shipped): `amd_fidelityfx_loader_dx12.dll` + `amd_fidelityfx_upscaler_dx12.dll` from FidelityFX
   SDK 2.x (`FidelityFX-Samples-vX-prebuilt.zip` on the GPUOpen-LibrariesAndSDKs/FidelityFX-SDK releases page, or a game
   that carries them; locally: the FarFarWest folder above). Optional `amdxcffx64.dll` (4.0.x/4.1 from the Windows driver or
   Proton's verb) with `FSR4_UPGRADE=1` if the bundled upscaler is older than the wanted FSR 4 version (REPORTED rule: the
   newer build time wins).
3. `fsr4_host.exe`: new target built with mingw-w64 (cross, `x86_64-w64-mingw32`) from the D3D12/ffx half of the existing
   `nvn_fsr4.cpp`, MIT ffx-api headers, d3d12 headers from mingw-w64 (or the Windows SDK ones used today).
   Needs `mingw-w64-gcc` (not checked whether installed; CachyOS has it in extra).

### 3.2 Concrete steps
M0 (0.5 day, no suyu change): put the loader+upscaler DLLs next to a copy of `fsr4_probe.exe`, run it with
`umu-run` (GAMEID=0, PROTONPATH as above). The probe loads `C:\Windows\System32\amd_fidelityfx_dx12.dll` (nvn_fsr4.cpp:402
equivalent in the probe): either copy the 2.x loader into the prefix `system32` renamed to `amd_fidelityfx_dx12.dll`
(UNVERIFIED that the loader's sibling lookup still works after renaming) or change the probe to `amd_fidelityfx_loader_dx12.dll`.
Pass criteria: (1) provider list contains an FSR 4.x entry, (2) the 1.1.4 headers are ABI-compatible with the 2.x loader,
(3) output PNG sane, (4) the in-Wine Vulkan<->D3D12 sharing of the probe works (this also decides fallback B).
If (1) fails, try `FSR4_UPGRADE=1` + `amdxcffx64.dll`, `PROTON_ENABLE_NVAPI` is irrelevant. If still no FSR 4, stop: the
chain is not usable, tell the user.

M1 (about 2 days): `fsr4_host.exe` standalone mode with a synthetic client, host-memory transport.
- D3D12 device: no LUID matching is needed to a Windows Vulkan device; pick the DXGI adapter by name/LUID passed from suyu
  (Wine's DXGI exposes the same GPU; the LUID differs, so match by `VkPhysicalDeviceIDProperties::deviceUUID` or just
  adapter index 0 + description string check). UNVERIFIED how Proton's DXGI reports the 9070 XT LUID/UUID.
- Resources: D3D12 committed DEFAULT textures (colour RGBA8, depth R32F, mv RG32F or RG16F, reactive R8, output RGBA8) plus one
  UPLOAD buffer and one READBACK buffer sized for the inputs / output. Per frame: copy upload->textures, ffxDispatch,
  copy output->readback, signal fence, CPU wait, reply on the pipe.
- Transport: suyu creates a file in `$XDG_RUNTIME_DIR` (tmpfs), `ftruncate`, mmap; the helper opens it as `Z:\run\user\1000\...`
  and `CreateFileMapping/MapViewOfFile`. Control messages over the helper's stdin/stdout pipes (inherited from the native
  parent via fork/exec of `umu-run`; `ReadFile/WriteFile` on `GetStdHandle`). UNVERIFIED: pressure-vessel (slr container)
  exposes `$XDG_RUNTIME_DIR` and `/dev/shm`; if not, use `PROTON_NO_STEAMRT`/plain wine, or bind-mount via `PRESSURE_VESSEL_FILESYSTEMS_RW`.
- Optional: D3D12 `OpenExistingHeapFromAddress` (vkd3d-proton uses `VK_EXT_external_memory_host`; UNVERIFIED supported on
  RADV+Wine) to avoid the CPU memcpy into the upload buffer.

M2 (about 2 days): Linux Bridge class in suyu (same `Bridge` interface, section 4). Native side:
- images are plain native Vulkan images (no external memory);
- per frame in `Dispatch`: CPU `vkWaitSemaphores(timeline, wait_value)`, then an own submit that copies the four input
  images into the mapped host-visible buffers (a `VkBuffer` over the mmap using `VK_EXT_external_memory_host`, or a normal
  host-visible buffer + `memcpy`), CPU fence wait, send DISPATCH, block on the pipe reply (with timeout), then a second own
  submit copies the output buffer into the output image and signals `signal_value` on the timeline. The renderer's existing
  wait on `v2` (nvn_vk.cpp:7552-7553) then holds unchanged.
- Failure handling as today (`Fsr4Failure`, nvn_vk.cpp:7402): helper dead / timeout -> `ReleaseWaiters()` -> FSR 3.1.

M3 (1 day): polish, benchmark line (helper reports its D3D12 GPU ms through the pipe -> `TakeGpuStats`), settings, docs.

M4 (optional, research 2-4 days): zero-copy, see section 6.

Estimate: 5-6 working days to a working benchmarkable result if M0 passes; +2-4 days for any zero-copy attempt. Biggest
schedule risk is M0/M1 (Wine environment surprises), not the renderer glue.

### 3.3 Cost / benchmark honesty
Per frame at 1920x1080 -> 3440x1440 (numbers are estimates from the design doc formats, UNVERIFIED): inputs about 8 MB colour +
8 MB depth + 16 MB mv (8 MB if RG16F) + 2 MB reactive = 26-34 MB up, 20 MB output back. Over PCIe/host memory that is
roughly 2-3 ms of copies plus two CPU round trips (pipe ~0.1 ms each) plus a CPU-blocked render thread. So the in-game
benchmark should print FSR 4 as `dispatch ms` (helper D3D12 timestamps, same as Windows) AND `bridge overhead ms`; the
Linux FSR 4 frame time is not comparable with the Windows zero-copy path or with FSR 3.1. Say so in the overlay line.

## 4. Interface changes in the repo (file:line)

CMake, `src/video_core/CMakeLists.txt`:
- Line 364 `if (WIN32)` gates `nvn_fsr4.cpp/.h`, the ffx-api include dir and `SUYU_HAS_FSR4_BRIDGE`. Change to:
  `if (WIN32) <existing> elseif (UNIX AND NOT APPLE) add renderer_vulkan/nvn_fsr4_linux.cpp + nvn_fsr4.h, define SUYU_HAS_FSR4_BRIDGE=1
  and SUYU_FSR4_WINE_HELPER=1 endif`. The Linux file needs no ffx headers (those are only in the helper).
- New optional target `fsr4_host` (mingw-w64 toolchain file, `add_custom_target`, not part of `all`) that compiles
  `src/video_core/renderer_vulkan/fsr4_host/main.cpp` + the D3D12 code factored out of `nvn_fsr4.cpp`.
  Do not make it a dependency of suyu; the user builds it once (or we install it next to the exe).

`src/video_core/renderer_vulkan/nvn_fsr4.h` (108 lines):
- Header is Win32-free already except the doc comment; `VkContext` (lines 20-27) needs: queue + family index, a
  `std::function<VkResult(const VkSubmitInfo&)> submit` (the renderer's queue lock), and a settings block
  `std::string wine_launcher, helper_path, dll_dir` (new `[Debug]` keys: `nvn_fsr4_helper`, `nvn_fsr4_launcher`, `nvn_fsr4_dlls`).
- Add `bool UsesExternalQueueFamily() const;` (true on Windows, false on Linux). Needed because the renderer releases/acquires
  the shared images with `VK_QUEUE_FAMILY_EXTERNAL`.
- `Bridge::Dispatch` doc (lines 76-78): state that on Linux it blocks the calling thread until the signal value is signalled
  by the bridge itself (CPU `vkSignalSemaphore` or its own submit).

`src/video_core/renderer_vulkan/nvn_fsr4.cpp` (780 lines, Windows only): keep as is; split the pure D3D12/ffx code
(roughly lines 175-230 ring/timestamps, 236-300 resource creation minus the Vulkan import, 402-545 init, ffxDispatch section ~680)
into a shared include `nvn_fsr4_d3d12.inl` used by both nvn_fsr4.cpp (Windows, with the Vulkan import at 249-296 and the
timeline import at 521-540) and `fsr4_host/main.cpp` (Wine, with the shared-memory transport instead). The Windows
LUID match (lines 412-443) becomes name/UUID based in the helper.

New `src/video_core/renderer_vulkan/nvn_fsr4_linux.cpp`: implements `Bridge` from nvn_fsr4.h with: fork/exec of the launcher,
pipe protocol (versioned, fixed-size messages: INIT{adapter name, dll dir}, ENSURE{sizes, formats}, DISPATCH{DispatchParams},
reply{ok, version string, provider list, gpu_ms, error text}), mmap file, host buffers, own timeline semaphore
(created with plain `VkSemaphoreTypeCreateInfo`, no external handle), `JitterPhaseCount/JitterOffset` forwarded as pipe
queries (the renderer calls them at nvn_vk.cpp:7451-7464).

`src/video_core/vulkan_common/vulkan_device.cpp:1022-1040` / `vulkan_device.h:284-287,1176`: `fsr4_interop` is `_WIN32` only.
On Linux, set `fsr4_interop = true` when `nvn_fsr`+`backend==4` and (if used) `VK_EXT_external_memory_host` is supported;
enable that extension and `timelineSemaphore` (1.2 feature, already core in 1.4). `HasFsr4Interop()` message at
nvn_vk.cpp:7358 should not mention win32 on Linux.

`src/video_core/renderer_vulkan/nvn_vk.cpp`:
- 7353-7356: `#ifndef SUYU_HAS_FSR4_BRIDGE` text says "Windows only"; reword.
- 7363-7370: `VkContext` fill: add queue, submit callback, the new settings.
- 7467-7480 `bar` lambda: if `!fsr4->UsesExternalQueueFamily()` use `VK_QUEUE_FAMILY_IGNORED` for both indices (the images are
  not external). Keep the layouts.
- 7521-7544 and 7547-7553: unchanged logically (submit v1; Dispatch(v1,v2); next submit waits v2). The only change is that on
  Linux `Dispatch` does CPU work and the ordering still holds because it signals v2 before returning.
- 9921-9940: caller unchanged.
- 7377-7392 `fsr4_cmd` second command buffer approach stays.
- `FsrBackendText()` (7577+): "(DX12 bridge)" -> "(Wine helper)" on Linux.

Settings (`Core::GameSettings`): add `NvnFsr4Helper/Launcher/DllDir` in the same place as `NvnFsrBackend` (not located; grep for
`NvnFsrBackend` in `src/core/` when implementing).

## 5. Risks (all UNVERIFIED unless stated)
1. The 2.x loader/upscaler may not expose an FSR 4 provider outside a game process under Wine, or its struct ABI may differ
   from the 1.1.4 headers (M0 decides). Mitigation: the Windows design doc already treats the ABI as a milestone-1 check.
2. Proton `amdxc64.dll` shim may only activate for specific executables / env (`FSR4_UPGRADE`, protonfixes by exe name).
   Mitigation: name the helper like a known game exe, or set the documented env; check the shim's logs.
3. Wine environment: pressure-vessel filesystem visibility for the mmap file; stdio pipes through `umu-run`; startup time of
   a prefix (several seconds the first run); the helper must not need a display (D3D12 without a window should be fine).
4. Sync stalls: render thread blocks ~2-4 ms per FSR 4 frame; timing numbers are not comparable to Windows.
5. FSR 4 provider performance on Mesa 26.2.4 RADV for RDNA4 (cooperative matrix FP8 shaders compile at first use: expect a
   shader-compile hitch of seconds; vkd3d-proton shader cache helps).
6. Licence/EULA: using AMD's DLLs from a third-party tool. User supplies the files; we ship none.
7. Memory: helper process holds its own Vulkan device (about 200-500 MB VRAM extra, estimate).

## 6. Fallbacks

### A. Zero-copy GPU sharing (upgrade of the recommended route, research only)
Idea: have the helper's vkd3d-proton resources exported as a dma-buf and import them in suyu with `VK_EXT_external_memory_dma_buf`
/ `VK_KHR_external_memory_fd`. Problem: the helper has only NT handles; winevulkan keeps the host fd inside Wine's unix side
and the wineserver. A native process can take an fd from another process with `pidfd_getfd` but needs ptrace permission
(`ptrace_scope` is 1 here: allowed only for descendants; the helper would be suyu's child, the wineserver is not).
A custom builtin DLL + unixlib (built with winegcc against Wine headers, `/usr/include/wine` exists) in the helper could call
`wine_server_handle_to_fd` and `SCM_RIGHTS` the fd to suyu over a unix socket. This needs Wine dev headers matching the
runtime and is fragile across Proton updates. Timeline sync would need `VK_KHR_external_semaphore_fd` equivalents that Wine does
not expose to the PE side. Verdict: only try after M2 works, and expect it to be a research project.

### B. Run the whole Windows suyu build under Proton (no native code at all)
The Windows code path (`nvn_fsr4.cpp` as is) needs: Wine's winevulkan exposing `VK_KHR_external_memory_win32` with
`D3D12_RESOURCE_BIT` and `VK_KHR_external_semaphore_win32` with `D3D12_FENCE_BIT` backed by vkd3d-proton shared handles.
OptiScaler's Vulkan->FSR 4 working in Proton (REPORTED) suggests some form of this works, but I did not find which mechanism it
uses; UNVERIFIED. M0 with `fsr4_probe.exe` tests exactly this. If it passes, it gives the user FSR 4 numbers on Linux today with
zero new code, but it is not a native process (the emulator would also run under Wine, with its own cost), so it only answers
"how fast is FSR 4 on this GPU", not "native in-game benchmark".

### C. Rejected
- Reimplementing FSR 4 in Vulkan from the weights: licence forbids reverse engineering, needs FP8 cooperative matrices
  and Mesa-side work. No.
- Native `libvkd3d` / Wine vkd3d: cannot run AMD's PE DLLs and lacks the AGS translation. No.
- Wait for AMD to publish a Vulkan FSR 4: no announcement found (searched).
- Linux-native benchmark limited to FSR off / FSR 3.1 and report FSR 4 from the Windows build: zero work, always available.

## 7. What I could not verify
- Nothing was executed (no probe run, no Wine launch, no DLL loaded).
- Whether the user's SDK 2.x loader+upscaler DLLs list an FSR 4 provider under Proton-CachyOS 11.0.
- Wine/DXGI adapter identity for the 9070 XT, pressure-vessel visibility of `$XDG_RUNTIME_DIR`, `OpenExistingHeapFromAddress`.
- Whether `mingw-w64` is installed; the exact name/location of the settings accessors.
- Whether Proton's `amdxc64.dll` shim source is public (I did not find it).

Sources: themaister.net/blog/2025/05 (FSR 4 mechanism), gamingonlinux 2025/11 vkd3d-proton 3.0, discuss.cachyos.org thread 23546
(file list and env vars), github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (releases, docs/license.md), pcgameshardware /
igorslab / wccftech OptiScaler Vulkan coverage, phoronix "CodeWeavers Working On Vulkan Shared Memory Support In Wine".
