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
- Game menu: F10, or hold Minus + Plus (Back + Start) on the controller for a second.
  Controls (controller, rebinding), Multiplayer, Settings (every game_settings.ini option)
  and General (frame rate, folders). The game ignores input while the menu has focus.
- The first controller connected is used automatically, unless you chose a setup yourself.
- game_settings.ini (beside the exe, created on first start) holds every option, read
  at each start (no re-export needed): fps (30 = original, 60, 90, 120 or auto),
  resolution (auto = your desktop), aspect, fullscreen, draw distance, multiplayer...
  Each option has a comment explaining it. The menu's Settings tab edits the same file.
- F11 or Alt+Enter: fullscreen. F12: the older developer panel.
- RivaTuner's Vulkan overlay is kept out of the game (it crashes it at start). For FPS, set
  [Display] show_fps = true or open the game menu.
- Texture packs and other file mods (a romfs folder): put them in
  mods\0100770008DD8000\<mod name>\romfs\..., as in suyu. Cheats and code patches
  (pchtxt/IPS) don't apply to the recompiled game; use game_settings.ini instead.
- On screens that aren't 16:9 the HUD is stretched unless you install the HudFix plugin
  for Forge PC (separate download; it goes in the export's mods\0100770008DD8000\ folder).

Full guide with troubleshooting: docs/a32recomp/INSTALL.md in the source repository.

MULTIPLAYER (local play over a room, e.g. Radmin VPN)
-----------------------------------------------------
In the game menu's Multiplayer tab: one player presses Host, then Copy invite next to
their Radmin VPN address and sends it; the others paste it under "Join a friend" and
press Join. Nicknames must be 4-20 characters (letters, digits, space, . _ -) and
different for each player. Addresses stay hidden until clicked. A dropped connection is
retried for a minute. Start-up (or [Multiplayer] mode = host / join in game_settings.ini)
connects at the next start. Then use the game's own local play (Gathering Hall).

REPORTING
---------
Send, from the exported game's folder:
  user\log\suyu_log.txt and user\recomp_report.txt
plus what you did and where it happened. Note: these files show your own folder paths.
Never send the exported .exe, your keys or the game files.
