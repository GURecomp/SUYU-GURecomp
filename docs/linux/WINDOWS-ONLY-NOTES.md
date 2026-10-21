# Windows-only code in suyu-gpu: porting catalogue

Scope: `src/`, `externals/fsr-vulkan`, CMake files, `scripts/`, `docs/`. Every file:line below was re-checked against the tree on 2026-10-20 after the Linux session (line numbers drift; re-grep the quoted symbol if they do not match). Rows/sections for files not touched in that session (e.g. `host_threads.cpp`, `stall_ring.h`, `perf_stats.cpp`, `logging.cpp`, Qt frontend, scripts, docs) were NOT re-grepped this time: their numbers are from the earlier pass.

See also: **"Linux-session changes that touch shared/Windows code (verify on Windows)"** and **"Windows rebuild checklist"** at the end of this file, and `FSR4-LINUX-PLAN.md` (repo root) for the Linux FSR 4 plan.

Method: grep for `_WIN32`, `WIN32`, `_MSC_VER`, `windows.h`, `LoadLibrary`, `GetProcAddress`, `__declspec`, `.dll`, `.exe`, `D3D12`, `DXGI`, `MessageBeep`, `GetAsyncKeyState`, `QueryPerformance`, `ShellExecute`, `OutputDebugString`, `AddVectoredExceptionHandler`, etc. Third-party and upstream-portable code (`src/dynarmic`, `src/common/*` with complete `#else` branches) is only listed when it has a real Linux gap.

Linux status key:
- **done**: a Linux implementation exists in the same file or a sibling.
- **stubbed**: compiles on Linux but does nothing / returns 0 / logs a fallback.
- **missing**: Windows-only, no Linux counterpart; the feature is absent on Linux.
- **n/a**: Windows concept with no meaning on Linux (e.g. DLL export attributes for GPU selection).

## Summary table

