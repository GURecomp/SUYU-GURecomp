suyu (recompiler test build, Linux) - Monster Hunter Generations Ultimate
=========================================================================

This suyu turns your own copy of MHGU (1.4.0) into a native Linux game: it recompiles
the game's code on your PC and packages it as one program with the game's files.
Test build: expect rough edges, and please report what you see.

NEEDED
------
- 64-bit Linux (x86-64) with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36,
  Linux Mint 21, SteamOS 3.5, Arch and anything newer), and a GPU with a current Vulkan
  driver (Mesa RADV/ANV, or NVIDIA's own driver).
- A C/C++ compiler, CMake and Ninja. The export compiles the game's code with them:
    Debian/Ubuntu/Mint:  sudo apt install build-essential cmake ninja-build
    Fedora:              sudo dnf install gcc gcc-c++ cmake ninja-build
    Arch/SteamOS:        sudo pacman -S base-devel cmake ninja
  (SteamOS: the system is read-only by default; see the install guide.)
- About 25 GB free disk where you export to.
- Your own Switch keys (prod.keys, title.keys) and your own dump of the game with the
  1.4.0 update. None are included and none may be shared.

EXPORTING THE GAME
------------------
1. Unpack this folder anywhere and run ./suyu in it. Put your keys in suyu's keys folder
   (File > Open suyu Folder > keys, normally ~/.local/share/suyu/keys), then restart suyu.
2. Add your game folder, install the 1.4.0 update if needed.
3. File > Export Game... : pick the game (ROM), an output folder and keep the defaults
   (Target: Linux). The first export takes a while (about 10-20 minutes, all CPU cores);
   the window shows progress. suyu remembers the ROM, folder and options for next time.
4. Run "./MONSTER HUNTER GENERATIONS ULTIMATE" in the output folder (quotes needed for the
   spaces), or copy "MONSTER HUNTER GENERATIONS ULTIMATE.desktop" from there to
   ~/.local/share/applications to get it in your application menu.

PLAYING
-------
- game_settings.ini (beside the game, created on first start) holds every option, read
  at each start (no re-export needed): fps (30 = original, 60, 90, 120 or auto),
  resolution (auto = your desktop), aspect, fullscreen, draw distance, multiplayer...
  Each option has a comment explaining it.
- Game menu: F10, or hold Minus + Plus (Back + Start) on the controller for a second.
  Controls, Multiplayer, Settings (every game_settings.ini option) and General.
- F11 or Alt+Enter: fullscreen.
- Texture packs and other file mods: mods/0100770008DD8000/<mod name>/romfs/..., as in
  suyu. Code mods (Forge PC, HudFix) are Windows-only for now.

Full guide with troubleshooting: docs/a32recomp/INSTALL.md in the source repository.

MULTIPLAYER (local play over a room, e.g. Radmin VPN or any VPN both players share)
--------------------------------------------------------------------------------
In the game menu's Multiplayer tab: one player presses Host and copies an invite, the
others paste it and press Join (or in game_settings.ini, [Multiplayer]: mode = host / join,
address = the host's VPN IP). Nicknames must be 4-20 characters (letters, digits, space,
. _ -) and different for each player. Then use the game's own local play (Gathering Hall).

REPORTING
---------
Send, from the exported game's folder:
  user/log/suyu_log.txt and user/recomp_report.txt
plus what you did and where it happened. Note: these files show your own folder paths.
Never send the exported game, your keys or the game files.
