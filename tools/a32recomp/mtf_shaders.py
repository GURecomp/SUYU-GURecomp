#!/usr/bin/env python3
"""Pull the compiled shader package out of an MT Framework NX game and index its programs.

usage: mtf_shaders.py <romfs.bin> <out dir> [arc path inside romfs]

  romfs.bin  the export's exefs/romfs.bin (a plain RomFS image)
  out dir    receives <name>.spk (the package), spk_index.txt (one program per line:
             "<hex offset of its program header>\t<sph shader_type>\t<bytes>") and
             romfs_list.txt (every RomFS file: offset, size, path)

Without an arc path the whole RomFS is searched for the archive entry that holds
Maxwell code (MHGU: nativeNX/sa/NX/root.arc -> sc\\NX\\root, magic "SPK\\0").
Feed the results to shader-scan to translate every program to SPIR-V:
  shader-scan <out>/<name>.spk <out>/spk_index.txt <out>/spv

Formats, as observed:
  RomFS   standard Switch layout (0x50 header, dir/file meta tables, data at data_off).
  .arc    "ARC\\0", u16 version (0x11), u16 count, u32 pad, then count x 80-byte entries
          {char name[64]; u32 type_hash; u32 comp_size; u32 size|flags; u32 offset},
          each entry zlib-compressed.
  SPK     programs are a 0x50-byte Maxwell program header (SPH) followed by code; each
          is preceded by a small block whose word at -0x30 is 0x12345678. Programs end
          with EXIT (0xE30000000007000F); the header's counts match the vertex and pixel
          programs found.
"""
import os, re, struct, sys, zlib

EXIT = struct.pack("<Q", 0xE30000000007000F)
MARKER = struct.pack("<I", 0x12345678)


def romfs_files(path):
    f = open(path, "rb")
    hs, dho, dhs, dmo, dms, fho, fhs, fmo, fms, data_off = struct.unpack("<10Q", f.read(80))
    f.seek(dmo); dm = f.read(dms)
    f.seek(fmo); fm = f.read(fms)

    def dir_name(off):
        parts = []
        while off != 0:
            parent, _, _, _, _, nl = struct.unpack_from("<6I", dm, off)
            parts.append(dm[off + 24:off + 24 + nl].decode("utf-8", "replace"))
            off = parent
        return "/".join(reversed(parts))

    files, off = [], 0
    while off < len(fm):
        parent, _, doff, size, _, nl = struct.unpack_from("<IIQQII", fm, off)
        name = fm[off + 32:off + 32 + nl].decode("utf-8", "replace")
        files.append((data_off + doff, size, dir_name(parent) + "/" + name))
        off += 32 + ((nl + 3) & ~3)
    return files


def arc_entries(blob):
    if blob[:4] != b"ARC\0":
        return
    count = struct.unpack_from("<H", blob, 6)[0]
    for k in range(count):
        e = 12 + k * 80
        name = blob[e:e + 64].split(b"\0")[0].decode("ascii", "replace")
        type_hash, comp, _, off = struct.unpack_from("<IIII", blob, e + 64)
        yield name, type_hash, blob[off:off + comp]


def index_programs(spk):
    starts = []
    for m in re.finditer(re.escape(MARKER), spk):
        p = m.start() + 0x30
        if p + 0x50 <= len(spk):
            w0 = struct.unpack_from("<I", spk, p)[0]
            if (w0 & 0x1F) in (1, 2) and ((w0 >> 5) & 0x1F) == 3:
                starts.append(p)
    out = []
    for i, p in enumerate(starts):
        limit = (starts[i + 1] - 0x30) if i + 1 < len(starts) else len(spk)
        last = spk.rfind(EXIT, p, limit)
        length = (last + 8 - p) if last >= 0 else (limit - p)
        shader_type = (struct.unpack_from("<I", spk, p)[0] >> 10) & 0xF
        out.append((p, shader_type, length))
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    romfs, out_dir = sys.argv[1], sys.argv[2]
    want = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out_dir, exist_ok=True)
    files = romfs_files(romfs)
    with open(os.path.join(out_dir, "romfs_list.txt"), "w", encoding="utf-8") as lst:
        lst.writelines(f"{o}\t{s}\t{n}\n" for o, s, n in files)
    arcs = [x for x in files if x[2].endswith(".arc") and (want is None or x[2] == want)]
    f = open(romfs, "rb")
    found = None
    for i, (o, s, n) in enumerate(arcs):
        f.seek(o)
        for name, type_hash, comp in arc_entries(f.read(s)):
            data = zlib.decompress(comp)
            if EXIT in data:
                found = (n, name, type_hash, data)
                break
        if found:
            break
        if i % 2000 == 0:
            print(f"  searched {i}/{len(arcs)} archives", flush=True)
    if not found:
        sys.exit("no archive entry with Maxwell code found")
    arc, name, type_hash, spk = found
    base = name.replace("\\", "_")
    open(os.path.join(out_dir, base + ".spk"), "wb").write(spk)
    progs = index_programs(spk)
    with open(os.path.join(out_dir, "spk_index.txt"), "w") as idx:
        idx.writelines(f"{p:x}\t{t}\t{n}\n" for p, t, n in progs)
    vs = sum(1 for _, t, _ in progs if t == 1)
    unique = len({spk[p:p + n] for p, _, n in progs})
    print(f"{arc} -> {name} (type {type_hash:08x}, {len(spk)} bytes): "
          f"{len(progs)} programs ({vs} vertex, {len(progs) - vs} pixel), {unique} unique")


if __name__ == "__main__":
    main()
