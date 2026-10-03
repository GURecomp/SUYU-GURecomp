#!/usr/bin/env python3
"""Differential tester: a32_to_c generated C vs Unicorn, instruction by instruction.

Usage:
  difftest.py random N [seed]          random ARM encodings (filtered to defined ones)
  difftest.py corpus FILE [N] [seed]   instruction words from a file (hex per line, or raw .bin)

Each case: identical random initial state on both sides, run the instruction(s),
compare r0-r15, NZCVQ, GE, Thumb, VFP registers, FPSCR.NZCV and memory writes.
"""
import ctypes, os, random, struct, subprocess, sys, collections, math
import capstone as cs
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_HOOK_INTR, UcError
from unicorn import arm_const as A

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "build")
CODE = 0x00100000

def pat(a):
    return ((a * 0x9E3779B1) & 0xFFFFFFFF) >> 24

class Ctx(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32),
                ("c", ctypes.c_uint32), ("v", ctypes.c_uint32), ("q", ctypes.c_uint32),
                ("ge", ctypes.c_uint32), ("thumb", ctypes.c_uint32), ("fpscr", ctypes.c_uint32),
                ("s", ctypes.c_uint32 * 64), ("halted", ctypes.c_uint32),
                ("tpidruro", ctypes.c_uint32), ("tpidrurw", ctypes.c_uint32), ("pending_svc", ctypes.c_uint32),
                ("host", ctypes.c_void_p)]

# ---------------------------------------------------------------- unicorn side
class UcRunner:
    def __init__(self):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        # "max" = ARMv8 features in AArch32 (LDA/STL, VSEL, VRINT, CRC32...), like the Switch's A57
        self.uc.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
        self.uc = self.uc
        self.mapped = set()
        self.writes = {}
        self.uc.mem_map(CODE, 0x10000)
        self.uc.mem_write(CODE, bytes(pat(CODE + k) for k in range(0x10000)))
        self.mapped.update(range(CODE >> 12, (CODE + 0x10000) >> 12))
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._write)
        self.uc.hook_add(UC_HOOK_INTR, self._intr)
        from unicorn import UC_HOOK_CODE
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self.limit = 0; self.executed = 0; self.snap = None
        # enable VFP / Advanced SIMD
        self.uc.reg_write(A.UC_ARM_REG_C1_C0_2, self.uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        self.uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        self.intr = False

    def _map_page(self, page):
        base = page << 12
        self.uc.mem_map(base, 0x1000)
        self.uc.mem_write(base, bytes(pat(base + k) for k in range(0x1000)))
        self.mapped.add(page)

    def _unmapped(self, uc, access, addr, size, value, data):
        for page in {addr >> 12, (addr + max(size, 1) - 1) >> 12}:
            if page not in self.mapped:
                self._map_page(page)
        return True

    def _write(self, uc, access, addr, size, value, data):
        if self.snap is not None:
            return
        for k in range(size):
            self.writes[(addr + k) & 0xFFFFFFFF] = (value >> (8 * k)) & 0xFF

    def _code(self, uc, addr, size, data):
        # stop before running anything past the instructions under test
        if self.executed >= self.limit:
            if self.snap is None:
                self.snap = self._state()
            uc.emu_stop()
            return
        self.executed += 1

    def _intr(self, uc, intno, data):
        self.intr = True
        uc.emu_stop()

    def run(self, words, st):
        uc = self.uc
        code = b"".join(struct.pack("<I", w) for w in words)
        if getattr(self, "last_len", 0) > len(code):
            uc.mem_write(CODE + len(code), bytes(pat(CODE + k) for k in range(len(code), self.last_len)))
        self.last_len = len(code)
        uc.mem_write(CODE, code)
        uc.ctl_remove_cache(CODE, CODE + 0x1000)
        # restore memory touched by the previous case
        for a in self.writes:
            if (a >> 12) in self.mapped and not (CODE <= a < CODE + self.last_len):
                uc.mem_write(a, bytes([pat(a)]))
        self.writes = {}
        self.intr = False
        cpsr = (st["n"] << 31) | (st["z"] << 30) | (st["c"] << 29) | (st["v"] << 28) | (st["q"] << 27) | (st["ge"] << 16) | 0x10
        uc.reg_write(A.UC_ARM_REG_CPSR, cpsr)
        for k in range(13):
            uc.reg_write(A.UC_ARM_REG_R0 + k, st["r"][k])
        uc.reg_write(A.UC_ARM_REG_SP, st["r"][13])
        uc.reg_write(A.UC_ARM_REG_LR, st["r"][14])
        uc.reg_write(A.UC_ARM_REG_FPSCR, st["fpscr"])
        uc.reg_write(A.UC_ARM_REG_C13_C0_3, st["tls"][0])
        for k in range(32):
            uc.reg_write(A.UC_ARM_REG_D0 + k, st["d"][k])
        self.limit = len(words); self.executed = 0
        self.snap = None
        uc.emu_start(CODE, 0xFFFFFFF0)
        return self.snap if self.snap is not None else self._state()

    def _state(self):
        uc = self.uc
        out = {"r": [uc.reg_read(A.UC_ARM_REG_R0 + k) for k in range(13)] +
                    [uc.reg_read(A.UC_ARM_REG_SP), uc.reg_read(A.UC_ARM_REG_LR), uc.reg_read(A.UC_ARM_REG_PC)]}
        cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
        out.update(n=cpsr >> 31, z=(cpsr >> 30) & 1, c=(cpsr >> 29) & 1, v=(cpsr >> 28) & 1, q=(cpsr >> 27) & 1,
                   ge=(cpsr >> 16) & 15, thumb=(cpsr >> 5) & 1, fpscr=uc.reg_read(A.UC_ARM_REG_FPSCR),
                   d=[uc.reg_read(A.UC_ARM_REG_D0 + k) for k in range(32)], writes=dict(self.writes), svc=self.intr,
                   tls=[uc.reg_read(A.UC_ARM_REG_C13_C0_3), 0])
        return out

# ---------------------------------------------------------------- state generation
def rand_float(rng):
    k = rng.random()
    if k < 0.1: return rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0])
    if k < 0.3: return float(rng.randint(-100000, 100000))
    if k < 0.4: return rng.uniform(-4e9, 4e9)
    return rng.uniform(-1000, 1000) * 10 ** rng.randint(-6, 6)