| # | Area | file:line | What | Linux status |
|---|------|-----------|------|--------------|
| 1 | FSR 4 bridge | `video_core/renderer_vulkan/nvn_fsr4.cpp:1-748` (whole file) | D3D12 + `amd_fidelityfx_dx12.dll` bridge | missing |
| 2 | FSR 4 bridge | `video_core/CMakeLists.txt:364-369` | file compiled only `if (WIN32)` | missing |
| 3 | FSR 4 bridge | `video_core/vulkan_common/vulkan_device.cpp:1022-1038` | enables `VK_KHR_external_{memory,semaphore}_win32` | missing |
| 4 | FSR 4 bridge | `video_core/renderer_vulkan/nvn_vk.cpp:7353-7356` | "no FSR 4 bridge (Windows only)" fallback to FSR 3.1 | stubbed |
| 5 | Renderer | `nvn_vk.cpp:49-52` | `<windows.h>/<psapi.h>/<dbghelp.h>` includes | missing (guarded) |
| 6 | Renderer | `nvn_vk.cpp:102-105` | `__declspec(noinline)` vs GCC attribute | done |
| 7 | Renderer | `nvn_vk.cpp:112-127` | `SehRun` (`__try/__except`) access-violation guard | stubbed |
| 8 | Renderer | `nvn_vk.cpp:1417-1424` | `Fatal()`: MessageBoxA + TerminateProcess | done (stderr + `_Exit`) |
| 9 | Renderer | `nvn_vk.cpp:6310-6320` | `WriteStats()` memory line via psapi / GlobalMemoryStatusEx | stubbed |
| 10 | Renderer | `nvn_vk.cpp:10323-10325, 10403-10405, 10418-10420` | `MessageBeep` bench cues | missing |
| 11 | Renderer | `nvn_vk.cpp:11518-11623` | SEH/PE crash recorder (`CrashHandler`, `CrashWalkStack`, `CrashWhere`, `CrashBuildLine`) | missing |
| 12 | Renderer | `nvn_vk.cpp:11641-11644` | `AddVectoredExceptionHandler(1, CrashHandler)` | missing |
| 13 | Renderer | `nvn_vk.cpp:752, 791, 10319` | `tools\perf-summary.ps1` referenced; script not in repo | missing |
| 14 | Renderer | `nvn_vk.cpp:792, 5824, 6282, 6288, 6857, 10352, 10436, 10442, 11692` (include at 10) | hotkeys via `Common::HostKeyDown(VK_*)` (no `GetAsyncKeyState`/dllimport left in this file) | done in suyu-cmd (SDL poller, see 15); Qt GUI: dead (no poller) |
| 15 | Hotkeys | `common/host_key.{h,cpp}` (cpp:17-22 Windows, 23-35 other; `common/CMakeLists.txt:70-71`); poller registered at `suyu_cmd/emu_window/emu_window_sdl2.cpp:640` (`HostKeyPollSDL` 606-637) | `GetAsyncKeyState` on Windows, poller callback elsewhere | done (suyu-cmd only; Qt GUI has no poller) |
| 16 | Vulkan | `video_core/vulkan_common/vulkan.h:10-11`, `vulkan_surface.cpp:22-35` | Win32 surface | done (other branches exist) |
| 17 | Vulkan | `video_core/vulkan_common/vulkan_wrapper.cpp:565-573` | `vkGetMemoryWin32HandleKHR` helper | n/a (upstream, Windows) |
| 18 | Vulkan | `renderer_vulkan/vk_texture_cache.cpp:1452-1458` | Windows-only linear-image copy workaround | n/a (driver workaround) |
| 19 | Vulkan | `renderer_vulkan/vk_staging_buffer_pool.cpp:30-40` | 256 MiB stream buffer on Windows, 128 MiB on Linux | done (tuning) |
| 20 | Recomp | `core/arm/recomp/mod_host.cpp:293-334` | mod loader `LoadLibraryExW`/`GetProcAddress` | done (`dlopen`, `.dll`->`.so` rewrite) |
| 21 | Recomp | `core/arm/recomp/mod_host_api.h:37-38` | `__declspec(dllexport)` | done |
| 22 | Recomp | `core/arm/recomp/game_settings.h:64-65`, `game_settings_ini.cpp:14-15` | default loader `Forge/forge.dll` (Linux `Forge/forge.so`) | done |
| 23 | Recomp | `core/arm/recomp/game_settings.cpp:309-322` (`#ifdef _WIN32` at 313; `SetDesktopMode` at 911, used at 939) | `DesktopMode()` via `EnumDisplaySettingsW` | done (SDL fills it, `suyu_cmd/suyu.cpp:922-928`) |
| 24 | Recomp | `core/arm/recomp/host_threads.cpp:11-70` | per-thread CPU report via Toolhelp32 / `GetThreadTimes` / `GetThreadDescription` | stubbed (`Describe` returns empty, line ~96) |
| 25 | Recomp | `core/arm/recomp/game_settings.cpp:1377,1378,1727` (include at 28) | F3/F4/F5 via `HostKeyDown` (no `GetAsyncKeyState` left) | done (suyu-cmd only; Qt GUI dead) |
| 26 | Kernel | `core/hle/kernel/stall_ring.h:20, 70-82` | `QueryThreadCycleTime` (`__declspec(dllimport)`) | stubbed (returns 0) |
| 27 | Core | `core/perf_stats.cpp:95-107` | `GetProcessTimes` for process CPU seconds | stubbed (returns 0.0) |
| 28 | Core | `core/core_timing.cpp:17-19, 412-416` and `common/windows/timer_resolution.*` | Windows timer resolution (`timeBeginPeriod`-style NtSetTimerResolution wrapper) | n/a |
| 29 | Core | `core/game_settings.cpp:77-78` | `DetectOS()` | done |
| 30 | Recompiler | `core/recompiler/arm64_to_c.h:32-36, 3604-3614, 3665-3667, 3812-3813, 4027-4031, 4365-4370, 4457-4459, 4490-4495` | generated-C emitter: `_mkdir`, DLL naming, `__declspec(dllexport)`, `__forceinline`, `Sleep`, `MemoryBarrier` | done (each has a POSIX/GCC branch) |
| 31 | Recompiler | `core/recompiler/a32/a32emit.h:192-230` | MSVC-specific CMake text in generated projects | done (non-MSVC branch) |
| 32 | suyu-cmd | `suyu_cmd/suyu.cpp:86-92` | `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` exports | n/a |
| 33 | suyu-cmd | `suyu_cmd/suyu.cpp:415-420` | `AttachConsole` + `freopen("CONOUT$")` | n/a |
| 34 | suyu-cmd | `suyu_cmd/suyu.cpp:435-445, 490-500, 513-523, 635-640, 660-665, 673-705, 836-845` | `GetModuleFileNameW`, `CommandLineToArgvW`, UTF-16 path handling | done (`#else` branches) |
| 35 | suyu-cmd | `suyu_cmd/suyu.cpp:473-477` | keys dir under `%APPDATA%` vs `XDG_DATA_HOME` | done |
| 36 | suyu-cmd | `suyu_cmd/suyu.cpp:744-790` | load `recompiled_*.dll` beside the exe (`LoadLibraryW`) | missing on Linux (no `.so` loader; static-link path only) |
| 37 | suyu-cmd | `suyu_cmd/suyu.cpp:857-870` | `SetEnvironmentVariableW(DISABLE_RTSS_LAYER)` (RivaTuner) | n/a |
| 38 | suyu-cmd | `suyu_cmd/suyu.cpp:931-934` | `Common::Windows::SetCurrentTimerResolutionToMaximum()` | n/a |
| 39 | suyu-cmd | `suyu_cmd/emu_window/emu_window_sdl2.cpp:34-60, 84-89 (ShellExecuteW at 88), 444 (`ShowDevMenu`), 712-716` | F12 Win32 debug panel (`ShowDevMenu`, `ShellExecuteW "explore"`) | missing |
| 40 | suyu-cmd | `emu_window_sdl2.cpp:1026-1066, 1068-1075` | Win32 "Compiling shaders" progress box (HWND, `DestroyWindow`) | stubbed (window title only) |
| 41 | suyu-cmd | `emu_window_sdl2.cpp:1082-1100` | window icon from PE resource (`LoadImageW`, `RT_GROUP_ICON` id 1) | stubbed (embedded suyu icon) |
| 42 | suyu-cmd | `suyu_cmd/emu_window/emu_window_sdl2_vk.cpp:10-11, 33, 67-72` | Win32 HWND window info | done (X11/Wayland branches) |
| 43 | suyu-cmd | `suyu_cmd/game_menu.cpp:965-969, 1534-1536, 1618-1620` | `forge.dll` option text, "F12: debug panel" hint, `C:\Windows\Fonts\segoeui.ttf` | done (Linux values); F12 panel itself missing (row 39) |
| 44 | suyu-cmd | `suyu_cmd/multiplayer.cpp:64-68`, `relaunch.cpp:12-100` | `localtime_s`, restart via `CreateProcessW` | done |
| 45 | suyu-cmd | `suyu_cmd/CMakeLists.txt:132-165` | MSVC/WIN32 link options, bundled SDL3 | n/a |
| 46 | Export (Qt) | `suyu/game_export.cpp:90-~133 (MakeIconDib), 3222-~3257 (`#ifdef _WIN32` resource update)` | PE icon embedding | n/a on Linux: a Linux export writes `icon.bmp` + `.desktop`/`icon.png` instead (block after the `#endif`, `if (linux_pkg)` ~3258); not read in full |
| 47 | Export (Qt) | `suyu/game_export.cpp:1542-1773, 2707-2757` | Visual Studio discovery (`vswhere`, `vcvars64.bat`, `cl.exe`/`link.exe`) | done (Linux `cc/gcc/clang` path at 1777-1863, 2246-2269; `#if defined(_WIN32) || defined(__linux__)` link-kit at 2671-2685) |
| 48 | Export (Qt) | `suyu/game_export.cpp:1761, 2061, 2824, 3163-3164, 3636-3643` | `static_launcher.exe` naming | done (names differ per OS) but see note below |
| 49 | Export (Qt) | `suyu/game_export.cpp:1492-1533 (ExportBuildJobs; Windows 1496-1501, Linux 1502), 3705, 3711` | RAM-aware `--parallel` via `GlobalMemoryStatusEx`; `SetThreadExecutionState` sleep inhibit | done (RAM) / missing (sleep inhibit); see change (6) |
| 50 | Common | `common/windows/timer_resolution.{h,cpp}`, `common/CMakeLists.txt:159-161` | Windows timer resolution API | n/a |
| 51 | Common | `common/nvidia_flags.cpp:14-27` | `__GL_SHADER_DISK_CACHE_*` env via `_putenv`, Windows only | missing (no-op on Linux) |
| 52 | Common | `common/logging.cpp:188-200, 313-320, 360-380` | colour console via `SetConsoleTextAttribute`, `OutputDebugStringW` backend | done (ANSI backend), debugger sink n/a |
| 53 | Common | `common/log_redact.cpp:55-62, 80-130` | Windows user/computer name, case-folding, `\` | done (`HOME`/`USER` branch) |
| 54 | Common | `common/device_power_state.cpp:6, 34`; `memory_detect.cpp:4, 28`; `host_memory.cpp:7, 78`; `virtual_buffer.cpp:7`; `dynamic_library.cpp:9-107`; `fs/*`; `thread.cpp` | upstream-style `_WIN32` splits | done |
| 55 | Common | `common/minicoro.h:419-530` | Windows fibres / VMEM allocator | done (third-party, other backends) |
| 56 | Common | `common/signal_chain.h:6` | `#ifndef _WIN32` only | n/a |
| 57 | Core tools | `core/tools/renderdoc.cpp:21-31` | `GetModuleHandleA("renderdoc.dll")` | done (`dlopen librenderdoc.so`) |
| 58 | Core tools | `core/tools/hactool_integration.cpp:34, 50` | `.exe` suffix, Windows install dirs | done |
| 59 | SSL | `core/hle/service/ssl/ssl_backend_schannel.cpp` (CMake lines 1343 commented, 1314 OpenSSL used) | Schannel backend | n/a (OpenSSL backend active) |
| 60 | Qt frontend | `suyu/hacker_environment.cpp:410-420` | Host info tree hard-codes `OS = Windows`, `GetSystemInfo` | done (non-Windows branch at ~145) |
| 61 | Qt frontend | `suyu/emulator_core_manager.cpp:40-150` | `retroarch.exe` discovery | done (Linux name at line 74) |
| 62 | Qt frontend | `suyu/external_decryption_tool.cpp:40-46` | `hactool.exe` / `Ryujinx.exe` names | done (alternate names listed) |
| 63 | Qt frontend | `suyu/main.cpp:296` | `Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)` | n/a |
| 64 | Qt frontend | `suyu/breakpad.cpp`, `startup_checks.cpp`, `debugger/console.cpp`, `social_sidebar.cpp`, `steam_integration.cpp`, `migration_worker.cpp`, `qt_common.cpp:22,35` | assorted `_WIN32` guards | done / n/a (not reviewed line by line) |
| 65 | Build | top-level `CMakeLists.txt:80,144-189,203-221,227,231,256,519-521,663-664,679-689` | MSVC runtime, bundled Qt/SDL3, `_WIN32_WINNT`, libretro `.dll`, crash dumps "unimplemented on Windows" | n/a |
| 66 | Build | `src/CMakeLists.txt:47,60-70,230`; `src/video_core/CMakeLists.txt:372-380` | MSVC flags; Nsight Aftermath hard-fails off Windows | n/a |
| 67 | externals | `externals/fsr-vulkan/CMakeLists.txt:58-71` | MSVC `/W0`; non-MSVC force-includes `src/ffx_1_1_4_portability.hpp` for Win32-only helpers (`wcscpy_s`) | done |
| 68 | externals | `externals/fsr-vulkan/suyu/ffx_vk_loader_shim.cpp:75-81, 86+` | Vulkan loader shim; stubs now `visibility("hidden")` on GCC/clang | done (no Windows APIs; MSVC unchanged), see change (5) |
| 69 | Scripts | `scripts/clean-boost.bat`, `clean-boost.ps1`, `fix-vcpkg-build.ps1` | PowerShell / batch tooling | missing (Linux has `fix-vcpkg-deps.sh` only for vcpkg) |
| 70 | Docs | `docs/VCPKG_BUILD_FIX.md`, `VCPKG_OVERLAY_FIXES.md`, `WORKFLOW_FIXES.md`, `Build.md` | Windows/vcpkg-oriented instructions | not audited line by line |

Entry count: 70 rows (about 55 distinct files). Genuinely missing on Linux: rows 1-3, 10-13, 36, 39, 49 (sleep inhibit), 51, 69. (Row 46 icon: Linux uses `icon.bmp`/`.desktop` instead.)

---

## 1. FSR 4 bridge (D3D12 + AMD driver ffx loader)

This is the largest self-contained Windows-only piece. Nothing here has a Linux implementation; on Linux the renderer falls back to FSR 3.1.4 (the portable Vulkan backend from `externals/fsr-vulkan`).

### 1.1 How it is gated

- `video_core/CMakeLists.txt:364-369`: `nvn_fsr4.cpp/.h` are added only `if (WIN32)`, with `externals/ffx-api/include` and `-DSUYU_HAS_FSR4_BRIDGE=1`. The comment (lines 362-363) states the DLLs are loaded at run time so nothing is linked.
- `nvn_fsr4.cpp:4` states "Windows only"; `nvn_fsr4.h:5` documents the System32 `amd_fidelityfx_dx12.dll`.
- `nvn_vk.cpp:97, 1371, 1380, 3133, 3141, 7353-7358, 7401, 7577, 9921, 10682, 11274`: every use is under `#ifdef SUYU_HAS_FSR4_BRIDGE`. Without it, `Fsr4Init()` sets `fsr4_reason = "this build has no FSR 4 bridge (Windows only)"` (line 7354) and uses FSR 3.1.4. So the Linux build is functionally correct, just without FSR 4.
- `vulkan_device.cpp:1022-1038`: only under `_WIN32` and only when `nvn_fsr` is on and `nvn_fsr_backend == 4`, enables `VK_KHR_external_memory_win32` and `VK_KHR_external_semaphore_win32`; sets `fsr4_interop`. `vulkan_device.h:284-286` exposes `HasFsr4Interop()`. `nvn_vk.cpp:7357-7358` checks it.

### 1.2 Exact Windows APIs used in `nvn_fsr4.cpp`

Headers (lines 7-13, 26-28): `<windows.h>`, `<d3d12.h>`, `<dxgi1_4.h>`, `ffx_api/dx12/ffx_api_dx12.h`.

Module loading and paths:
- `GetSystemDirectoryW` (line 52) builds `C:\Windows\System32\<file>` (`SystemPath`, lines 50-55).
- `LoadLibraryExW(L"version.dll", ..., LOAD_LIBRARY_SEARCH_SYSTEM32)` (67) + `GetProcAddress` for `GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `VerQueryValueW` (69-71) to log the DLL file version.
- `LoadLibraryExW(<System32>\amd_fidelityfx_dx12.dll, ..., LOAD_WITH_ALTERED_SEARCH_PATH)` (402); `GetProcAddress` for `ffxCreateContext`, `ffxDestroyContext`, `ffxQuery`, `ffxDispatch` (404-407). Comment at line 400: System32 only, a copy next to the exe is not picked up.
- `LoadLibraryExW(L"d3d12.dll")` and `L"dxgi.dll"` with `LOAD_LIBRARY_SEARCH_SYSTEM32` (419-420); `GetProcAddress` for `CreateDXGIFactory1` (423) and `D3D12CreateDevice` (425).
- `GetLastError` (403), `HMODULE` members (165-167).

Adapter matching:
- Vulkan side: `VkPhysicalDeviceIDProperties::deviceLUID` / `deviceLUIDValid` (416-418).
- DXGI side: `CreateDXGIFactory1`, `IDXGIFactory4::EnumAdapterByLuid` (431), then `D3D12CreateDevice` on that adapter (444).

D3D12 objects:
- `CreateCommandQueue` (447, DIRECT queue), `CreateFence(0, D3D12_FENCE_FLAG_SHARED)` (449), `CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT)` (455, ring), `CreateQueryHeap` + `CreateCommittedResource` for timestamp heap/readback (473-474), `ResolveQueryData(D3D12_QUERY_TYPE_TIMESTAMP)` (677), `queue->Wait(fence, v)` (681), `ExecuteCommandLists` (685), `queue->Signal(fence, v)` (687), `fence->Signal` (712).
- Events: `CreateEventW` (451), `WaitForSingleObject` (326), `CloseHandle` (267, 278, 291, 363, 530, 539).

Shared resources (D3D12 -> Vulkan):
- `CreateCommittedResource(..., D3D12_HEAP_FLAG_SHARED, ..., D3D12_RESOURCE_STATE_COMMON)` (246) with simultaneous-access flag (error text line 247).
- `CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &handle)` (250) gives an NT handle.
- Vulkan import: `VkExternalMemoryImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT` (253), `vkGetMemoryWin32HandlePropertiesKHR` (274, loaded 394), `VkImportMemoryWin32HandleInfoKHR` with the same handle type (283). Handle is closed after import (291: NT-handle import does not take ownership).

Shared fence -> Vulkan timeline semaphore:
- `CreateSharedHandle(fence, ...)` (522), `vkImportSemaphoreWin32HandleKHR` (538, loaded 395) with `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT` (536). `timeline` is the imported timeline semaphore the renderer waits on / signals.

FidelityFX: `ffxCreateContext` (588), `ffxDispatch` (674), `ffxQuery` (493, 501, 739, 752), `ffxDestroyContext` (314); provider list queried to pick the highest FSR 4.x.

### 1.3 Why Windows-only

The AMD driver's `amd_fidelityfx_dx12.dll` is a D3D12 ffx-api loader that only exists in the Windows Adrenalin driver package; the bridge feeds it D3D12 views of Vulkan images. Handle types (`D3D12_RESOURCE_BIT`, `D3D12_FENCE_BIT`), `*_win32` extensions, LUIDs and NT handles have no Linux equivalents.

### 1.4 Linux options (suggestions, not implemented)

- Cheapest and already working: keep FSR 3.1.4 (`SUYU_FSR_VULKAN`).
- A real FSR 4 path needs a non-D3D12 FSR 4 provider. This repo contains none; whether one is obtainable on Linux is unverified. Other Linux-side interop (if a Vulkan-native provider ever appears) would use `VK_KHR_external_memory_fd` / `VK_KHR_external_semaphore_fd` with `VK_EXTERNAL_*_HANDLE_TYPE_OPAQUE_FD_BIT` and `dlopen` in place of `LoadLibraryExW`/`GetProcAddress`; device matching would use `deviceUUID` (`VkPhysicalDeviceIDProperties`) instead of the LUID.
- Running the D3D12 path under a D3D12-on-Vulkan layer (e.g. VKD3D-Proton inside Wine/Proton) is a separate deployment option, not a port of this code.

---

## 2. NVN Vulkan renderer (`video_core/renderer_vulkan/nvn_vk.cpp`)

- **49-52** `<windows.h>`, `<psapi.h>`, `<dbghelp.h>` under `_WIN32`. Needed for rows below. Linux: nothing to include.
- **102-105** `NVN_VK_NOINLINE` uses `__declspec(noinline)` on MSVC, `__attribute__((noinline))` otherwise. Done.
- **112-127** `SehRun(fn, arg)`: Windows version wraps the call in `__try/__except(EXCEPTION_EXECUTE_HANDLER)` and returns false on an access violation (used at 1571, 3760, 3783, 4287, 4347 so the renderer can switch itself off). Linux version just calls `fn(arg)` and returns true, so a bad guest pointer crashes the process instead of disabling the renderer. Linux equivalent: `sigsetjmp` + a `SIGSEGV`/`SIGBUS` handler on an alternate stack scoped to the call (note `src/common/signal_chain.h` and dynarmic's fastmem also install SIGSEGV handlers, so chain, do not replace).
- **1417-1424** `Fatal()`: `MessageBoxA(... MB_TOPMOST)` + `TerminateProcess` on Windows; Linux prints to stderr and `std::_Exit(3)`. Done; a GUI dialog (SDL `SDL_ShowSimpleMessageBox`) would be the parity option.
- **6310-6320** `WriteStats()` memory line: `GetProcessMemoryInfo` (`PROCESS_MEMORY_COUNTERS_EX`), `GlobalMemoryStatusEx`. Linux text is a fixed "not reported on this platform" string. Linux equivalent: parse `/proc/self/status` (`VmRSS`, `VmHWM`) and `/proc/meminfo`, or `getrusage`.
- **10323-10325, 10403-10405, 10418-10420** `MessageBeep(MB_*)` cues for the benchmark start/step/end. Linux: no sound. Equivalent: terminal bell `\a`, `SDL_` audio, or `canberra-gtk-play`.
- **11518-11623** crash recorder: `CrashBuildLine` reads PE headers (`IMAGE_DOS_HEADER`, `IMAGE_NT_HEADERS`: timestamp, SizeOfImage, checksum) via `GetModuleHandleW`/`GetModuleFileNameA`; `CrashWalkStack` uses `RtlVirtualUnwind` on `CONTEXT.Rip` (x64 only, `_M_X64`); `CrashWhere` uses `GetModuleHandleExA(FROM_ADDRESS)`; `CrashHandler` is a `LONG WINAPI` vectored handler filtering `0xc0000005`, using `GetCurrentThreadStackLimits`/`GetCurrentThreadId`, writing `nvn_vk_crash.txt`. Linux equivalent: `sigaction(SIGSEGV, SA_SIGINFO|SA_ONSTACK)` + `backtrace()`/`_Unwind_Backtrace`, `dladdr` for module+offset, ELF build-id (`dl_iterate_phdr`) instead of PE timestamp.
- **11641-11644** installs the handler with `AddVectoredExceptionHandler(1, CrashHandler)`. Linux: install the sigaction above.
- **11532, 11581** (approx.; CrashBuildLine 11524, CrashWalkStack 11541, CrashWhere 11569, CrashHandler 11590) path-splitting treats `\` and `/`; harmless on Linux.
- **690** comment says `QueryPerformanceCounter`; the profiling code itself was not found calling it directly in this grep (only the comment), so treat as already portable.
- **752, 791, 10319** text references `tools\perf-summary.ps1`. `tools/` in the repo has no such file (listing: `a32recomp`, `clang-format.sh`, `cpm`, ...). A Linux counterpart (shell/python) would have to be written once the file format is known.
- **Hotkeys** (792, 5824, 6282, 6288, 6857, 10352, 10436, 10442, 11692): go through `Common::HostKeyDown`, so portable in code, but see section 3 for coverage.

Related Vulkan files:
- `vulkan_common/vulkan.h:10-11` (`VK_USE_PLATFORM_WIN32_KHR`) and `vulkan_surface.cpp:22-35` (`vkCreateWin32SurfaceKHR`): other platforms already have branches (Metal, Android, X11/Wayland via the generic path). Done.
- `vulkan_wrapper.cpp:565-573`: `vkGetMemoryWin32HandleKHR` (upstream external-memory export). Windows only; Linux would use `vkGetMemoryFdKHR`. Only relevant if something calls it.
- `vk_texture_cache.cpp:1452-1458`: on Windows only, linear-image copies with mismatched block size are skipped (device loss on Mario + Rabbids). Driver workaround; Linux intentionally does not take it.
- `vk_staging_buffer_pool.cpp:30-40`: stream buffer 256 MiB on Windows/Android, 128 MiB elsewhere. Intentional tuning.
- `vk_graphics_pipeline.cpp:35`: `_MSC_VER && NDEBUG` optimisation pragma. n/a.

---

## 3. Hotkeys (`src/common/host_key.*`) - routed through one API

- All former `GetAsyncKeyState` polls (`nvn_vk.cpp`, `core/arm/recomp/game_settings.cpp`) now call `Common::HostKeyDown(vk)`. Remaining `GetAsyncKeyState` in `src/`: only `host_key.cpp:21` (grep-verified).
- `host_key.h:6-31` defines the Windows VK constants **only when `!_WIN32`** (`VK_CONTROL`, `VK_NUMPAD0-9`, `VK_MULTIPLY/ADD/SUBTRACT/DECIMAL`, `VK_F1-F9`); on Windows they come from `<windows.h>`. `HostKeyDown(int windows_vk)` / `SetHostKeyPoller` declared in `namespace Common`.
- `host_key.cpp:17-22` Windows: `GetAsyncKeyState(vk) & 0x8000` (global, works unfocused); `SetHostKeyPoller` is a no-op. Lines 23-35 elsewhere: atomic `HostKeyPoller`, false if none registered.
- Registered only in `suyu_cmd/emu_window/emu_window_sdl2.cpp:640` (`Common::SetHostKeyPoller(&HostKeyPollSDL)` in the ctor; poller 606-637, SDL scancodes, KP_* independent of NumLock, Ctrl via keyboard state). Sees keys only while the SDL window has focus (Windows poll is global). **Qt GUI (`src/suyu`) has no poller** (grep: no other caller), so debug/bench hotkeys are dead in the Qt build on Linux. No effect on Windows.
- Callers: `nvn_vk.cpp` (list in section 2), `game_settings.cpp:1377, 1378, 1727`. `nvn_vk.cpp:5824` and `:11692` pass raw `0x77` (F8) / `0x78` (F9); same numbers on both OS.

---

## 4. Recompiler / ARM recomp (`src/core/arm/recomp`, `src/core/recompiler`)

Already ported (each has an explicit non-Windows branch):
- `mod_host.cpp:293-334`: Windows `LoadLibraryExW(... LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | DEFAULT_DIRS)` + `GetProcAddress("modhost_attach")`; Linux `dlopen(RTLD_NOW)`/`dlsym`, and a `.dll` -> `.so` rewrite of the configured name (lines 293-299). Default loader `Forge/forge.dll` vs `Forge/forge.so` at `game_settings.h:64-65`, `game_settings_ini.cpp:14-15`, `suyu_cmd/game_menu.cpp:965-969`.
- `mod_host_api.h:37-38`: `MODHOST_EXPORT` is `__declspec(dllexport)` on Windows.
- `game_settings.cpp:313-325`: `DesktopMode` queries `EnumDisplaySettingsW`; on Linux the values come from SDL (`suyu_cmd/suyu.cpp:922-928`).
- `core/recompiler/arm64_to_c.h` (generator for the exported game's C code): `_mkdir`/`<direct.h>` vs `<sys/stat.h>` (32-36); generated CMake builds per-module `recompiled_*` shared libraries (3604-3614); `RECOMP_API` is `__declspec(dllexport)` vs `visibility("default")` (3665-3667); `__forceinline` vs `always_inline` (3812-3813); `Sleep` vs `nanosleep` in the SleepThread SVC and the 4096-block yield (4365-4370, 4490-4495); `MemoryBarrier` vs `__atomic_thread_fence` (4457-4459); `PATH_SEP '\\'` in generated tooling (4027-4034).
- `a32/a32emit.h:192-230`: `if(MSVC)` blocks in generated CMake; GCC/Clang flags in the else.

Stubbed / missing:
- `host_threads.cpp:11-70`: `Describe()` is implemented with `CreateToolhelp32Snapshot`, `Thread32First/Next`, `OpenThread(THREAD_QUERY_LIMITED_INFORMATION)`, `GetThreadTimes`, and `GetThreadDescription` resolved from `KernelBase.dll` via `GetProcAddress`. The `#else` stub (around line 96) returns an empty string. Linux equivalent: iterate `/proc/self/task/*/{stat,comm}` (utime/stime ticks, thread name).
- `core/perf_stats.cpp:95-107` `ProcessCpuSeconds()` returns 0.0 off Windows. Equivalent: `getrusage(RUSAGE_SELF)` or `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`.
- `core/hle/kernel/stall_ring.h:20, 70-82` `ThreadCycles()` uses `QueryThreadCycleTime` (declared `__declspec(dllimport)` at line 20), returns 0 otherwise. Equivalent: `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` (time, not cycles) or `perf_event_open` (cycles, needs permissions).
- `core/core_timing.cpp:17-19, 412-416`, `common/windows/timer_resolution.cpp`: timer resolution control (Windows scheduler tick). `core_timing.h` carries a `timer_resolution_ns` member only on Windows. n/a on Linux (high-resolution timers by default); `suyu_cmd/suyu.cpp:931-934` calls it only on Windows.
- `suyu_cmd/suyu.cpp:744-790`: "compatibility path" that loads `recompiled_rtld.dll`, `recompiled_image.dll`, `recompiled_subsdk0-9.dll`, `recompiled_sdk.dll` next to the exe with `LoadLibraryW`, resolving `recomp_image_lookup` and `recomp_image_set_base` via `GetProcAddress` (lines 767-776). There is no `#else` with `dlopen` for the corresponding `lib*.so` files that the generated CMake also produces on Linux; Linux relies on the static link variant (`SUYU_CMD_RECOMP_DIR`, `suyu_cmd/CMakeLists.txt:45-60`, `s_recomp_modules`). Equivalent: `dlopen` + `dlsym` of the same two exports.

