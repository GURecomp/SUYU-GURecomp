#!/usr/bin/env python3
"""Map the game's NVN program objects to shader package programs.

usage: nvn_program_map.py <nvn_programs.txt> <package.spk> <spk_index.txt> <out map.tsv>
                          [nvn_args.txt pairs.txt]

nvn_programs.txt (written by the exe's NVN trace) has one line per nvnProgramSetShaders call:
program object, stage count, then per stage the code's GPU address, control pointer and
control bytes. The game copies each program to its own aligned GPU allocation, so addresses
don't map to package offsets directly. Observed instead (MHGU 1.4.0, checked against the shader
cache: every bound program's pair matched the code suyu compiled):
  - programs are created in package order: the k-th SetShaders call uses the k-th vertex
    program of the package;
  - pixel programs follow package order by first use (a few are shared between programs).

Writes "<program object>\t<vs n>\t<fs n>" with n the unique program numbers shader-scan and
pipeline-test use. With nvn_args.txt it also writes the (vs n, fs n) pairs of every program the
run bound (nvnCommandBufferBindProgram), for pipeline-test.
"""
import sys


def unique_numbers(spk_path, index_path):
    spk = open(spk_path, "rb").read()
    seen, vs, fs = {}, [], []
    for line in open(index_path):
        f = line.split()
        if len(f) < 3:
            continue
        off, kind, size = int(f[0], 16), f[1], int(f[2])
        n = seen.setdefault(spk[off:off + size], len(seen))
        (fs if kind == "5" else vs).append(n)
    return vs, fs


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    vs_n, fs_n = unique_numbers(sys.argv[2], sys.argv[3])
    calls = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if not l.startswith("#")]
    fs_order, programs = {}, {}
    for k, c in enumerate(calls):
        fs_gpu = int(c[3].split(" ")[0], 16)
        j = fs_order.setdefault(fs_gpu, len(fs_order))
        programs[int(c[0], 16)] = (vs_n[k], fs_n[j])
    with open(sys.argv[4], "w") as out:
        for obj, (v, f) in programs.items():
            out.write(f"{obj:08x}\t{v}\t{f}\n")
    print(f"{len(calls)} programs ({len(set(vs_n))} vs / {len(fs_order)} fs package entries used)")
    if len(sys.argv) > 6:
        bound, inside = set(), False
        for l in open(sys.argv[5]):
            if l.startswith("== nvnCommandBufferBindProgram"):
                inside = True
                continue
            if l.startswith(("== ", "-- ")):
                inside = False
            if inside and l[:1].isdigit():
                bound.add(int(l.split()[1], 16))
        pairs = sorted({programs[p] for p in bound if p in programs})
        with open(sys.argv[6], "w") as out:
            out.writelines(f"{v} {f}\n" for v, f in pairs)
        print(f"{len(bound)} bound program objects -> {len(pairs)} vs+fs pairs")


if __name__ == "__main__":
    main()
