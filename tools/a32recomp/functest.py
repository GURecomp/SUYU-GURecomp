#!/usr/bin/env python3
"""Whole-function differential test on real module code.

  functest.py <subset_dir> <base_hex> [cases_per_function] [seed] [--no-page-table]

<subset_dir> comes from a32subset (emitted C + image.bin + functions.txt).
Each function runs to completion in Unicorn and in the recompiled C from the
same random arguments; registers, flags, VFP state and every byte written are
compared. A case counts only if both sides return to the caller.
"""
import ctypes, glob, os, random, struct, subprocess, sys, collections
import numpy as np
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_HOOK_INTR, UcError
from unicorn import arm_const as A

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from difftest import rand_float, f2b, d2b, isnan32, isnan64, special

STACK_VA, STACK_SZ = 0x7F000000, 0x100000
HEAP_VA, HEAP_SZ = 0x10000000, 0x800000
SENTINEL = 0x0FFF0000
UC_CAP = 300_000
C_CAP = 150_000

def pattern(va, size):
    a = np.arange(va, va + size, dtype=np.uint64)
    return (((a * 0x9E3779B1) & 0xFFFFFFFF) >> 24).astype(np.uint8)

class Ctx(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32),
                ("c", ctypes.c_uint32), ("v", ctypes.c_uint32), ("q", ctypes.c_uint32),
                ("ge", ctypes.c_uint32), ("thumb", ctypes.c_uint32), ("fpscr", ctypes.c_uint32),
                ("s", ctypes.c_uint32 * 64), ("halted", ctypes.c_uint32),
                ("tpidruro", ctypes.c_uint32), ("tpidrurw", ctypes.c_uint32), ("pending_svc", ctypes.c_uint32),
                ("host", ctypes.c_void_p)]
assert Ctx.pending_svc.offset == 364 and Ctx.host.offset == 368

def build(subset, use_pt):
    so = os.path.join(subset, "functest.so")
    srcs = sorted(glob.glob(os.path.join(subset, "src", "*.c"))) + [
        os.path.join(subset, "recomp_export.c"), os.path.join(subset, "a32_host_runtime.c"),
        os.path.join(HERE, "funchost.c")]
    newest = max(os.path.getmtime(p) for p in srcs)
    if not os.path.exists(so) or os.path.getmtime(so) < newest:
        objs = []
        procs = []
        for s_ in srcs:
            o = os.path.join(subset, "obj_" + os.path.basename(s_) + ".o")
            objs.append(o)
            procs.append(subprocess.Popen(["gcc", "-O1", "-fPIC", "-ffp-contract=off", "-fno-strict-aliasing", "-w",
                                           "-I", subset, "-c", s_, "-o", o]))
            if len(procs) >= os.cpu_count():
                for p in procs: assert p.wait() == 0
                procs = []
        for p in procs: assert p.wait() == 0
        subprocess.run(["gcc", "-shared", "-o", so] + objs + ["-lm"], check=True)
    lib = ctypes.CDLL(so)
    lib.fh_init.argtypes = [ctypes.c_char_p] + [ctypes.c_uint32] * 5 + [ctypes.c_int]
    lib.fh_run.argtypes = [ctypes.POINTER(Ctx), ctypes.c_uint32, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)]
    lib.fh_diff_reset.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
    return lib