---

## 5. suyu-cmd (`src/suyu_cmd`)

- `suyu.cpp:86-92`: `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` exported data symbols for laptop GPU selection. n/a on Linux (use `DRI_PRIME=1` / `__NV_PRIME_RENDER_OFFLOAD=1` env, or `prime-run`).
- `suyu.cpp:415-420`: attach to parent console and reopen `CONOUT$`. n/a.
- `suyu.cpp:435-445, 490-500, 513-523, 635-640, 660-665, 673-705, 836-845`: wide-char exe path, `CommandLineToArgvW`, UTF-16 -> UTF-8. Done (POSIX branches present).
- `suyu.cpp:473-477`: keys under `%APPDATA%\suyu\keys` vs `$XDG_DATA_HOME/suyu/keys`. Done.
- `suyu.cpp:857-870`: sets `DISABLE_RTSS_LAYER=1` to keep the RivaTuner Vulkan layer (`RTSSVkLayer64.dll`) out unless `[Debug] rtss_overlay` is true. n/a.
- `emu_window_sdl2.cpp:34-60, 84-89, 444+, 710-716`: F12 debug panel built from Win32 controls (IDs `kIdStatus`..`kIdClose`, `ShowDevMenu` at 444, folder buttons via `ShellExecuteW(L"explore")` at 88). F12 is only dispatched under `_WIN32` (710-716). Linux equivalent: open folders with `xdg-open` / `QDesktopServices::openUrl`; panel could be an ImGui window since `game_menu.cpp` already uses ImGui.
- `emu_window_sdl2.cpp:1026-1075`: shader-compile progress as a separate Win32 window (`HWND g_shader_box`, `GetWindowRect`, `DestroyWindow`). Linux: title-bar text only (the common part, lines 1052-1058).
- `emu_window_sdl2.cpp:1082-1100`: window icon from the exe's PE resource id 1 (`LoadImageW`) when `g_native_export_mode`. Linux: uses the embedded suyu icon; the game icon is not embedded in a Linux export (see section 6).
- `emu_window_sdl2_vk.cpp:10-11, 33, 67-72`: `SDL_PROP_WINDOW_WIN32_HWND_POINTER`; X11 and other branches follow (line 73+). Done.
- `game_menu.cpp:1534-1536`: "F12: debug panel" hint shown only on Windows; consistent with the missing panel.
- `game_menu.cpp:1618-1620`: Windows font path `C:\Windows\Fonts\segoeui.ttf`; Linux list of DejaVu/Noto paths follows. Done.
- `relaunch.cpp`, `multiplayer.cpp:64-68`: both OS branches present. Done.

