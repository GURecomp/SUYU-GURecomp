"""Name the GPU methods in nvn_methods.txt (the method census the recomp writes to the user dir).

usage: nvn_methods_names.py <nvn_methods.txt> <suyu src dir> [out file]

Each "sc.mmm=value" (subchannel, method in words) becomes "sc.mmm<register+offset>=value". The
engine on each subchannel is found from BindObject (method 0) writes in the census: class
0xB197 = Maxwell 3D, 0xB1C0 = compute, 0xA140 = inline to memory, 0x902D = 2D, 0xB0B5 = DMA.
Subchannels never bound in the census default to 0 = 3D, 1 = compute, 2 = inline to memory,
3 = 2D, 4 = DMA. Register positions come from suyu's ASSERT_REG_POSITION tables (method index
= register word index; headers that assert byte offsets are divided by 4); a method inside an
array or struct is shown as the nearest register at or below it plus the word offset. Methods below 0x40 are the channel's own (dma_pusher.h).
"""
import bisect, os, re, sys

CLASSES = {0xB197: "engines/maxwell_3d.h", 0xB1C0: "engines/kepler_compute.h",
           0xA140: "engines/kepler_memory.h", 0x902D: "engines/fermi_2d.h",
           0xB0B5: "engines/maxwell_dma.h"}
DEFAULT = {0: 0xB197, 1: 0xB1C0, 2: 0xA140, 3: 0x902D, 4: 0xB0B5}


def positions(path):
    text = open(path, encoding="utf-8").read()
    # Some headers assert byte offsets, some word indices ("== position * 4").
    macro = text[text.index("#define ASSERT_REG_POSITION"):][:300]
    scale = 1 if "position * 4" in macro else 4
    table = {}
    for m in re.finditer(r"ASSERT_REG_POSITION\(([\w\[\]\.]+),\s*(0x[0-9A-Fa-f]+)\)", text):
        table.setdefault(int(m.group(2), 16) // scale, m.group(1))
    keys = sorted(table)
    return keys, [table[k] for k in keys]


def channel_methods(src):
    names = {}
    text = open(os.path.join(src, "video_core/dma_pusher.h"), encoding="utf-8").read()
    body = text[text.index("enum class BufferMethods"):]
    body = body[:body.index("};")]
    for m in re.finditer(r"(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", body):
        names[int(m.group(2), 16)] = m.group(1)
    return names


def main():
    census, src = sys.argv[1], os.path.join(sys.argv[2], "")
    text = open(census, encoding="utf-8").read()
    bound = dict(DEFAULT)
    for sc, cls in re.findall(r"\b(\d)\.000=([0-9a-f]+)", text):
        if int(cls, 16) in CLASSES:
            bound[int(sc)] = int(cls, 16)
    tables = {cls: positions(os.path.join(src, "video_core", rel)) for cls, rel in CLASSES.items()}
    chan = channel_methods(src)

    def name(sc, method):
        if method < 0x40:
            return chan.get(method, "channel")
        if bound.get(sc) == 0xB197 and method >= 0xE00:
            # Maxwell 3D macro calls: two methods per macro, start (with the first argument)
            # then further arguments.
            n = (method - 0xE00) // 2
            return f"macro{n:#x}" + ("" if method % 2 == 0 else ".arg")
        keys, vals = tables[bound.get(sc, 0xB197)]
        i = bisect.bisect_right(keys, method) - 1
        if i < 0:
            return "?"
        off = method - keys[i]
        return vals[i] + (f"+{off}" if off else "")

    def sub(m):
        sc, method = int(m.group(1)), int(m.group(2), 16)
        return f"{m.group(1)}.{m.group(2)}<{name(sc, method)}>="

    out = re.sub(r"\b(\d)\.([0-9a-f]{3,4})=", sub, text)
    header = "# subchannels: " + ", ".join(
        f"{sc}={os.path.basename(CLASSES[c]).replace('.h', '')}" for sc, c in sorted(bound.items())) + "\n"
    dest = sys.argv[3] if len(sys.argv) > 3 else census.replace(".txt", "_named.txt")
    open(dest, "w", encoding="utf-8").write(header + out)
    print("wrote", dest)


if __name__ == "__main__":
    main()
