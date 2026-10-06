# Installing and exporting MHGU as a native Linux game

This guide covers the prebuilt Linux release (`suyu-mhgu-<version>-linux-x64.tar.xz` from the
Releases page). It turns **your own** copy of Monster Hunter Generations Ultimate (1.4.0) into
a native Linux program: suyu recompiles the game's ARM code to C on your PC, compiles it with
your system's C compiler (gcc or clang) and links it into one launcher. The Windows guide is
[INSTALL.md](INSTALL.md); most of it (keys, settings, multiplayer) applies here too.

Nothing from the game is in this release. No keys, no game files, no recompiled code. You need
your own console dump, and you must never share it or anything the export produces.

## 1. What you need

| | |
|---|---|
| OS | 64-bit x86 Linux with glibc **2.35 or newer**: Ubuntu 22.04+, Debian 12+, Fedora 36+, Linux Mint 21+, SteamOS 3.5+, Arch and derivatives |
| CPU | 64-bit x86 (Intel or AMD). The export uses all cores. |
| RAM | 16 GB recommended (the export compiles several large C files at once) |
| GPU | Vulkan capable, with a **current** driver: Mesa (RADV for AMD, ANV for Intel) or NVIDIA's own driver |
| Disk | About **25 GB free** where you export to (the finished folder is about 22 GB, mostly the unpacked game files; about 12 GB if you untick "Decompress game archives") |
| Build tools | A C/C++ compiler (gcc or clang), CMake and Ninja (section 2) |
| Game | Your own dump of MHGU (base game) **and the 1.4.0 update**, plus your own `prod.keys` / `title.keys` |

## 2. Install the build tools

One command, for your distribution:

```bash
# Debian, Ubuntu, Linux Mint, Pop!_OS
sudo apt install build-essential cmake ninja-build
# Fedora
sudo dnf install gcc gcc-c++ cmake ninja-build
# Arch, Manjaro, EndeavourOS, CachyOS
sudo pacman -S --needed base-devel cmake ninja
```

Any gcc 9+ or clang 10+ works: suyu's own part is prebuilt (with its C++ runtime linked in), and
your compiler only builds the game's generated C.

**SteamOS (Steam Deck)**: the system partition is read-only. Either switch to desktop mode and
run `sudo steamos-readonly disable`, then `sudo pacman-key --init && sudo pacman-key
--populate archlinux holo && sudo pacman -S --needed base-devel cmake ninja` (SteamOS updates
may undo it, which is fine: the tools are only needed while exporting), or export on another
Linux PC and copy the finished folder to the Deck.

## 3. Unpack suyu

```bash
tar -xJf suyu-mhgu-<version>-linux-x64.tar.xz
cd suyu-mhgu-<version>-linux-x64
./suyu
```

Keep the folder together: `suyu` finds its libraries (`lib/`), Qt plugins (`plugins/`) and the
link kit (`link_kit/`) beside itself.

## 4. Keys and game files

1. Put `prod.keys` and `title.keys` in `~/.local/share/suyu/keys/` (**File → Open suyu Folder**,
   then `keys`), and restart suyu.
2. Add the folder with your game dump (double-click the game list), and install the 1.4.0
   update (**File → Install Files to NAND**). The game list should show version 1.4.0.

## 5. Export the game

1. **File → Export Game...**
2. Pick the game, an **empty** output folder with about 25 GB free, keep the default options
   (Target **Linux**, Format **Build**).
3. Press Export. The first export takes **about 10-20 minutes** and uses all CPU cores.
4. When it finishes, the log ends with `Built single-file launcher with the link kit`.
5. In the output folder, run the game:

   ```bash
   cd "<output folder>/MONSTER HUNTER GENERATIONS ULTIMATE"
   ./"MONSTER HUNTER GENERATIONS ULTIMATE"
   ```

   For the application menu: `cp "MONSTER HUNTER GENERATIONS ULTIMATE.desktop"
   ~/.local/share/applications/` (it points at this folder; export again or edit it if you
   move the folder). For Steam: **Add a Game → Add a Non-Steam Game**, browse to the game file.

## 6. Play and settings

Same as on Windows ([INSTALL.md, section 6](INSTALL.md#6-play-and-settings)): the **game menu**
(**F10**, or hold Minus + Plus on the controller) sets the controller, multiplayer and every
`game_settings.ini` option. **F11** or **Alt+Enter** toggles fullscreen.

Differences on Linux:

- There is no **F12** developer panel; the game menu covers it.
- Code mods (Forge PC, HudFix) are Windows-only for now. File replacement mods such as
  texture packs (`mods/0100770008DD8000/<mod>/romfs/...`) work.

## 7. Multiplayer

Same steps as [INSTALL.md, section 8](INSTALL.md#8-multiplayer), in the game menu's
Multiplayer tab, or in `game_settings.ini`:

```ini
[Multiplayer]
mode = host          ; the other players: mode = join
nickname = Hunter1   ; 4-20 characters, different for each player
address = 26.1.2.3   ; join: the host's VPN address
port = 24872
password =
```

Linux and Windows players use the same room protocol (mixed groups not tested yet). Radmin VPN
has no Linux version; use a VPN that
both sides have, such as ZeroTier or Tailscale.

## 8. Troubleshooting

| Problem | Fix |
|---|---|
| Export: "The build tools were not found" | Section 2. Check with `cc --version`, `c++ --version`, `cmake --version`. |
| Export: "static recompiled executable was not produced" | Read the lines above it in the export log (`~/.local/share/suyu/log/suyu_log.txt`). |
| `./suyu`: "cannot open shared object file" | Keep the unpacked folder together; a missing system library (for example `libxcb-cursor0` on some Ubuntu installs) is installed with the package manager. |
| `./suyu`: "version GLIBC_2.xx not found" | The distribution is older than the requirements in section 1. |
| The game closes right away | Run it from its folder; keep `lib/`, `exefs/` and `user/` beside it. Check `user/log/suyu_log.txt`. |
| Black screen or Vulkan error | Update Mesa / the NVIDIA driver; `vulkaninfo --summary` should list your GPU. On hybrid laptops start the game with `DRI_PRIME=1` (Mesa) or `prime-run` (NVIDIA). |
| No sound (log: "SDL_InitSubSystem audio failed", "cubeb_init failed") | The sound client libraries are missing (minimal installs, WSL): `sudo apt install libpulse0 libasound2` (Fedora: `pulseaudio-libs alsa-lib`). |
| Wayland: no window or wrong scaling | Start with `SDL_VIDEODRIVER=x11` to use XWayland. |

## 9. Reporting a problem

As in [INSTALL.md, section 10](INSTALL.md#10-reporting-a-problem). Mention that it's Linux and
your distribution. Logs: `~/.local/share/suyu/log/suyu_log.txt` for the export,
`<game folder>/user/log/suyu_log.txt` and `user/recomp_report.txt` for the game (they contain
your home folder path).