---

## 6. Game export (`src/suyu/game_export.cpp`)

Linux support is substantial (compiler discovery, static launcher link, `cmake` + `ninja` paths). Remaining Windows-only items:
- **86-129, 3221-3256**: `MakeIconDib` builds an RT_ICON DIB; the surrounding block updates the copied launcher's resources with the game icon (Windows resource-update API, per the comment at 3221). No Linux equivalent: a Linux export has no embedded icon. Equivalent: write a `.desktop` file + PNG alongside the binary, or set the icon via the AppImage/`.desktop` metadata.
- **1536-1773, 2705-2757**: Visual Studio discovery (`vswhere.exe`, `vcvars64.bat`, `cmd.exe /c call ... &&`, `cl.exe`, `link.exe`, `cmake.exe` under VS). Linux counterpart at 1777-1863 and 2246-2269 (finds `cc/gcc/clang`, prints distro install commands).
- **1496-1513**: RAM query for build job count: `GlobalMemoryStatusEx` vs a `__linux__` branch at 1502. Done.
- **3702-3712**: `ExportRunGuard` calls `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED)` to keep the PC awake during a long export. Linux: not inhibited. Equivalent: D-Bus `org.freedesktop.login1.Inhibit` / `systemd-inhibit`, or `QDBus` on `org.freedesktop.ScreenSaver`.
- **Naming inconsistency to check**: Windows launcher is `static_launcher.exe` (1761, 2061, 2824); Linux link step produces `static_launcher` (1852, 2827). The AOT-cache reuse test at line 2061 looks only for `static_launcher.exe` even though it is not under `_WIN32`; on Linux this check is likely never true (the cache is never "reused"), and `exe_suffix` at 3164 suggests a variable suffix is already used elsewhere. Verify before relying on cache reuse on Linux.
- **2507-2513**: output globbing lists `*.exe`, `*.dll`, `*.so`, `*.dylib`; fine.
- **3639-3643**: package launcher name gets `.exe` only on Windows. Done.

