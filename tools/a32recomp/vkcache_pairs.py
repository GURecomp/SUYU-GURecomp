"""Read suyu's shader cache (user/cache/shader/<title>/vulkan.bin) and list the vertex+pixel
program pairs of every graphics pipeline a run built, as unique program numbers of the
game's shader package (shader-scan / pipeline-test numbering).

usage: vkcache_pairs.py <vulkan.bin> <package.spk> <spk_index.txt> <out pairs.txt> [--unmatched]
  The pairs file feeds pipeline-test, which then rebuilds exactly those pipelines headless.
  --unmatched also lists cached programs that aren't found in the package.

Format, as written by this tree (magic "yuzucach", version 18): per pipeline a u32 stage
count, then per stage: u64 code size and four u64 table counts, seven u32 (local memory,
texture cbuf, start, lowest, highest, viewport transform, stage), the code (program header
first), the tables (texture types 8 B, pixel formats 8 B, cbuf values 12 B, replacements
12 B), then a 0x50-byte copy of the program header; then the 1056-byte pipeline key.
"""
import collections, struct, sys

KEY_SIZE = 1056  # sizeof(GraphicsPipelineCacheKey) in this build

def read_envs(path):
    d = open(path, "rb").read()
    assert d[:4] == b"yuzu", d[:8]
    ver = struct.unpack_from("<I", d, 8)[0]
    p = 12
    pipes = []
    while p < len(d):
        (n,) = struct.unpack_from("<I", d, p); p += 4
        envs = []
        for _ in range(n):
            code_size, ntt, ntpf, ncb, ncr = struct.unpack_from("<5Q", d, p); p += 40
            lm, tb, start, lo, hi, vts, stage = struct.unpack_from("<7I", d, p); p += 28
            code = d[p:p + code_size]; p += code_size
            p += ntt * 8 + ntpf * 8 + ncb * 12 + ncr * 12 + 0x50  # tables, then a copy of the SPH
            envs.append({"stage": stage, "code": code, "start": start, "lo": lo, "hi": hi})
        key = d[p:p + KEY_SIZE]; p += KEY_SIZE
        pipes.append((envs, key))
    assert p == len(d), (p, len(d))
    return ver, pipes

def spk_programs(spk_path, index_path):
    spk = open(spk_path, "rb").read()
    seen, out = {}, []
    for line in open(index_path):
        f = line.split()
        if len(f) < 3:
            continue
        off, t, size = int(f[0], 16), int(f[1]), int(f[2])
        blob = spk[off:off + size]
        if blob in seen:
            continue
        seen[blob] = len(out)
        out.append((blob, t == 5))
    return out

def main():
    ver, pipes = read_envs(sys.argv[1])
    progs = spk_programs(sys.argv[2], sys.argv[3])
    by_prefix = collections.defaultdict(list)
    for n, (blob, fs) in enumerate(progs):
        by_prefix[blob[:0x80]].append(n)

    def match(code):
        for o in range(0, min(len(code), 0x400), 8):
            for n in by_prefix.get(code[o:o + 0x80], ()):
                blob = progs[n][0]
                m = min(len(blob), len(code) - o)
                if code[o:o + m] == blob[:m]:
                    return n
        return None

    stages = collections.Counter()
    pairs = collections.Counter()
    states = set()
    unmatched = 0
    for envs, key in pipes:
        stages[tuple(e["stage"] for e in envs)] += 1
        ns = [match(e["code"]) for e in envs]
        if any(n is None for n in ns):
            unmatched += 1
            continue
        vs = [n for n in ns if not progs[n][1]]
        fs = [n for n in ns if progs[n][1]]
        if len(vs) == 1 and len(fs) == 1:
            pairs[(vs[0], fs[0])] += 1
        states.add(key[48:])  # key minus the six 64-bit shader hashes
    with open(sys.argv[4], "w") as f:
        for (v, s), c in sorted(pairs.items()):
            f.write(f"{v} {s}\n")
    used = {n for p in pairs for n in p}
    print(f"cache v{ver}: {len(pipes)} pipelines, stage sets {dict(stages)}")
    print(f"matched to SPK: {len(pipes) - unmatched}, unmatched {unmatched}")
    print(f"distinct VS+PS pairs {len(pairs)}, programs used {len(used)} "
          f"({sum(1 for n in used if not progs[n][1])} VS, {sum(1 for n in used if progs[n][1])} PS)")
    print(f"distinct fixed-function states {len(states)}")

main()

def unmatched_report():
    ver, pipes = read_envs(sys.argv[1])
    spk = open(sys.argv[2], "rb").read()
    progs = spk_programs(sys.argv[2], sys.argv[3])
    pre = {b[:0x80] for b, _ in progs}
    seen = set()
    for envs, key in pipes:
        for e in envs:
            c = e["code"]
            if c[:0x80] in pre or c in seen:
                continue
            seen.add(c)
            body = c[0x50:0x50 + 64]
            print(f"stage {e['stage']} size {len(c):#x} in spk: {spk.find(body) >= 0} sph {c[:8].hex()}")

if "--unmatched" in sys.argv[5:]:
    unmatched_report()
