# a32recomp: 32-bit ARM static recompilation for suyu

suyu v0.04's AOT "Recompiler mode" only understood AArch64, so 32-bit titles such as
Monster Hunter Generations Ultimate (MHGU) came out as broken exports (88% of MHGU's `main`
unhandled, the rest mistranslated). This adds the AArch32 path: ARM + VFP code is lifted
to C, one function per basic block, built into the exported exe and run by a new CPU
backend, with suyu's dynarmic A32 JIT as fallback.

## What changed in suyu

| File | Change |
|---|---|
| `src/core/recompiler/a32/a32_to_c.h` | The lifter: one ARM instruction to C. |
| `src/core/recompiler/a32/a32disc.h` | Code discovery for 32-bit NSOs (entry, exports, relocations, PC-relative pointers, prologues); literal pools marked as data. |
| `src/core/recompiler/a32/a32emit.h` | Writes a module's C project (same shape as the AArch64 exporter's: `recomp_static_<mod>` + `recomp_image_lookup/set_base/entry`). |
| `src/core/recompiler/a32/a32_runtime_sources.h` | `a32_runtime.h` / `a32_host_runtime.c` embedded as strings (regenerate with `tools/a32recomp/embed_runtime.py`). |
| `src/core/arm/recomp/arm_recomp32.{h,cpp}` | `ArmRecomp32`: CPU backend for recompiled 32-bit code. SVCs to the HLE kernel, memory through suyu's page table/Memory, exclusives through the kernel's monitor, JIT fallback for missing blocks, untranslated instructions and Thumb. |
| `src/core/arm/recomp/arm_recomp.{h,cpp}` | `GetRecompBaseSetter()` so both backends share module-base registration. |
| `src/core/hle/kernel/k_process.cpp` | 32-bit application processes get `ArmRecomp32` when a recompiled image is registered. |
| `src/suyu/game_export.cpp` | Detects 32-bit titles from `main.npdm` and lifts their modules with a32recomp. |
| `src/core/CMakeLists.txt` | Adds `arm_recomp32`. |

Nothing changes for 64-bit titles or for normal (JIT) emulation.

## Using it

1. Apply the patch to the suyu v0.04 source (`git apply suyu-a32recomp.patch`) and build suyu
   as usual (MSVC on Windows).
2. In suyu: right-click MHGU, **Export Game (AOT Static Recompilation)**, backend Dynarmic,
   and enable **Fall back to interpreter**.
3. The log should say `AOT: 32-bit title - lifting modules with a32recomp` and report per
   module `AOT a32 [main]: 732750 blocks, 4058891 instructions (6 untranslated -> JIT)`.
   `main` is ~500 MB of C (64 files, ~18 s each at -O1 with gcc); the build takes a while.
4. Run the exported exe. `ArmRecomp32` logs module bases at start
   (`recomp32: module [n] ... at 0x...`), any block misses (`no block at ...; running on JIT`),
   and a summary on shutdown (`recomp32 execution: N static blocks, N SVCs, JIT fallbacks: ...`).

For a first boot report, send `suyu_log.txt` from the export's `user/log` folder.

## Standalone tools (this folder)

Build with `-I../../src/core/recompiler/a32` (the tools include the lifter headers from there),
and keep `a32_runtime.h` / `a32_host_runtime.c` beside them.

| Tool | Purpose |
|---|---|
| `a32cov module.nso [--emit dir --name mod --rt .] [--sweep N] [--misses recomp_misses.txt <module>]` | Coverage report; `--emit` writes the module project exactly as the exporter does. `--sweep` sets discovery's boundary sweep level (the exporter uses 2); `--misses` scores discovery against addresses a run of the exported game recorded as missed. NSO must have uncompressed segments (`hactool --uncompressed`). |
| `difftest.py random/vfp/v8 N seed` (+ `a32gen.cpp`, `test_host.c`) | Per-instruction differential test vs Unicorn (ARMv8 CPU model), bit-exact including NaN payloads. |
| `a32subset` + `functest.py` (+ `funchost.c`) | Whole-function differential test on real module code: runs functions to completion in Unicorn and in the recompiled C through the real host runtime and page-table fast path, compares registers, flags, VFP and every byte written. |
| `mtf_shaders.py` + `shader-scan` (`src/shader_scan`) | Pulls the compiled shader package out of an MT Framework NX RomFS (`.arc` -> `SPK`), indexes every Maxwell program, and runs each through the shader recompiler to SPIR-V offline. Validate the output with `spirv-val --target-env vulkan1.3 --scalar-block-layout`. |
| `pipeline-test` (`src/shader_scan`) + `vkcache_pairs.py` | Builds a real Vulkan graphics pipeline headless for every program of the package (or for the exact vertex+pixel pairs a game run built, read from suyu's shader cache by `vkcache_pairs.py`), through the Vulkan backend's own device/profile/layout code; reports failures and driver compile times. `--validate` enables the validation layer. |
| `nvn_program_map.py` | Maps the game's NVN program objects (from the exe's `nvn_programs.txt`) to shader package programs, and lists the vertex+pixel pairs a run bound, for `pipeline-test`. |

## Results on MHGU (update 0100770008DD8800)

| Module | Code reached | Translated |
|---|---|---|
| main | 77.6% of .text (4.06M instructions) | 100.000% (6 words left; they decode as data, not code) |
| subsdk0 | 87.4% | 100.000% |
| sdk | 85.9% | 99.98% (AES/SHA + a few NEON forms go to the JIT) |

- Per-instruction tests: 100% bit-exact across integer, VFP and ARMv8/NEON suites.
- Whole-function tests on 300 real `main` functions: 984 completed runs, all identical.
  (Found and fixed a real NaN-encoding difference between x86 and ARM this way.)
- Generated `rtld` project builds and links through its CMake; `main`'s C compiles as strict
  C11 (`-std=c11 -pedantic-errors -Wall`) with no diagnostics.
- suyu changes compile-checked (GCC, suyu's headers, Qt6) - not yet built with MSVC.

## Known limits

- **Not yet run inside suyu.** Thread switching, SVCs, module bases and rtld self-relocation
  are compile-checked only; expect a few rounds of fixes from the first boot's logs.
- **Single-core mode:** recompiled code does not advance suyu's CPU tick count (same as the
  AArch64 backend); use multicore.
- **FPSCR modes:** flush-to-zero, default-NaN and non-default rounding modes are not modelled.
- **JIT fallback** covers Thumb code, crypto instructions, and code discovery doesn't reach.
- **Speed:** every block returns to the dispatcher (binary search lookup); no direct
  block-to-block chaining yet.
- Exclusive-access alignment faults are not modelled (real game code doesn't rely on them).