def f2b(x): return struct.unpack("<I", struct.pack("<f", x))[0]
def d2b(x): return struct.unpack("<Q", struct.pack("<d", x))[0]

SPECIAL32 = [0x7FC00000, 0xFFC00000, 0x7FC12345, 0xFFC54321, 0x7F800001, 0xFF812345, 0x7FA00000,
             0x7F800000, 0xFF800000, 0x00000001, 0x80000001, 0x007FFFFF, 0x00000000, 0x80000000]
SPECIAL64 = [0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000012345678, 0xFFF8000087654321,
             0x7FF0000000000001, 0xFFF0000012345678, 0x7FF4000000000000, 0x7FF0000000000000,
             0xFFF0000000000000, 0x0000000000000001, 0x8000000000000001, 0x000FFFFFFFFFFFFF, 0, 1 << 63]

def special(rng, dbl, v):
    """Replace a register value with a NaN/inf/denormal/zero pattern some of the time."""
    if rng.random() >= STRICT_SPECIAL: return v
    if dbl: return rng.choice(SPECIAL64)
    lo, hi = v & 0xFFFFFFFF, v >> 32
    if rng.random() < 0.7: lo = rng.choice(SPECIAL32)
    if rng.random() < 0.5: hi = rng.choice(SPECIAL32)
    return lo | (hi << 32)

STRICT_SPECIAL = 0.25

def rand_state(rng, dbl):
    r = [rng.getrandbits(32) for _ in range(15)]
    for k in range(15):
        if rng.random() < 0.15: r[k] = rng.choice([0, 1, 2, 31, 32, 33, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, rng.randint(0, 64)])
    d = []
    for k in range(32):
        if dbl: d.append(special(rng, True, d2b(rand_float(rng))))
        else: d.append(special(rng, False, f2b(rand_float(rng)) | (f2b(rand_float(rng)) << 32)))
    return {"r": r, "n": rng.getrandbits(1), "z": rng.getrandbits(1), "c": rng.getrandbits(1), "v": rng.getrandbits(1),
            "q": 0, "ge": rng.getrandbits(4), "fpscr": 0, "d": d,
            "tls": [rng.getrandbits(32), 0]}  # Unicorn ignores writes to TPIDRURW

def is_vfp_double(w):
    return ((w >> 25) & 7) in (6, 7) and ((w >> 9) & 7) == 5 and (w >> 8) & 1

