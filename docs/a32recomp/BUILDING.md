# Building suyu (A32 recompiler) from source on Windows

This builds `suyu.exe` (the Qt frontend with **File → Export Game...**) and `suyu-cmd.exe` from
this repository. Players don't need this: the prebuilt release and
[INSTALL.md](INSTALL.md) are enough. Upstream suyu/Eden build notes for other platforms are in
[../Build.md](../Build.md); this guide covers the Windows MSVC build this fork is tested with.

## 1. Install the tools (once)

| Tool | Version | Where | Notes |
|---|---|---|---|
| Git for Windows | any recent | https://gitforwindows.org | default options |
| Visual Studio 2022 | **17.14+** | https://visualstudio.microsoft.com/vs/community/ (or Build Tools 2022) | workload **Desktop development with C++**, keep *MSVC v143 (Latest)*, *Windows 11 SDK*, *C++ CMake tools for Windows* (CMake and Ninja come with it) |
| Vulkan SDK | 1.4.x (tested 1.4.341.1) | https://vulkan.lunarg.com/sdk/home#windows | default install under `C:\VulkanSDK`; or run `tools\windows\install-vulkan-sdk.ps1` as administrator |
| Qt 6 | tested 6.12.0 | Qt online installer, https://www.qt.io/download-qt-installer-oss | pick **MSVC 2022 64-bit** for your Qt 6 version (Qt SVG is part of it). Default location `C:\Qt` |
| Python 3 | 3.10+ | https://www.python.org/downloads/windows/ | tick **Add python.exe to PATH**; some dependency downloads and the packaging scripts use it |

Notes:

- The Qt installer needs a (free) Qt account. If Qt isn't under `C:\Qt\6.*`, set `QT_DIR`
  to its `msvc2022_64` folder, e.g. `set QT_DIR=D:\Qt\6.12.0\msvc2022_64`.
- The Vulkan SDK is needed for its shader compiler (`glslangValidator`); it's found through
  `VULKAN_SDK` or under `C:\VulkanSDK`.
- Disk: about 15 GB for the build tree and downloaded dependencies.

## 2. Get the source

```bat
git clone <this repository's URL> C:\suyu-src
```

A short path without spaces is safest. For release packages it **must** be outside
`C:\Users\` (see section 5).

## 3. Build

Double-click `tools\a32recomp\build_windows.bat`, or run it from a cmd window. It:

1. finds Visual Studio (through `vswhere`) and loads its x64 C++ environment;
2. checks for CMake, Ninja, Git, Python, the Vulkan SDK and Qt (with Qt SVG), and stops with
   a clear message if one is missing;
3. configures `build\` with Ninja in Release mode (the first configure downloads the
   dependencies: 10+ minutes);
4. builds `suyu` and `suyu-cmd` (30-90 minutes the first time, a few minutes after).

The result is `build\bin\suyu.exe`. Run it from there, not a copy: a suyu run from its build
tree uses that tree for exports. Output is logged to `build-log.txt` in the source root.

The same by hand, from a "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_TESTS=OFF ^
  -DYUZU_USE_BUNDLED_QT=OFF -DCMAKE_PREFIX_PATH=C:\Qt\6.12.0\msvc2022_64
cmake --build build --target suyu suyu-cmd
```

## 4. Exporting from a source build

A suyu built from source exports through its own build tree: the export configures
`suyu-cmd-static` with the game's recompiled modules and builds it with CMake. The steps are
as in [INSTALL.md](INSTALL.md) section 5. Close every suyu and game exe before rebuilding.

## 5. Making a release package

The prebuilt release has no build tree, so it carries a **link kit**: suyu-cmd-static's own
objects and libraries plus a linker response file. The export compiles the game, then links
it with the kit.

1. Clone or check out the release commit at a neutral path such as `C:\suyu-src` (the
   compiler embeds source paths into the binaries).
2. Run, from any cmd window:

   ```bat
   C:\suyu-src\tools\a32recomp\build_release_package.bat 0.1.0 <your name> <your user name>
   ```

   The extra words are searched for in every byte of the package (and every `C:\Users\...`
   path); the script stops if any is found. It configures the build against the empty stub
   module in `tools\a32recomp\kit_stub` (the static launcher's final link fails on purpose
   there; only its objects are needed), builds suyu and the kit objects, runs
   `make_link_kit.py` and `make_package.py`, and zips the result to
   `publish\suyu-mhgu-<version>-windows-x64.zip`.
3. Test the zip on a clean folder: export a game with its `suyu.exe` into an empty folder. The
   export log must say `Built single-file launcher with the link kit`.

Then follow [RELEASE.md](RELEASE.md).

## Layout of the recompiler code

| Path | What |
|---|---|
| `src/core/recompiler/a32/` | AArch32 → C translator: code discovery (`a32disc.h`), emitter, runtime sources |
| `src/core/arm/recomp/` | runtime side: block dispatch (`arm_recomp32.cpp`), hooks, `game_settings` (the ini), mod host (`mod_host*.{h,cpp}`, ABI in `mod_host_api.h`) |
| `src/suyu/game_export.cpp` | the Export Game dialog: recompile, build, link (build tree or link kit) |
| `src/suyu_cmd/` | the exported game's frontend: window, F12 panel, multiplayer, keyboard |
| `dist/coverage/` | code offsets (per module build ID) that discovery can't reach statically |
| `tools/a32recomp/` | build, packaging, publish check and test tools |
