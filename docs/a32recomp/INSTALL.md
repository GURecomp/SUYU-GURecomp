# Installing and exporting MHGU as a native Windows game

This guide covers the prebuilt release (`suyu-mhgu-<version>-windows-x64.zip` from the
Releases page). It turns **your own** copy of Monster Hunter Generations Ultimate (1.4.0) into
a native Windows `.exe`: suyu recompiles the game's ARM code to C on your PC, compiles it with
Microsoft's C++ compiler and links it into one launcher.

On Linux, follow [INSTALL-LINUX.md](INSTALL-LINUX.md) instead.

Nothing from the game is in this release. No keys, no game files, no recompiled code. You need
your own console dump, and you must never share it or anything the export produces.

Contents:

1. [What you need](#1-what-you-need)
2. [Install Visual Studio (the C++ compiler)](#2-install-visual-studio-the-c-compiler)
3. [Unpack suyu](#3-unpack-suyu)
4. [Keys and game files](#4-keys-and-game-files)
5. [Export the game](#5-export-the-game)
6. [Play and settings](#6-play-and-settings)
7. [Mods (Forge)](#7-mods-forge)
8. [Multiplayer](#8-multiplayer)
9. [Troubleshooting](#9-troubleshooting)
10. [Reporting a problem](#10-reporting-a-problem)

---

## 1. What you need

| | |
|---|---|
| OS | Windows 10 or 11, 64-bit |
| CPU | 64-bit x86 (Intel or AMD). The export uses all cores. |
| RAM | 16 GB recommended (the export compiles several large C files at once) |
| GPU | Vulkan capable, with a **current** driver from NVIDIA, AMD or Intel |
| Disk | About **25 GB free** on the drive you export to (the finished folder is about 22 GB, mostly the unpacked game files; about 12 GB if you untick "Decompress game archives"), plus about 10 GB for Visual Studio |
| Compiler | Visual Studio 2022 (**17.14 or newer**) or Visual Studio 2026, with "Desktop development with C++" (free; section 2) |
| Game | Your own dump of MHGU (base game) **and the 1.4.0 update**, plus your own `prod.keys` / `title.keys` |

Only the compiler needs a separate install. Everything else suyu needs at runtime ships in
the zip.

## 2. Install Visual Studio (the C++ compiler)

The export calls Microsoft's C++ compiler (`cl.exe`), linker (`link.exe`) and the CMake that
comes with Visual Studio. suyu finds them automatically through the Visual Studio Installer,
wherever and whichever edition you install.

Both **Visual Studio 2022** (version 17.14 or newer) and **Visual Studio 2026** work. If you
already have one of them, skip to step 3 and check the workload. Otherwise install **either** of
these (both are free), from https://visualstudio.microsoft.com/downloads/:

- **Visual Studio Community**: the full IDE.
- **Build Tools for Visual Studio**: just the compiler, no IDE, smaller (under "Tools for
  Visual Studio").

Steps:

1. Download and run the installer (`VisualStudioSetup.exe` or `vs_BuildTools.exe`).
2. On the **Workloads** tab, tick **Desktop development with C++**.
3. On the right, under *Installation details*, keep these ticked (they are by default):
   - **MSVC ... C++ x64/x86 build tools (Latest)** (the name depends on the Visual Studio version)
   - **Windows 11 SDK** (any version; Windows 10 SDK also works)
   - **C++ CMake tools for Windows**
4. Click **Install** and wait (about 8-10 GB).
5. **Version check:** the release was built with MSVC 14.44, which ships with Visual Studio
   2022 **17.14**. An older Visual Studio can't link it. If you have Visual Studio 2022, open the
   **Visual Studio Installer** and press **Update** on it; its card must say 17.14 or higher.
   Visual Studio 2026 (version 18) is newer and works as it is.

You don't have to open Visual Studio after installing. A reboot is not usually needed.

Other compilers (MinGW, clang, Visual Studio 2019 or older) aren't supported for the export.

## 3. Unpack suyu

1. Extract the zip to a folder you can write to, for example `D:\suyu-mhgu`. Avoid
   `C:\Program Files` (Windows blocks writes there).
2. Keep everything together. `suyu.exe` needs the DLLs, `plugins`, `pipelines`, `coverage` and
   `link_kit` folders beside it. `link_kit` holds suyu's own compiled code, which the export
   links into the game launcher.
3. Run `suyu.exe`. Windows SmartScreen may warn because the build isn't signed; choose
   *More info → Run anyway*.

## 4. Keys and game files

suyu needs the keys from **your own** Switch, and the game dumped from **your own** cartridge or
eShop purchase. This project won't link to or help with obtaining them elsewhere.

1. In suyu: **File → Open suyu Folder**, open `keys`, and copy your `prod.keys` and
   `title.keys` there. Restart suyu.
2. In the game list, double-click the entry "Double-click to add a new folder to the game
   list" and pick the folder with your MHGU dump (`.xci` or `.nsp`).
3. Install the 1.4.0 update: **File → Install Files to NAND...** and pick the update `.nsp`.
   Right-click the game → *Properties* should then list the update as 1.4.0.

The export only works with version **1.4.0** (title ID `0100770008DD8000`).

## 5. Export the game

1. **File → Export Game...**
2. Pick the game, an **empty** output folder on a drive with about 25 GB free, and keep the
   default options. **Use my suyu controller setup for Player 1** (on by default) copies the
   controller you set in suyu's Controls into the game; you can also change it later in the
   game menu.
3. Press Export. The first export takes **about 10-20 minutes** and uses all CPU cores. The
   window shows the steps: unpacking, recompiling each module, compiling the C code, then
   linking. The PC is busy during that time; that's expected.
4. When it finishes, the log ends with `Built single-file launcher with the link kit`. You can
   close suyu.
5. In the output folder, run **`MONSTER HUNTER GENERATIONS ULTIMATE.exe`**.

suyu remembers the game, folder and options for next time. You only export again when you get
a newer suyu release. Settings and mods never need a new export.

## 6. Play and settings

- **Game menu: F10**, or hold **Minus + Plus** (Back + Start / View + Menu / Create + Options)
  on the controller for one second. It opens in its own window; the game doesn't receive
  input while it has focus. Tabs:
  - **Controls**: pick the controller for Player 1 or the keyboard, rebind any button (press
    it), swap A/B and X/Y, or go back to automatic.
  - **Multiplayer**: host or join a room (section 8).
  - **Settings**: every `game_settings.ini` option. Frame rate, the FPS display and the menu
    buttons apply at once; the rest when the game restarts (the tab says which).
  - **General**: frame rate, folders (saves, logs, mods), mods found.
- **Controller**: with the default keyboard setup, the first controller that's connected is
  used for Player 1 automatically. A setup you choose yourself (game menu, or copied from suyu
  at export) is never replaced.
- `game_settings.ini` (created beside the game exe on first start) holds every option, each
  with a comment. Change them in the game menu's Settings tab, or edit the file and restart
  the game. Defaults are the original game.
  - `[Display] fps`: 30 (original), 60, 90, 120, or `auto`
  - `[Display] resolution`: `default`, `auto` (your desktop), or e.g. `3440x1440`
  - `[Display] console_mode`: `docked` (default, 1920x1080 render) or `handheld` (1280x720
    render, as on the Switch's own screen: the choice for weak PCs and integrated graphics)
  - `[Display] aspect`, `fullscreen`, `show_fps` (frame rate in the window title);
    `[Graphics] native_render`, `draw_distance`
  - `[Menu] key`, `pad_combo`: what opens the game menu
  - `[Multiplayer] ...` (section 8), `[Mods] loader` (section 7)
  - `[Debug] diagnostics`: off by default; `true` writes developer reports (a little slower)
  - `[Debug] rtss_overlay`: RivaTuner's Vulkan overlay crashes the game at start, so it is kept
    out by default (RivaTuner can stay running). Use `show_fps` or the game menu for FPS.
- **F11** or **Alt+Enter**: fullscreen. **F12**: the older developer panel (status, controller,
  mod folders, multiplayer); the game menu does the same things more easily.
- When the game asks for text (character name, chat), type it on your keyboard. Enter
  confirms, Esc cancels.
- On screens that aren't 16:9 the 3D view fills the screen. The HUD stays stretched unless you
  install the HudFix mod (section 7).

## 7. Mods

Mods go in the export's `mods\0100770008DD8000\` folder, one folder per mod:

- **File replacement mods** (texture packs, model and armor swaps, UI and text changes):
  anything that comes as a `romfs` folder. Put it at
  `mods\0100770008DD8000\<mod name>\romfs\...`, exactly as in suyu's or yuzu's mod folder
  (copying a mod folder over from suyu's `load\0100770008DD8000\` works). Restart the game;
  nothing is exported again. To turn a mod off, rename its `romfs` folder or move the mod out.
  If two mods replace the same file, only one wins.
- **Not supported**: `exefs` patches, `.pchtxt`/`.ips` code patches and cheats (60 FPS,
  ultrawide, FOV, ...). They edit the Switch code, which the export has recompiled; use
  `game_settings.ini` (frame rate, resolution, aspect, draw distance) or Forge plugins instead.
- **Code mods** need **Forge PC**, a separate download (MIT license). Extract it so that
  `mods\0100770008DD8000\Forge\forge.dll` exists. Its plugins (for example HudFix) go where
  Forge's README says. Restart the game; nothing is exported again.

`game_settings.ini [Mods] loader = none` turns code mods off.

## 8. Multiplayer

This is the game's own local play, carried over a suyu room. It works over the internet
through a VPN such as Radmin VPN or ZeroTier.

1. Everyone sets a nickname in the game menu's **Multiplayer** tab (**4-20 characters**:
   letters, digits, space, `. _ -`; each player a different one).
2. The host presses **Host**, then **Copy invite** next to their VPN address (for example
   Radmin VPN) and sends that invite to the others. Addresses stay hidden on screen until
   clicked, so they don't show on a stream.
3. The others paste the invite into **Join a friend** and press **Join**. Hosts joined before
   are listed below it for one-click joining.
4. If the connection drops, the game tries to rejoin for a minute (`[Multiplayer]
   auto_reconnect`). The window title shows the room status.
5. **Start-up** in the tab (or `[Multiplayer] mode = host` / `join`) connects automatically at
   the next start.
6. In game, use the Gathering Hall's local play.

## 9. Troubleshooting

| Problem | Fix |
|---|---|
| Export: "no Visual Studio C++ build tools found", "cl.exe / link.exe not found" or "cmake was not found" | Section 2: install the C++ workload with "C++ CMake tools for Windows"; check that the Visual Studio Installer shows it. A Visual Studio in an unusual place: set the environment variable `SUYU_VCVARS` to its `VC\Auxiliary\Build\vcvars64.bat`. |
| Export: link errors such as `LNK1104`, `LNK1143`, "unsupported version" or "was created with a newer compiler" | Update Visual Studio 2022 to 17.14 or newer (Installer → Update), or use Visual Studio 2026. |
| Export: "static recompiled executable was not produced" | Read the lines above it in the export log; usually a missing workload or no disk space. |
| Export stops with an out-of-space error | Free about 25 GB on the output drive. |
| suyu says keys are missing | Section 4: `prod.keys` and `title.keys` in suyu's `keys` folder, then restart. |
| The game exe closes right away | Run it from the export folder; keep `dxcompiler.dll`, `dxil.dll`, `libssl.dll`, `libcrypto.dll` and `user\` beside it. Update the GPU driver. |
| Black screen or Vulkan error at start | Update the GPU driver. On laptops, run the exe on the dedicated GPU. |
| The game closes right away with RivaTuner (RTSS) running | Fixed: the game keeps RivaTuner's Vulkan overlay out. Leave `[Debug] rtss_overlay = false`. |
| Controller does nothing | Game menu (F10) → Controls: pick the controller. Or plug it in before starting with the default keyboard setup. |
| A texture or file mod has no effect | Check the path: `mods\0100770008DD8000\<mod name>\romfs\...` (the `romfs` folder directly inside the mod folder). Code patches and cheats don't work (section 7). |
| Multiplayer: "nickname already in use" | The nickname is shorter than 4 characters or taken; pick another. |
| Slow first minutes | Shaders compile the first time each effect appears; later runs reuse them. |

## 10. Reporting a problem

Open an issue on the repository with:

- what you did and where it happened (town, quest, menu);
- `user\log\suyu_log.txt` and `user\recomp_report.txt` from the export folder;
- for export problems, the export log text from suyu's window.

These logs show your own folder paths; edit them out if you like. **Never attach** the game
exe, the `exefs` folder, keys, or game files.