# ---------------------------------------------------------------- C side
def build(cases):
    os.makedirs(WORK, exist_ok=True)
    src = os.path.join(WORK, "cases.c")
    inp = "\n".join(f"case_{k} {CODE:x} " + " ".join(f"{w:08x}" for w in c["words"]) for k, c in enumerate(cases))
    res = subprocess.run([os.path.join(WORK, "a32gen"), src], input=inp, capture_output=True, text=True, check=True)
    status = [l.split()[1] for l in res.stdout.strip().splitlines()]
    so = os.path.join(WORK, "cases.so")
    subprocess.run(["gcc", "-O1", "-shared", "-fPIC", "-ffp-contract=off", "-w", "-I", HERE, "-o", so, src,
                    os.path.join(HERE, "test_host.c"), "-lm"], check=True)
    lib = ctypes.CDLL(so)
    lib.th_run.argtypes = [ctypes.c_uint, ctypes.POINTER(Ctx)]
    lib.th_writes.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
    return lib, status

def run_c(lib, idx, st):
    c = Ctx()
    for k in range(15): c.r[k] = st["r"][k]
    c.r[15] = CODE
    c.n, c.z, c.c, c.v, c.q, c.ge, c.fpscr = st["n"], st["z"], st["c"], st["v"], st["q"], st["ge"], st["fpscr"]
    c.tpidruro, c.tpidrurw = st["tls"]
    for k in range(32):
        c.s[2 * k] = st["d"][k] & 0xFFFFFFFF
        c.s[2 * k + 1] = st["d"][k] >> 32
    ws = st["words"]
    lib.th_set_code(CODE, (ctypes.c_uint32 * len(ws))(*ws), len(ws))
    flags = lib.th_run(idx, ctypes.byref(c))
    addrs = (ctypes.c_uint32 * 8192)(); vals = (ctypes.c_uint8 * 8192)()
    n = lib.th_writes(addrs, vals, 8192)
    return {"r": list(c.r), "n": c.n, "z": c.z, "c": c.c, "v": c.v, "q": c.q, "ge": c.ge, "thumb": c.thumb,
            "fpscr": c.fpscr, "d": [c.s[2 * k] | (c.s[2 * k + 1] << 32) for k in range(32)],
            "writes": {addrs[k]: vals[k] for k in range(n)}, "svc": bool(flags & 2), "unhandled": bool(flags & 1),
            "tls": [c.tpidruro, c.tpidrurw]}

def isnan32(b): return (b >> 23) & 0xFF == 0xFF and (b & 0x7FFFFF) != 0
def isnan64(b): return (b >> 52) & 0x7FF == 0x7FF and (b & ((1 << 52) - 1)) != 0

def compare(a, b):
    diffs = []
    for k in range(16):
        if a["r"][k] != b["r"][k]: diffs.append(f"r{k}: c={a['r'][k]:08x} uc={b['r'][k]:08x}")
    for f in ("n", "z", "c", "v", "q", "ge", "thumb"):
        if a[f] != b[f]: diffs.append(f"{f}: c={a[f]} uc={b[f]}")
    if (a["fpscr"] >> 28) != (b["fpscr"] >> 28): diffs.append(f"fpscr.nzcv: c={a['fpscr']>>28:x} uc={b['fpscr']>>28:x}")
    for k in range(32):
        x, y = a["d"][k], b["d"][k]
        if x != y: diffs.append(f"d{k}: c={x:016x} uc={y:016x}")  # bit-exact, NaN payloads included
    if a["writes"] != b["writes"]:
        ks = sorted(set(a["writes"]) | set(b["writes"]))
        bad = [k for k in ks if a["writes"].get(k) != b["writes"].get(k)]
        diffs.append("mem: " + ", ".join(f"{k:08x}: c={a['writes'].get(k)} uc={b['writes'].get(k)}" for k in bad[:6]))
    if a["svc"] != b["svc"]: diffs.append(f"svc: c={a['svc']} uc={b['svc']}")
    if a["tls"] != b["tls"]: diffs.append(f"tls: c={a['tls']} uc={b['tls']}")
    return diffs

# ---------------------------------------------------------------- main
MD = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_ARM | cs.CS_MODE_V8)