class UcSide:
    def __init__(self, image, base):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
        img_sz = (len(image) + 0xFFF) & ~0xFFF
        self.regions = []
        for va, data in ((base, np.frombuffer(image.ljust(img_sz, b"\0"), dtype=np.uint8).copy()),
                         (STACK_VA, pattern(STACK_VA, STACK_SZ)), (HEAP_VA, pattern(HEAP_VA, HEAP_SZ))):
            uc.mem_map(va, len(data))
            uc.mem_write(va, data.tobytes())
            self.regions.append((va, data))
        self.mapped_extra = set()
        self.outside = {}
        self.intr = False
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        ends = sorted((va, va + len(d)) for va, d in self.regions)
        gaps, prev = [], 0
        for lo, hi in ends:
            if lo > prev: gaps.append((prev, lo - 1))
            prev = hi
        gaps.append((prev, 0xFFFFFFFF))
        for lo, hi in gaps:
            uc.hook_add(UC_HOOK_MEM_WRITE, self._write, begin=lo, end=hi)
        uc.hook_add(UC_HOOK_INTR, self._intr)
        uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)

    def _unmapped(self, uc, access, addr, size, value, data):
        for page in {addr >> 12, (addr + max(size, 1) - 1) >> 12}:
            if page not in self.mapped_extra:
                uc.mem_map(page << 12, 0x1000)
                uc.mem_write(page << 12, pattern(page << 12, 0x1000).tobytes())
                self.mapped_extra.add(page)
        return True

    def _write(self, uc, access, addr, size, value, data):
        if len(self.outside) > 400_000:  # same limit as the C host's overlay
            self.overflow = True
            uc.emu_stop()
            return
        for k in range(size):
            self.outside[(addr + k) & 0xFFFFFFFF] = (value >> (8 * k)) & 0xFF

    def _intr(self, uc, intno, data):
        self.intr = True
        uc.emu_stop()

    def run(self, st, entry):
        uc = self.uc
        self.intr = False
        self.overflow = False
        uc.reg_write(A.UC_ARM_REG_CPSR, (st["n"] << 31) | (st["z"] << 30) | (st["c"] << 29) | (st["v"] << 28) | 0x10)
        for k in range(13): uc.reg_write(A.UC_ARM_REG_R0 + k, st["r"][k])
        uc.reg_write(A.UC_ARM_REG_SP, st["r"][13])
        uc.reg_write(A.UC_ARM_REG_LR, SENTINEL)
        uc.reg_write(A.UC_ARM_REG_FPSCR, 0)
        uc.reg_write(A.UC_ARM_REG_C13_C0_3, st["tls"])
        for k in range(32): uc.reg_write(A.UC_ARM_REG_D0 + k, st["d"][k])
        try:
            uc.emu_start(entry, SENTINEL, timeout=1_000_000, count=UC_CAP)
            err = None
        except UcError as e:
            err = str(e)
        pc = uc.reg_read(A.UC_ARM_REG_PC)
        out = {"r": [uc.reg_read(A.UC_ARM_REG_R0 + k) for k in range(13)] +
                    [uc.reg_read(A.UC_ARM_REG_SP), uc.reg_read(A.UC_ARM_REG_LR), pc]}
        cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
        out.update(n=cpsr >> 31, z=(cpsr >> 30) & 1, c=(cpsr >> 29) & 1, v=(cpsr >> 28) & 1, q=(cpsr >> 27) & 1,
                   ge=(cpsr >> 16) & 15, fpscr=uc.reg_read(A.UC_ARM_REG_FPSCR),
                   d=[uc.reg_read(A.UC_ARM_REG_D0 + k) for k in range(32)])
        # memory: diff regions against pristine (vectorised), then restore
        addr_parts, val_parts = [], []
        for va, pristine in self.regions:
            live = np.frombuffer(uc.mem_read(va, len(pristine)), dtype=np.uint8)
            idx = np.nonzero(live != pristine)[0]
            if len(idx):
                addr_parts.append(idx.astype(np.uint64) + va)
                val_parts.append(live[idx])
                for pg in np.unique(idx >> 12):
                    o = int(pg) << 12
                    uc.mem_write(va + o, pristine[o:o + 4096].tobytes())
        if self.outside:
            oa = np.fromiter(self.outside.keys(), dtype=np.uint64, count=len(self.outside))
            ov = np.fromiter(self.outside.values(), dtype=np.uint8, count=len(self.outside))
            addr_parts.append(oa); val_parts.append(ov)
            for pg in np.unique(oa >> 12):
                base_pg = int(pg) << 12
                uc.mem_write(base_pg, pattern(base_pg, 0x1000).tobytes())
        self.outside = {}
        if addr_parts:
            a_ = np.concatenate(addr_parts); v_ = np.concatenate(val_parts)
            order = np.argsort(a_, kind="stable")
            writes = (a_[order], v_[order])
        else:
            writes = (np.zeros(0, np.uint64), np.zeros(0, np.uint8))
        if self.overflow: status = "overlay full"
        elif err: status = "uc_error"
        elif self.intr: status = "svc/udf"
        elif pc == SENTINEL: status = "returned"
        else: status = "cap"
        return status, out, writes

def rand_args(rng):
    r = []
    for k in range(4):
        x = rng.random()
        if x < 0.6: r.append(HEAP_VA + (rng.randrange(0, 0x400000) & ~7))
        elif x < 0.9: r.append(rng.choice([0, 1, 2, 3, 4, 8, 16, 100, 0xFFFFFFFF]))
        else: r.append(rng.getrandbits(32))
    r += [rng.getrandbits(32) for _ in range(9)]
    r.append(STACK_VA + STACK_SZ - 0x1000)
    r.append(SENTINEL)
    d = [special(rng, bool(k % 2), d2b(rand_float(rng)) if k % 2 else (f2b(rand_float(rng)) | (f2b(rand_float(rng)) << 32)))
         for k in range(32)]
    return {"r": r, "n": 0, "z": 1, "c": 0, "v": 0, "d": d, "tls": HEAP_VA + 0x7F0000}

