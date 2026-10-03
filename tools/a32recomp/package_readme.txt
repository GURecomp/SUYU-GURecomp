suyu (recompiler test build) - Monster Hunter Generations Ultimate
==================================================================

This suyu turns your own copy of MHGU (1.4.0) into a native Windows game: it recompiles
the game's code on your PC and packages it as one .exe with the game's files.
Test build: expect rough edges, and please report what you see.

NEEDED
------
- Windows 10/11, 64-bit, and a GPU with a current Vulkan driver.
- Visual Studio 2022 version 17.14 or newer, or Visual Studio 2026 (the free Community
  edition or "Build Tools for Visual Studio"), with the workload "Desktop development with C++"
  (keep its default parts: MSVC build tools, Windows SDK, C++ CMake tools). The export compiles
  the game's code with it. Already installed? Visual Studio Installer > Update.
  Nothing else to install.
- About 25 GB free disk on the drive you export to.
- Your own Switch keys (prod.keys, title.keys) and your own dump of the game with the
  1.4.0 update. None are included and none may be shared.

EXPORTING THE GAME
------------------
1. Run suyu.exe. Put your keys in suyu's keys folder (File > Open suyu Folder > keys),
   then restart suyu.
2. Add your game folder, install the 1.4.0 update if needed.
3. File > Export Game... : pick the game (ROM), an output folder and keep the defaults.
   The first export takes a while (about 10-20 minutes, all CPU cores); the window shows
   progress. suyu remembers the ROM, folder and options for next time.
4. Run "MONSTER HUNTER GENERATIONS ULTIMATE.exe" in the output folder.

PLAYING
-------
- game_settings.ini (beside the exe, created on first start) holds every option, read
  at each start (no re-export needed): fps (30 = original, 60, 90, 120 or auto),
  resolution (auto = your desktop), aspect, fullscreen, draw distance, multiplayer...
  Each option has a comment explaining it.
- F11 or Alt+Enter: fullscreen. F12: panel with status, controller binding, mod folders
  and multiplayer.
- On screens that aren't 16:9 the HUD is stretched unless you install the HudFix plugin
  for Forge PC (separate download; it goes in the export's mods\0100770008DD8000\ folder).

Full guide with troubleshooting: docs/a32recomp/INSTALL.md in the source repository.

MULTIPLAYER (local play over a room, e.g. Radmin VPN)
-----------------------------------------------------
One player presses Host in the F12 panel (or sets [Multiplayer] mode = host in
game_settings.ini); the others enter the host's address (their Radmin VPN IP) and
press Join. Same port and password on both sides. Nicknames must be 4-20 characters
(letters, digits, space, . _ -) and different for each player. "Save settings" makes it automatic
at the next start. Then use the game's own local play (Gathering Hall).

REPORTING
---------
Send, from the exported game's folder:
  user\log\suyu_log.txt and user\recomp_report.txt
plus what you did and where it happened. Note: these files show your own folder paths.
Never send the exported .exe, your keys or the game files.