# (fixed-bit mask, value) templates for ARMv8 / NEON forms the lifter supports
V8_TEMPLATES = [
    (0xFF800E50, 0xFE000A00),  # VSEL
    (0xFFB00E10, 0xFE800A00),  # VMAXNM/VMINNM
    (0xFFBC0ED0, 0xFEB80A40),  # VRINT{A,N,P,M}
    (0xFFBC0E50, 0xFEBC0A40),  # VCVT{A,N,P,M}
    (0x0FBF0ED0, 0x0EB60A40),  # VRINTR/Z (conditional)
    (0x0FBF0ED0, 0x0EB70A40),  # VRINTX
    (0xFEB80090, 0xF2800010),  # NEON modified immediate
    (0xFE800F10, 0xF2000110),  # NEON bitwise
    (0xFFA00F10, 0xF2000F00),  # VMAX/VMIN.F32
    (0xFFB00010, 0xF2B00000),  # VEXT
    (0xFFB00F90, 0xF3B00C00),  # VDUP scalar
    (0xFF900000, 0xF4000000),  # VLD1/VST1 multiple
    (0xFF900300, 0xF4800000),  # VLD1/VST1 single lane
    (0x0F900F5F, 0x0E800B10),  # VDUP core
    (0x0F900F7F, 0x0E000B10),  # VMOV scalar<->core
    (0x0F800CF0, 0x01800C90),  # LDA/STL/LDAEX/STLEX families
    (0x0F900DF0, 0x01000040),  # CRC32
    (0x0FFF0FDF, 0x0E1D0F50),  # MRC/MCR TPIDRURW/URO
]

def disasm(w):
    for ins in MD.disasm(struct.pack("<I", w), CODE):
        return f"{ins.mnemonic} {ins.op_str}"
    return None

def main():
    mode = sys.argv[1]
    if mode == "v8":
        n = int(sys.argv[2]); seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        rng = random.Random(seed)
        words = []
        while len(words) < n:
            mask, val = rng.choice(V8_TEMPLATES)
            w = (rng.getrandbits(32) & ~mask) | val
            if not (mask >> 28): w = (w & 0x0FFFFFFF) | (0xE << 28)
            if disasm(w) is not None: words.append(w)
    elif mode == "vfp":
        n = int(sys.argv[2]); seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        rng = random.Random(seed)
        words = []
        while len(words) < n:
            w = rng.getrandbits(32)
            top = rng.choice([0xE, 0xE, 0xE, 0xD, 0xC])
            w = (w & 0x00FFF1FF) | (top << 24) | (5 << 9)
            if top == 0xE and rng.random() < 0.7: w &= ~0x10  # mostly data processing
            w |= (0xE if rng.random() < 0.8 else rng.randint(0, 13)) << 28
            if disasm(w) is not None: words.append(w)
    elif mode == "random":
        n = int(sys.argv[2]); seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        rng = random.Random(seed)
        words = []
        while len(words) < n:
            w = rng.getrandbits(32)
            w = (w & 0x0FFFFFFF) | ((0xE if rng.random() < 0.7 else rng.randint(0, 13)) << 28)
            if disasm(w) is not None: words.append(w)
    else:
        path = sys.argv[2]; n = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
        seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
        rng = random.Random(seed)
        if path.endswith(".txt"): words = [int(l, 16) for l in open(path) if l.strip()]
        else:
            data = open(path, "rb").read(); words = list(struct.unpack(f"<{len(data)//4}I", data[:len(data)//4*4]))
        words = list(dict.fromkeys(words))
        rng.shuffle(words); words = words[:n]
    cases = [{"words": [w], "st": rand_state(rng, is_vfp_double(w))} for w in words]
    for c in cases: c["st"]["words"] = c["words"]
    lib, status = build(cases)
    ucr = UcRunner()
    tally = collections.Counter(); fails = collections.defaultdict(list)
    for k, c in enumerate(cases):
        if status[k] != "ok": tally["unhandled"] += 1; continue
        try:
            b = ucr.run(c["words"], c["st"])
        except UcError as e:
            tally["uc_error"] += 1; continue
        if b["svc"] and b["r"][15] == CODE:  # CPU exception (e.g. alignment fault), not an SVC
            tally["uc_fault"] += 1; continue
        a = run_c(lib, k, c["st"])
        d = compare(a, b)
        if d:
            tally["FAIL"] += 1
            key = (disasm(c["words"][0]) or "?").split()[0]
            fails[key].append((c["words"][0], d))
        else:
            tally["pass"] += 1
    total = sum(tally.values())
    print(f"cases {total}: " + ", ".join(f"{k} {v}" for k, v in sorted(tally.items())))
    tested = tally["pass"] + tally["FAIL"]
    if tested: print(f"pass rate on translated+executable cases: {100*tally['pass']/tested:.3f}%")
    for key, lst in sorted(fails.items(), key=lambda kv: -len(kv[1]))[:25]:
        w, d = lst[0]
        print(f"  FAIL x{len(lst):4d} {key:10s} e.g. {w:08x} [{disasm(w)}]: {'; '.join(d[:3])}")

if __name__ == "__main__":
    main()
