"""Pipeline manifest: suyu's Vulkan shader cache (vulkan.bin) without any game code in it.

  pipeline_manifest.py make <out.pmf> <package.spk> <vulkan.bin>...
      [--source <romfs path of the archive> <entry name>]  (default: MHGU's)
  pipeline_manifest.py build <in.pmf> <package.spk> <out vulkan.bin>
  pipeline_manifest.py check <package.spk> <vulkan.bin>      (make + build, compare bytes)

A cached pipeline is, per shader stage, the program's code (as the GPU saw it) plus small tables
the translator read at run time (texture types, pixel formats, constant buffer values, replaced
values), a few environment words and the program header; then the pipeline key (fixed-function
state + shader hashes). The code is a slice of the game's shader package (the .spk inside
nativeNX/sa/NX/root.arc), so the manifest keeps the package offset and length instead of the
bytes: everything left is numbers, and each player's exporter rebuilds the cache from their own
copy of the game. Caches from several sessions merge (identical entries kept once).

Program code in the cache is either a contiguous slice of the package, or the program followed
by the 16-byte terminator the GPU memory had after it (a branch-to-self and a fixed word; seen
on 7% of programs). Entries that match neither are left out and counted.

Manifest format (little endian), version 1:
  "SPMF", u32 version, u32 cache version, u32 key size, u32 package size, u32 package crc32,
  u32 pipeline count, u16 + UTF-8 RomFS path of the MT archive holding the package, u16 + name
  of the package's entry in that archive; per pipeline: u32 stage count; per stage: u32 package offset,
  u32 code size, u32 program size (code size, or the program part when the terminator follows),
  u8 header copy matches code start (1) / raw 0x50 bytes follow (0), then the environment:
  4 u64 table counts, 7 u32 words, the tables, [0x50 program header if not matching], then the
  key. Compute and geometry stages are not handled (MHGU uses vertex + pixel only).
"""
import struct, sys, zlib

MAGIC = b"SPMF"
VERSION = 1
KEY_SIZE = 1056
SPH = 0x50
DEFAULT_SOURCE = ("nativeNX/sa/NX/root.arc", r"sc\NX\root")  # MHGU 1.4.0
TERMINATOR = bytes.fromhex("0f0087ffff0f40e23412769801000000")
STAGE_COMPUTE, STAGE_GEOMETRY = 5, 3  # Shader::Stage values that carry extra trailing words


def read_cache(path):
    d = open(path, "rb").read()
    if d[:8] != b"yuzucach":
        raise ValueError(f"{path}: not a pipeline cache")
    ver = struct.unpack_from("<I", d, 8)[0]
    p, pipes = 12, []
    while p < len(d):
        (n,) = struct.unpack_from("<I", d, p)
        p += 4
        stages = []
        for _ in range(n):
            counts = struct.unpack_from("<5Q", d, p)
            p += 40
            words = struct.unpack_from("<7I", d, p)
            p += 28
            code_size, ntt, ntpf, ncb, ncr = counts
            code = d[p:p + code_size]
            p += code_size
            tsize = ntt * 8 + ntpf * 8 + ncb * 12 + ncr * 12
            tables = d[p:p + tsize]
            p += tsize
            stage = words[6]
            if stage in (STAGE_COMPUTE, STAGE_GEOMETRY):
                raise ValueError(f"{path}: stage {stage} not supported")
            sph = d[p:p + SPH]
            p += SPH
            stages.append((counts[1:], words, code, tables, sph))
        key = d[p:p + KEY_SIZE]
        p += KEY_SIZE
        pipes.append((stages, key))
    if p != len(d):
        raise ValueError(f"{path}: trailing bytes")
    return ver, pipes


MARKER = struct.pack("<I", 0x12345678)  # word 0x30 bytes before every program header


def index_package(spk):
    """First 0x80 bytes of every program -> offsets. Programs (0x50-byte header + code) follow
    a small block whose word at -0x30 is MARKER (see mtf_shaders.py)."""
    starts = {}
    pos = spk.find(MARKER)
    while pos >= 0:
        off = pos + 0x30
        if off + 0x80 <= len(spk):
            starts.setdefault(spk[off:off + 0x80], []).append(off)
        pos = spk.find(MARKER, pos + 4)
    return starts


def locate(spk, starts, code):
    """(offset, program size) of code inside the package, or None."""
    for off in starts.get(code[:0x80], ()):
        if spk[off:off + len(code)] == code:
            return off, len(code)
        body = len(code) - len(TERMINATOR)
        if code[body:] == TERMINATOR and spk[off:off + body] == code[:body]:
            return off, body
    return None


