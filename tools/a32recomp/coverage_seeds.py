"""coverage_seeds.py <recomp_modules.txt> <out dir> <recomp_misses.txt> [more misses ...]

Turns the block misses that play sessions recorded (<export>/user/recomp_misses.txt, lines
"<load index> <offset> <name>") into shippable coverage seeds: <out dir>/<build id>.txt per
module, one hex code offset per line, merged with what the file already holds. The game
export (LoadA32RuntimeMisses) adds the seeds of every module whose build ID matches, so a
fresh export folder is as complete as one with play history. Offsets only: no game code.

<recomp_modules.txt> is the export's stamp ("<name> <build id> <text size>" in load-index
order) that the misses were recorded against.
"""
import os
import sys

stamp, out_dir, misses = sys.argv[1], sys.argv[2], sys.argv[3:]
modules = [line.split() for line in open(stamp, encoding="utf-8") if line.strip()]
os.makedirs(out_dir, exist_ok=True)

seeds = {}
for path in misses:
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.split()
        if len(parts) < 2:
            continue
        try:
            index, off = int(parts[0]), int(parts[1], 16)
        except ValueError:
            continue
        if 0 <= index < len(modules):
            seeds.setdefault(modules[index][1].lower(), set()).add(off)

for build_id, offs in seeds.items():
    path = os.path.join(out_dir, build_id + ".txt")
    if os.path.exists(path):
        offs |= {int(x, 16) for x in open(path, encoding="utf-8").read().split()
                 if not x.startswith("#")}
    name = next(m[0] for m in modules if m[1].lower() == build_id)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# %s: code offsets found missing by play sessions\n" % name)
        f.write("".join("%08x\n" % o for o in sorted(offs)))
    print("%s (%s): %d offsets" % (name, build_id[:16], len(offs)))