---

## 7. src/common

Upstream-style dual-path files (Windows branch + POSIX branch), no action: `atomic_helpers.h`, `cpu_features.cpp`, `demangle.cpp`, `device_power_state.cpp`, `dynamic_library.cpp`, `error.cpp`, `fs/file.cpp`, `fs/path_util.cpp`, `fs/symlink.cpp`, `host_memory.cpp`, `logging/*`, `memory_detect.cpp`, `net/net.cpp`, `settings.cpp`, `steady_clock.cpp`, `string_util.cpp`, `thread.cpp`, `virtual_buffer.cpp`, `minicoro.h`.

Items with a real gap or Windows-only meaning:
- `windows/timer_resolution.{h,cpp}` (`common/CMakeLists.txt:159-161`, `if(WIN32)`): `GetMinimum/Maximum/CurrentTimerResolution`, `SetCurrentTimerResolution[ToMaximum]`, `SleepForOneTick`. n/a on Linux.
- `nvidia_flags.cpp:14-27`: creates `<shader dir>/nvidia` and sets `__GL_SHADER_DISK_CACHE_PATH`, `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1`, `__GL_THREADED_OPTIMIZATIONS=1` via `_putenv`; whole body is `#ifdef _WIN32`, so on Linux nothing is set. Equivalent: `setenv(..., 1)` with the same names (the NVIDIA Linux driver honours `__GL_*` variables; whether it is desirable for Vulkan-only use is untested here).
- `logging.cpp:188-200`: `ColorConsoleBackend` uses `GetStdHandle` + `SetConsoleTextAttribute`; the Linux class (ANSI colours) is the `#else` branch. `313-320, 360-380`: `DebuggerBackend` (`OutputDebugStringW`) is Windows only; Linux equivalent is the console/file backends, no change needed.
- `log_redact.cpp:55-62, 80-130`: Windows folds case and `/`->`\`, reads `USERPROFILE`, `USERNAME`, `GetComputerNameW`; Linux branch uses `HOME`, `LOGNAME`/`USER` (line ~133-150). Done.
- `signal_chain.h:6`: `#ifndef _WIN32` (POSIX-only helper); n/a.