def compare(a, b):
    diffs = []
    for k in range(16):
        if a["r"][k] != b["r"][k]: diffs.append(f"r{k}: c={a['r'][k]:08x} uc={b['r'][k]:08x}")
    for f in ("n", "z", "c", "v", "q", "ge"):
        if a[f] != b[f]: diffs.append(f"{f}: c={a[f]} uc={b[f]}")
    if (a["fpscr"] >> 28) != (b["fpscr"] >> 28): diffs.append("fpscr.nzcv")
    for k in range(32):
        x, y = a["d"][k], b["d"][k]
        if x != y: diffs.append(f"d{k}: c={x:016x} uc={y:016x}")  # bit-exact
    (aa, av), (ba, bv) = a["writes"], b["writes"]
    if len(aa) != len(ba) or not np.array_equal(aa, ba) or not np.array_equal(av, bv):
        wa = dict(zip(aa.tolist(), av.tolist())); wb = dict(zip(ba.tolist(), bv.tolist()))
        bad = sorted(k for k in set(wa) | set(wb) if wa.get(k) != wb.get(k))
        diffs.append(f"mem ({len(bad)} bytes): " + ", ".join(f"{k:08x}: c={wa.get(k)} uc={wb.get(k)}" for k in bad[:4]))
    return diffs

def main():
    subset, base = sys.argv[1], int(sys.argv[2], 16)
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    use_pt = "--no-page-table" not in sys.argv
    lib = build(subset, use_pt)
    img_sz = lib.fh_init(os.path.join(subset, "image.bin").encode(), base, STACK_VA, STACK_SZ, HEAP_VA, HEAP_SZ,
                         1 if use_pt else 0)
    assert img_sz > 0
    image = open(os.path.join(subset, "image.bin"), "rb").read()
    ucs = UcSide(image, base)
    funcs = [int(l, 16) for l in open(os.path.join(subset, "functions.txt")) if l.strip()]
    for a in sys.argv:
        if a.startswith("--funcs="):
            lo, hi = (int(x) for x in a[8:].split(":"))
            funcs = funcs[lo:hi]
    rng = random.Random(seed)
    tally = collections.Counter()
    fails = []
    addrs = (ctypes.c_uint32 * 2_000_000)(); vals = (ctypes.c_uint8 * 2_000_000)()
    insns_total = 0
    for fo in funcs:
        for _ in range(per):
            st = rand_args(rng)
            ustatus, uo, uw = ucs.run(st, base + fo)
            c = Ctx()
            for k in range(15): c.r[k] = st["r"][k]
            c.r[15] = base + fo
            c.n, c.z, c.c, c.v = st["n"], st["z"], st["c"], st["v"]
            for k in range(32):
                c.s[2 * k] = st["d"][k] & 0xFFFFFFFF; c.s[2 * k + 1] = st["d"][k] >> 32
            c.tpidruro = st["tls"]
            blocks = ctypes.c_uint(0)
            cst = lib.fh_run(ctypes.byref(c), SENTINEL, C_CAP, ctypes.byref(blocks))
            n = lib.fh_diff_reset(addrs, vals, len(addrs))
            m_ = min(n, len(addrs))
            ca = np.ctypeslib.as_array(addrs)[:m_].astype(np.uint64); cv = np.ctypeslib.as_array(vals)[:m_].copy()
            order = np.argsort(ca, kind="stable")
            cw = (ca[order], cv[order])
            cstatus = ["returned", "miss", "unhandled", "svc", "cap", "thumb", "overlay full"][cst]
            if ustatus != "returned" or cstatus != "returned":
                key = f"inconclusive (uc {ustatus}, c {cstatus})"
                tally[key] += 1
                continue
            co = {"r": list(c.r), "n": c.n, "z": c.z, "c": c.c, "v": c.v, "q": c.q, "ge": c.ge, "fpscr": c.fpscr,
                  "d": [c.s[2 * k] | (c.s[2 * k + 1] << 32) for k in range(32)], "writes": cw}
            uo["writes"] = uw
            d = compare(co, uo)
            if d:
                tally["FAIL"] += 1
                fails.append((fo, d))
            else:
                tally["pass"] += 1
                insns_total += blocks.value
    print(f"functions {len(funcs)} x {per} cases ({'page table' if use_pt else 'callbacks only'})")
    for k, v in sorted(tally.items()): print(f"  {k}: {v}")
    conclusive = tally["pass"] + tally["FAIL"]
    if conclusive:
        print(f"  pass rate on completed runs: {100*tally['pass']/conclusive:.2f}%  ({insns_total} blocks executed in passing runs)")
    for fo, d in fails[:15]:
        print(f"  FAIL main+{fo:#x}: {'; '.join(d[:4])}")

if __name__ == "__main__":
    main()
