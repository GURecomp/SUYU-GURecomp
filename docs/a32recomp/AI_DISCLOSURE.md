# AI disclosure and usage

This project was built with the help of an AI coding assistant (a large language model), working under the direction of the maintainer. This page says what that means, so nobody has to guess.

## What the AI did
- Wrote and edited a large share of the source code in this repository: the ARM32 lifter (a32recomp), the exporter, the runtime, the NVN to Vulkan replacement renderer, the in-game menu, the LAN/online room code and the build and test tooling.
- Wrote and maintained the documentation and notes in `docs/a32recomp/`.
- Did the reverse-engineering legwork: disassembling the game's own code, tracing calls, and reading logs and profiles from test runs.

## What the maintainer did
- Set the goals, decided what to build and what to drop, and chose every design direction.
- Ran every build, export and play test on real hardware, and judged the results (visuals, speed, stability).
- Caught bugs by hand and dug through files, logs and screenshots to find what was wrong, not just exporting and testing.
- Reviewed what is published here and decided what goes public.

## How AI output is checked
- The lifter is checked instruction by instruction and function by function against a reference CPU emulator (bit-exact, including NaN payloads), then by running the real game.
- The renderer is checked draw by draw against suyu's own GPU emulation, and by playing.
- Nothing is trusted because an AI wrote it. If it is not tested, it is not claimed to work.

## What the AI did NOT do
- It did not use any leaked or NDA material. NVN support is clean-room: names come from strings in the game itself, behaviour from observing calls.
- It was not given, and this repository does not contain, any game files, keys, or generated game code. You export from your own legally dumped copy.
- Third-party ideas are credited in the release notes (for example the community frame-rate, aspect-ratio and resolution research). Their patches are reference only and are not shipped.

## Licensing and responsibility
The code here is released under the licence in this repository (suyu's GPL-3.0-or-later for derived parts). Everything published here is published as-is, whoever or whatever typed it, and the project is maintained under the GURecomp handle. Report problems as ordinary issues.

## Policy for contributions
Pull requests are currently limited: only large contributions are being accepted for now, and small ones may be closed without review. Contributions that used AI tools are fine if you disclose it in the pull request, understand and have tested what you submit, and can answer questions about it. Do not submit code you cannot explain, and do not submit anything that includes leaked SDK material, keys or game data.