---

## 8. Core service / tools

- `core/hle/service/ssl/ssl_backend_schannel.cpp` is Windows-only (`SCHANNEL_CRED`); `core/CMakeLists.txt:1314` builds the OpenSSL backend and lines 1343 (Schannel) and 1339 (SecureTransport) are commented out, so Linux is unaffected.
- `core/tools/renderdoc.cpp:21-31`: Windows `GetModuleHandleA("renderdoc.dll")`; Linux `dlopen("librenderdoc.so", RTLD_NOW|RTLD_NOLOAD)`. Done.
- `core/tools/hactool_integration.cpp:34, 50`: `.exe` suffix and Windows install dirs, with POSIX alternatives. Done.
- `core/game_settings.cpp:77-78`: `DetectOS()` returns `OS::Windows` under `_WIN32`; Linux branches follow. Done.

---

## 9. Qt frontend (`src/suyu`)

All use `_WIN32` guards with POSIX alternatives unless noted; counts of guards per file: `main.cpp` 24, `emulator_core_manager.cpp` 6 (retroarch.exe vs retroarch, line 71-75), `hacker_environment.cpp` 3 (host info tree: Windows hard-coded `OS = Windows`, line 414, non-Windows branch near 145), `startup_checks.cpp` 4, `breakpad.cpp` 2, `debugger/console.cpp` 2, `social_sidebar.cpp` 2, `qt_common.cpp` 2 (`WindowSystemType::Windows`, lines 22 and 35), `migration_worker.cpp` 1, `steam_integration.cpp` 1, `util/util.cpp` 1, `external_decryption_tool.cpp` 1 (`hactool.exe` / `Ryujinx.exe` name tables, lines 40-46), `main.cpp:296` imports `QWindowsIntegrationPlugin`. `gamer_environment.cpp:2214, 2237` uses a "Windows NT 10.0" HTTP User-Agent string (cosmetic). These files were only surveyed by guard count and a few spot checks, not read line by line.