def make(out, spk_path, caches, source=DEFAULT_SOURCE):
    spk = open(spk_path, "rb").read()
    starts = index_package(spk)
    cache_ver = None
    seen, entries, skipped = set(), [], 0
    for path in caches:
        ver, pipes = read_cache(path)
        if cache_ver is None:
            cache_ver = ver
        elif ver != cache_ver:
            raise ValueError(f"{path}: cache version {ver}, expected {cache_ver}")
        for stages, key in pipes:
            rec = bytearray(struct.pack("<I", len(stages)))
            ok = True
            for tables_counts, words, code, tables, sph in stages:
                hit = locate(spk, starts, code)
                if hit is None:
                    ok = False
                    break
                off, prog = hit
                same = sph == code[:SPH]
                rec += struct.pack("<IIIB", off, len(code), prog, 1 if same else 0)
                rec += struct.pack("<4Q", *tables_counts) + struct.pack("<7I", *words) + tables
                if not same:
                    rec += sph
            if not ok:
                skipped += 1
                continue
            rec += key
            if bytes(rec) not in seen:
                seen.add(bytes(rec))
                entries.append(bytes(rec))
    with open(out, "wb") as f:
        f.write(MAGIC + struct.pack("<6I", VERSION, cache_ver, KEY_SIZE, len(spk),
                                    zlib.crc32(spk) & 0xffffffff, len(entries)))
        for text in source:
            raw = text.encode("utf-8")
            f.write(struct.pack("<H", len(raw)) + raw)
        for e in entries:
            f.write(e)
    print(f"{out}: {len(entries)} pipelines, {skipped} left out (code not in the package)")


def build(pmf, spk_path, out):
    spk = open(spk_path, "rb").read()
    d = open(pmf, "rb").read()
    if d[:4] != MAGIC:
        raise ValueError("not a manifest")
    version, cache_ver, key_size, spk_size, spk_crc, count = struct.unpack_from("<6I", d, 4)
    if version != VERSION or key_size != KEY_SIZE:
        raise ValueError("manifest version / key size mismatch")
    if spk_size != len(spk) or spk_crc != (zlib.crc32(spk) & 0xffffffff):
        raise ValueError("this manifest was made from a different shader package")
    p = 28
    for _ in range(2):  # archive path, entry name
        (n,) = struct.unpack_from("<H", d, p)
        p += 2 + n
    o = bytearray(b"yuzucach" + struct.pack("<I", cache_ver))
    for _ in range(count):
        (n,) = struct.unpack_from("<I", d, p)
        p += 4
        o += struct.pack("<I", n)
        for _ in range(n):
            off, code_size, prog, same = struct.unpack_from("<IIIB", d, p)
            p += 13
            counts = struct.unpack_from("<4Q", d, p)
            p += 32
            words = d[p:p + 28]
            p += 28
            tsize = counts[0] * 8 + counts[1] * 8 + counts[2] * 12 + counts[3] * 12
            tables = d[p:p + tsize]
            p += tsize
            code = spk[off:off + prog] + (TERMINATOR if prog != code_size else b"")
            assert len(code) == code_size
            if same:
                sph = code[:SPH]
            else:
                sph = d[p:p + SPH]
                p += SPH
            o += struct.pack("<5Q", code_size, *counts) + words + code + tables + sph
        o += d[p:p + key_size]
        p += key_size
    open(out, "wb").write(o)
    print(f"{out}: {count} pipelines")


def main():
    cmd = sys.argv[1]
    if cmd == "make":
        args = sys.argv[4:]
        source = DEFAULT_SOURCE
        if "--source" in args:
            i = args.index("--source")
            source = (args[i + 1], args[i + 2])
            del args[i:i + 3]
        make(sys.argv[2], sys.argv[3], args, source)
    elif cmd == "build":
        build(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == "check":
        import os, tempfile
        spk, cache = sys.argv[2], sys.argv[3]
        tmp = tempfile.mkdtemp()
        make(os.path.join(tmp, "m.pmf"), spk, [cache])
        build(os.path.join(tmp, "m.pmf"), spk, os.path.join(tmp, "v.bin"))
        a, b = open(cache, "rb").read(), open(os.path.join(tmp, "v.bin"), "rb").read()
        print("identical" if a == b else f"DIFFERENT ({len(a)} vs {len(b)} bytes)")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
