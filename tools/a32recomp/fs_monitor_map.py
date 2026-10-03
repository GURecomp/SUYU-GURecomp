#!/usr/bin/env python3
"""Names the files behind user/fs_monitor.txt (the exported game's file load monitor).

    fs_monitor_map.py <romfs.bin> <fs_monitor.txt> [--all]

Reads the RomFS file table from the exported romfs.bin, then prints every hitch with the burst
that overlapped it and the files that burst read, followed by the slowest bursts. --all lists
every burst.
"""
import bisect
import struct
import sys


def romfs_files(path):
    with open(path, "rb") as f:
        h = struct.unpack("<10Q", f.read(80))

        def rd(off, n):
            f.seek(off)
            return f.read(n)

        dmeta, fmeta, data = rd(h[3], h[4]), rd(h[7], h[8]), h[9]

    def dname(o):
        parts = []
        while o != 0:
            parent, _, _, _, _, nl = struct.unpack_from("<6I", dmeta, o)
            parts.append(dmeta[o + 24:o + 24 + nl].decode("utf-8", "replace"))
            o = parent
        return "/".join(reversed(parts))

    files, o = [], 0
    while o < len(fmeta):
        parent, _, off, size, _, nl = struct.unpack_from("<IIQQII", fmeta, o)
        name = fmeta[o + 32:o + 32 + nl].decode("utf-8", "replace")
        files.append((data + off, size, dname(parent) + "/" + name))
        o = (o + 32 + nl + 3) & ~3
    files.sort()
    return files


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    files = romfs_files(sys.argv[1])
    starts = [f[0] for f in files]

    def name_of(off):
        k = bisect.bisect_right(starts, off) - 1
        if k < 0 or off >= files[k][0] + max(files[k][1], 1):
            return "(romfs metadata)"
        return files[k][2]

    header, hitches, bursts, section = [], [], {}, None
    for line in open(sys.argv[2], encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("## hitches"):
            section = "h"
        elif line.startswith("## bursts"):
            section = "b"
        elif line.startswith("#"):
            header.append(line)
        elif line and section == "h":
            cols = line.split("\t")
            at, ms, b = cols[:3]
            guest = cols[3] if len(cols) > 3 else ""
            hitches.append((float(at), float(ms), None if b == "-" else int(b), guest))
        elif line and section == "b":
            cols = line.split("\t")
            reads_by_file = {}
            for r in cols[6].split():
                if "+" not in r or r.startswith("(+"):
                    continue
                off, ln = (int(x, 16) for x in r.split("+"))
                n = name_of(off)
                reads_by_file[n] = reads_by_file.get(n, 0) + ln
            bursts[int(cols[0])] = dict(start=float(cols[1]), wall=float(cols[2]),
                                        reads=int(cols[3]), kb=int(cols[4]),
                                        read_ms=float(cols[5]), files=reads_by_file)

    def show(bid, indent="    "):
        b = bursts.get(bid)
        if b is None:
            print(indent + f"burst {bid}: no longer in the report")
            return
        print(indent + f"burst {bid} at {b['start']:.2f} s: {b['wall']:.1f} ms wall, "
              f"{b['reads']} reads, {b['kb']} KB, {b['read_ms']:.1f} ms reading")
        for n, ln in sorted(b["files"].items(), key=lambda x: -x[1])[:12]:
            print(indent + f"  {ln / 1024:10.0f} KB  {n}")

    print("\n".join(header))
    print(f"\n{len(hitches)} hitches")
    for at, ms, b, guest in hitches:
        print(f"{at:9.2f} s  frame {ms:7.1f} ms" + ("" if b is not None else "  (no file reads)"))
        if guest:
            print(f"    guest: {guest}")
        if b is not None:
            show(b)
    which = sorted(bursts) if "--all" in sys.argv else \
        sorted(bursts, key=lambda k: -bursts[k]["wall"])[:15]
    print("\nall bursts" if "--all" in sys.argv else "\nslowest bursts")
    for k in which:
        show(k, "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