---

## 10. Build system, scripts, docs

- Top-level `CMakeLists.txt`: MSVC runtime selection (179-185), MSVC-gated bundled Qt/SDL3 (80, 203-218), `ENABLE_OPENGL` off on WIN32+ARM64 (227), libretro core named `suyu_libretro.dll` (231), `_WIN32_WINNT=0x0A00` (519-521), crash dumps "unimplemented on Windows" (663-664), MSVC+Ninja adjustments (679-689). Linux builds ignore all of these.
- `src/CMakeLists.txt:47, 60-70, 230` and `src/suyu_cmd/CMakeLists.txt:132-165`: MSVC options, `WIN32_LEAN_AND_MEAN`, MSVC bundled SDL3 copy.
- `src/video_core/CMakeLists.txt:372-380`: `ENABLE_NSIGHT_AFTERMATH` is a hard `FATAL_ERROR` off Windows.
- `externals/fsr-vulkan/CMakeLists.txt:58-71`: non-MSVC builds force-include `src/ffx_1_1_4_portability.hpp` to replace upstream Win32-only helpers (e.g. `wcscpy_s`). Done. `externals/fsr-vulkan/suyu/ffx_vk_loader_shim.*` has no Windows API usage.
- `scripts/`: `clean-boost.bat`, `clean-boost.ps1`, `fix-vcpkg-build.ps1` are Windows scripts; only `fix-vcpkg-deps.sh` is the POSIX counterpart for vcpkg. `docs/a32recomp/INSTALL-LINUX.md` exists as a Linux install doc. `docs/VCPKG_BUILD_FIX.md`, `VCPKG_OVERLAY_FIXES.md`, `WORKFLOW_FIXES.md` were not read in detail; assume Windows/vcpkg orientation.

---

## Suggested porting order (by user-visible impact)

1. Crash recorder (`nvn_vk.cpp` 11518-11644) and `SehRun` for Linux (SIGSEGV handling) so renderer failures are diagnosable and survivable.
2. Qt frontend: register a `SetHostKeyPoller` (hotkeys dead in the Qt build on Linux; `QKeyEvent`/`QGuiApplication::queryKeyboardModifiers` or an event filter).
3. `suyu.cpp:744-790`: add a `dlopen` branch for the per-module shared libraries.
4. Cheap stubs to real values: `ProcessCpuSeconds`, `ThreadCycles`, `HostThreads::Describe`, `WriteStats` memory line (all via `/proc` or `clock_gettime`).
5. `nvidia_flags.cpp` env vars, F12 panel, benchmark beep, `perf-summary` script, export icon and sleep-inhibit.
6. FSR 4: no direct port; keep the FSR 3.1.4 fallback. The only realistic native-Linux route (Wine helper process hosting the D3D12 half of `nvn_fsr4.cpp`) is planned in `FSR4-LINUX-PLAN.md` (research only, nothing implemented or run).

---

## Linux-session changes that touch shared/Windows code (verify on Windows)

None of these were compiled or run on Windows. Line numbers are as of 2026-10-20.

1. **`Common::HostKeyDown` (new `src/common/host_key.h/.cpp`, `common/CMakeLists.txt:70-71`)**; call sites `nvn_vk.cpp` (include line 10; uses at 792, 5824, 6282, 6288, 6857, 10352, 10436, 10442, 11692) and `core/arm/recomp/game_settings.cpp` (include 28; uses 1377, 1378, 1727). `_WIN32` guards and the `__declspec(dllimport) GetAsyncKeyState` declarations were removed there.
   Static read of the Windows path: `host_key.cpp:3-10` defines `WIN32_LEAN_AND_MEAN`/`NOMINMAX` (guarded) and includes `<windows.h>`, so `GetAsyncKeyState` resolves (winuser.h is kept by LEAN_AND_MEAN). `host_key.h` defines `VK_*` only under `#ifndef _WIN32`, so no macro clash with `<windows.h>`. **But `host_key.h` does not itself include `<windows.h>`**: on Windows the `VK_*` names used in `nvn_vk.cpp` (`windows.h` at line 50, after the host_key include at 10, used much later) and `game_settings.cpp` (`windows.h` at line 24, include at 28) rely on those TUs including `<windows.h>` first/anywhere before use. Risk: a new includer without `<windows.h>` gets "VK_F4 undeclared". Verify: build `core` and `video_core` on MSVC; check `GetAsyncKeyState` unresolved-symbol free link (common now provides it, `common` must be linked by both - it is). Behaviour must be identical to before (global async key state).
