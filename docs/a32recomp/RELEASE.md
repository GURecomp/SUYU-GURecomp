# Publishing and releases

How this fork goes public, and the text for each GitHub release. Read it top to bottom before
the first push.

## What is and isn't published

Published: this source tree (GPL-3.0-or-later, like suyu/Eden/yuzu), the prebuilt release zip
(suyu.exe, its DLLs, the link kit, licenses), and the docs.

Never published, in the repo, a release, an issue or a screenshot:

- console keys (`prod.keys`, `title.keys`) or anything derived from them;
- game files: dumps (`.xci`/`.nsp`/`.nca`), `exefs`/`romfs`, extracted assets, layout or
  table data copied out of the game;
- anything an export produces: the game exe, `aot_cache`/`exefs` folders, generated C code,
  `.lib`/`.obj` files built from the game;
- run logs and reports (`suyu_log.txt`, `recomp_report.txt`, diagnostics): they contain local
  paths;
- NVN material from Nintendo's SDK. The NVN work here is clean-room: names and behaviour
  come from public research and observation of the running game, never from SDK headers or
  docs.

`dist/coverage/*.txt` is fine: it lists code offsets per module build ID, no game bytes.

## One-time setup

1. **Account and e-mail.** Create these yourself: the e-mail provider (Proton Mail is a good
   choice for a separate identity: free, no phone number usually needed), then the GitHub
   account with that e-mail. Use a handle with no link to your other accounts. Note that
   GitHub's terms allow one free personal account per person; read them before opening a
   second one.
2. **Commit identity.** In GitHub → Settings → Emails, tick *Keep my email addresses private*.
   GitHub then shows an address like `<id>+<handle>@users.noreply.github.com`. Use only that
   in commits (step 3 below), never a real name or e-mail.
3. **Fresh history.** The private history contains early snapshot files (a binary with a
   local path, among others) and must not be pushed. Publish a new history that starts at
   the current tree:

   ```bat
   git clone --branch a32recomp <private repo path> C:\suyu-pub
   cd /d C:\suyu-pub
   git checkout --orphan main-public
   git -c user.name="<handle>" -c user.email="<id>+<handle>@users.noreply.github.com" ^
     commit -m "suyu with AArch32 static recompilation (MHGU native export)"
   python tools\a32recomp\publish_check.py <your name> <your e-mail> <your user name>
   ```

   Only push when the check says `clean`. Later commits on the public repo are made the same
   way (`-c user.name=... -c user.email=...`, or set them in that clone's own
   `git config`). Never push the private repo or its branches.
4. **Push.** Create an empty public repo on GitHub (no README, no license: both are in the
   tree), then:

   ```bat
   git remote add origin https://github.com/<handle>/<repo>.git
   git push -u origin main-public:main
   ```

## Licensing checklist

| Part | License | What's required | Where it's done |
|---|---|---|---|
| suyu/Eden/yuzu code and our changes | GPL-3.0-or-later (files tagged GPL-2.0-or-later and others are compatible) | Source for every binary you distribute; keep notices | Public repo; `LICENSE.txt`, SPDX tags |
| `mod_host_api.h` | MIT | Keep the notice | In the file |
| Forge PC (separate download) | MIT, based on Fexty's Forge (MIT) | Keep both notices | forge-pc `LICENSE` |
| Qt 6 DLLs | LGPL-3.0 | License text; DLLs replaceable (dynamic linking) | `licenses/qt/` in the zip |
| OpenSSL | Apache-2.0 | License text | `licenses/Apache-2.0.txt` |
| DXC (`dxcompiler.dll`, `dxil.dll`) | NCSA / Microsoft redistributable | Notice | `LICENSES.txt` in the zip |
| Other bundled libraries | their own GPL-compatible licenses | Texts | `licenses/` |
| Public patches used as references (masagrator 60 FPS, minderrx 90/120 FPS, Fl4sh9174 pchtxt pack, TLin-Y 16:10) | none of their content is shipped | Credit | `game_settings.h` comment, release notes |

Every release zip must match a commit in the public repo (the GPL's source offer). Tag that
commit with the version.

## Making a release

1. In the public clone, at the release commit: `python tools\a32recomp\publish_check.py ...`
   → `clean`.
2. Build the package from a neutral path ([BUILDING.md](BUILDING.md) section 5):
   `tools\a32recomp\build_release_package.bat <version> <your name> <your user name>`.
3. Test the zip: unpack to a new folder, export into an empty folder, play a quest. The export
   log must say `Built single-file launcher with the link kit`.
4. Hash it: `certutil -hashfile publish\suyu-mhgu-<version>-windows-x64.zip SHA256`.
5. Tag and push: `git tag v<version>` then `git push origin v<version>`.
6. GitHub → Releases → *Draft a new release* → choose the tag → title and text below → attach
   the zip → tick *Set as a pre-release* while it's a test build → Publish.

## Release text (copy into the Releases tab)

Fill in `<version>` and the SHA-256, and check the known issues are still current.

```markdown
## suyu MHGU native export <version> (test build)

Turns **your own** copy of Monster Hunter Generations Ultimate (Switch, version 1.4.0) into a
native Windows game. suyu recompiles the game's 32-bit ARM code to C on your PC, compiles it
with Microsoft's C++ compiler and links it into one `.exe`. This release contains no game
code, no keys and no game files.

### Download
- `suyu-mhgu-<version>-windows-x64.zip`: suyu with the exporter (Windows 10/11, 64-bit)
  SHA-256: `<hash>`

### You need
- Visual Studio 2022 **17.14 or newer** (free Community or Build Tools) with
  "Desktop development with C++". It's the only extra install.
- About 25 GB free disk for the export, a GPU with a current Vulkan driver.
- Your own keys and your own dump of MHGU with the 1.4.0 update.

Step-by-step guide: [docs/a32recomp/INSTALL.md](docs/a32recomp/INSTALL.md)

### What works
- Town, hunts, menus and local play tested; reports welcome for anything that breaks
- 30, 60, 90, 120 FPS or auto (`game_settings.ini`, no re-export)
- Any resolution and aspect ratio (ultrawide), native render up to 3840x2160
- Fullscreen (F11 / Alt+Enter), F12 panel (status, controller binding, mods, multiplayer)
- Local play over a suyu room (e.g. Radmin VPN), host/join from the F12 panel
- Typing on the PC keyboard where the game asks for text
- Mods: romfs replacements, and code mods through Forge PC (separate download, drop-in)

### Known issues
- The HUD is stretched on screens that aren't 16:9. Fix: the HudFix plugin for Forge PC.
- suyu.exe can stay open in the background after an export; end it in Task Manager before
  updating.
- A first start has once failed with a Vulkan out-of-memory error; starting again worked.

### Credits
suyu, Eden and yuzu developers (emulator base). Public MHGU patches used as references, not
shipped: 60 FPS by masagrator, 90/120 FPS by minderrx, pchtxt pack by Fl4sh9174, 16:10 by
TLin-Y. Forge by Fexty (Forge PC is a port, with permission).

### License
GPL-3.0-or-later; source for this build is the tagged commit. Third-party licenses are in the
zip (`LICENSE.txt`, `LICENSES.txt`, `licenses/`). Not affiliated with Nintendo or Capcom.
```

## Reporting template

Point testers to [INSTALL.md section 10](INSTALL.md#10-reporting-a-problem). Remind them that
logs show their folder paths.