2. **Queue mutex**: `vk::Queue` gained `std::mutex* mutex` + `SetMutex` and a RAII `Lock` in `Submit`, `Submit2`, `Present` (`vulkan_common/vulkan_wrapper.h:826-873`, `#include <mutex>` at 12); `Device::queue_mutex` (`vulkan_device.h:1099`) and wiring in `vulkan_device.cpp:777-784`. `graphics_queue` ALWAYS gets the mutex; `present_queue` gets it when `present_family == graphics_family`; `nvn_queue` gets it only when it aliases the graphics queue (`!nvn_queue_separate`, i.e. no second queue; a second queue is requested at `vulkan_device.cpp:1639` when the family has `queueCount >= 2`, typical on NVIDIA/AMD Windows, so there NVN uses its own unlocked queue). Consequence on Windows: every graphics `Submit/Submit2/Present` (`vk_master_semaphore.cpp:194,232,288,323`, `vk_present_manager.cpp:525`, `vk_swapchain.cpp:256`, `vk_turbo_mode.cpp:223`) now takes a mutex. It is uncontended while a single thread uses the queue; it can contend between the scheduler/worker thread (submit) and the present thread (present) because they were previously unserialised. **Risk to verify:** `vkQueuePresentKHR` blocking (FIFO/vsync, some drivers) while holding the lock would stall submits; measure frame time / GPU idle vs the previous build. `Device` now contains a `std::mutex`, so it is implicitly non-copyable/non-movable; I found no copy in `video_core` by grep but only compiled on Linux/GCC, so confirm MSVC builds (`vulkan_device.h:224` ctor, no explicit delete).
3. **NVN `Renderer` ctor gate** (`nvn_vk.cpp:1232-1240`): the renderer is no longer disabled when `!device.HasSeparateNvnQueue()`; only `IsKhrPushDescriptorSupported()` gates it (the diagnostic text at 1235-1236 still prints the separate-queue flag). Windows drivers with two queues take the same path as before; verify NVN still enables on your Windows GPU(s) and that a single-queue Windows GPU (some Intel iGPUs) now enables it too (previously off), sharing the graphics queue through the mutex.
4. **`nvn_mv_lag` default 0 -> 1**: `core/arm/recomp/game_settings.cpp:693-700` (`NvnMvLag`, `v.empty() ? 1`), doc in `game_settings.h:222`, ini template `game_settings_ini.cpp:97` (`"1"`). The Windows ini already sets lag 1 explicitly (per the session notes; not re-verified), so only games/inis without the key change behaviour (motion-vector camera record choice, FSR/jitter path). Verify other titles' ghosting/MV alignment with the F2 dump.
5. **`externals/fsr-vulkan/suyu/ffx_vk_loader_shim.cpp:75-81` + every `vk*` stub from line 86**: stubs carry `SUYU_FFX_SHIM_HIDDEN` = `visibility("hidden")` only under `__GNUC__`/`__clang__`; empty on MSVC, so the MSVC build is textually the same as before. Verify nothing else (e.g. clang-cl, which defines `__clang__`) is broken: hidden visibility on `clang-cl` is accepted and harmless; Windows exes do not export these anyway.
6. **`game_export.cpp` `ExportBuildJobs()` (1492-1533)**: RAM-aware job count (`min(threads, available RAM / 3 GiB)`, at least 1), `SUYU_EXPORT_JOBS` override, falls back to `Common::GetMemInfo().TotalPhysicalMemory` (`#include "common/memory_detect.h"` at 51; `memory_detect.h`). Used for both builds: module build `--parallel` (2463-2464, in the generic path, so MSVC/VS-generator uses it) and the static-launcher link (2793-2794). Windows branch uses `GlobalMemoryStatusEx` `ullAvailPhys` (1496-1501), so low free RAM on Windows now lowers parallelism vs the old behaviour (unverified what the old count was). Verify the MSVC export still completes and is not much slower; set `SUYU_EXPORT_JOBS` to compare.
7. **Build system**: `option(YUZU_WERROR ... ON)` (`CMakeLists.txt:76`); gated `-Werror=*` in `src/CMakeLists.txt:158, 175` and `src/video_core/CMakeLists.txt:395` (default ON = same as before; the Linux scripts pass OFF). `src/CMakeLists.txt:269` adds `shader_scan`; its `--start-group` link group for the `pipeline-test` target is `src/shader_scan/CMakeLists.txt:13-15` under `if (UNIX AND NOT APPLE)` (verified; Windows takes the plain link at line 17, which may hit the core/frontend_common cyclic-static-lib problem on MSVC only if that tool needs it - MSVC resolves static-lib cycles itself) and `core/CMakeLists.txt:1329-1332`: `$<LINK_GROUP:RESCAN,...>` only when `CMAKE_VERSION < 4.0 AND NOT MSVC`, else plain `OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB` (so MSVC takes the plain branch; that is a change to verify if it was previously linked differently). `src/suyu/CMakeLists.txt:444-448`: `Qt6::GuiPrivate` / `Qt6Gui_PRIVATE_INCLUDE_DIRS` only under `UNIX AND NOT APPLE`. Verify: configure + full build on Windows, and that `shader-scan` builds (or is skipped) on MSVC.
8. **Repo-root scripts (Linux only, not used on Windows)**: `build.sh` (cmake/ninja, passes `-DYUZU_WERROR=OFF -DENABLE_QT=ON ...`) and `relink.sh` (rebuilds `suyu-cmd-static` against an AOT export in `build-aot/`, RAM-aware jobs). Build dirs `build/`, `build-aot/`, `build-ninja/` and `relink.log` in the repo root are Linux artefacts; do not reuse them from Windows.
9. **Linux-only behaviours/settings**: native GPU on RADV (single graphics queue, so NVN shares it via the mutex, see 2/3). FSR backend 4 (`nvn_fsr_backend = 4`, read at `game_settings.cpp:650`) is not available on Linux: no `SUYU_HAS_FSR4_BRIDGE` (`video_core/CMakeLists.txt:364-369` is `if (WIN32)`), `Fsr4Init` falls back to FSR 3.1 (`nvn_vk.cpp:7353-7358`). Plan for a Wine-helper FSR 4 on Linux: `FSR4-LINUX-PLAN.md` (repo root; research only). Linux exports also write `icon.bmp`/`.desktop` (see row 46), unrelated to Windows.

## Windows rebuild checklist (priority order)

1. Delete/ignore the Linux build dirs; fresh CMake configure on MSVC (default `YUZU_WERROR=ON`). Confirm the new files are picked up (`common/host_key.cpp` -> `common`).
2. Compile `common` (`host_key.cpp`), then `video_core` (`nvn_vk.cpp`, `vulkan_wrapper.h` users, `vulkan_device.cpp`, `nvn_fsr4.cpp`) and `core` (`game_settings.cpp`) first: these hold the shared-code edits (items 1, 2, 4). Watch for `VK_*` undeclared and for any `Device` copy error (`std::mutex`).
3. Link `suyu-cmd` and `suyu` (Qt); check `shader-scan` target and the `LINK_GROUP` generator expression configure cleanly.
4. Run a game on a two-queue GPU: NVN renders as before, FSR 4 backend still reaches `Fsr4Init` (`HasFsr4Interop()`), hotkeys (F2-F9, numpad, Ctrl+Numpad1) still react globally.
5. Measure frame pacing with graphics-queue mutex (item 2): scheduler submit vs present thread.
6. Check `nvn_mv_lag` default effect on a game that has no explicit lag (item 4).
7. Run a game export on MSVC: job count log line "Export build: using N parallel jobs", build completes (item 6).
8. Optional: confirm items 3 and 5 (single-queue Windows GPU enabling NVN; shim on clang-cl).
