// a32_to_c.h - AArch32 (ARM state) + VFP -> C static recompiler.
//
// Companion to suyu's src/core/recompiler/arm64_to_c.h, which only understands
// AArch64. Same model: every guest basic block becomes one C function that
// operates on a context struct (A32Context, see a32_runtime.h), ends by setting
// r15 to the next block's address and returns to a dispatcher.
//
// Scope (milestone 1): the ARM instruction set as used by MHGU's main module
// (integer, load/store, block transfer, branches, media, multiplies) plus VFP.
// Thumb and Advanced SIMD (NEON) are not translated yet; such instructions end
// the block with a32_unhandled() so the host can resume on a JIT.
//
// Conventions used below:
//   * pc  = address of the instruction being translated.
//   * Reading r15 as an operand yields pc + 8 (ARM state).
//   * Condition codes wrap each instruction in `if (cond) { ... }`.
//   * An instruction that can write the PC is a block terminator.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace a32recomp {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;

inline std::string F(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

inline u32 Bits(u32 v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline u32 Bit(u32 v, int b) { return (v >> b) & 1; }

// Result of decoding one instruction.
struct Tr {
    std::string body;       // C statements (without the condition wrapper)
    bool handled = true;
    bool terminates = false; // body always ends in `return;`
};

inline const char* CondExpr(u32 cond) {
    static const char* t[16] = {
        "c->z",          "!c->z",           "c->c",           "!c->c",
        "c->n",          "!c->n",           "c->v",           "!c->v",
        "(c->c && !c->z)", "(!c->c || c->z)", "(c->n == c->v)", "(c->n != c->v)",
        "(!c->z && c->n == c->v)", "(c->z || c->n != c->v)", "1", "1"};
    return t[cond & 15];
}

// Guest addresses derived from the PC are module-relative at translation time:
// the loader picks each module's base at run time. Generated code wraps them in
// A32_PC(), which the generated project defines as (module base + offset).
inline std::string PcConst(u32 off) { return F("A32_PC(0x%08Xu)", off); }

// Register read. r15 reads as pc + 8.
inline std::string R(u32 n, u32 pc) {
    if (n == 15) return PcConst(pc + 8);
    return F("c->r[%u]", n);
}
// Word-aligned PC value used for literal addressing (Align(PC, 4)).
inline std::string RAlign(u32 n, u32 pc) {
    if (n == 15) return PcConst((pc + 8) & ~3u);
    return F("c->r[%u]", n);
}

// DecodeImmShift: type 0..3, imm5 -> (type, amount), with RRX as type 4.
inline void DecodeImmShift(u32 type, u32 imm5, u32& t, u32& amt) {
    t = type;
    amt = imm5;
    if (type == 1 || type == 2) {
        if (imm5 == 0) amt = 32;
    } else if (type == 3 && imm5 == 0) {
        t = 4;
        amt = 1;
    }
}

// ARMExpandImm_C
inline u32 ExpandImm(u32 imm12, bool& carry_known, u32& carry) {
    u32 v = imm12 & 0xFF, rot = 2 * (imm12 >> 8);
    u32 r = rot ? (v >> rot) | (v << (32 - rot)) : v;
    carry_known = rot != 0;
    carry = r >> 31;
    return r;
}

// Writing the PC from a computed value (ALU, load, BX): always interworking on ARMv7.
inline std::string WritePcAndReturn(const std::string& val) {
    return "a32_bx_write_pc(c, " + val + "); return;";
}

// ---------------------------------------------------------------------------
// Data processing (immediate, register, register-shifted register)
// ---------------------------------------------------------------------------
inline Tr DataProcessing(u32 i, u32 pc) {
    Tr t;
    const u32 opc = Bits(i, 24, 21), S = Bit(i, 20), Rn = Bits(i, 19, 16), Rd = Bits(i, 15, 12);
    const bool imm_form = Bit(i, 25);
    const bool logical = (opc == 0 || opc == 1 || opc == 8 || opc == 9 || opc == 12 || opc == 13 ||
                          opc == 14 || opc == 15);
    const bool test = (opc >= 8 && opc <= 11);
    if (test && !S) { t.handled = false; return t; } // misc/MSR space, decoded elsewhere
    if (S && Rd == 15 && !test) { t.handled = false; return t; } // exception return (SUBS pc, lr)

    std::string s = "{ uint32_t op2";
    std::string carry_update; // statement that sets c->c for logical S ops
    if (imm_form) {
        bool known;
        u32 carry;
        u32 v = ExpandImm(Bits(i, 11, 0), known, carry);
        s += F(" = 0x%08Xu;", v);
        if (logical && S && known) carry_update = F(" c->c = %u;", carry);
    } else {
        const u32 Rm = Bits(i, 3, 0), type = Bits(i, 6, 5);
        std::string amt;
        u32 st = type;
        if (Bit(i, 4)) { // register-shifted register
            const u32 Rs = Bits(i, 11, 8);
            if (Rs == 15 || Rm == 15 || Rn == 15 || Rd == 15) { t.handled = false; return t; }
            amt = F("(c->r[%u] & 0xFFu)", Rs);
        } else {
            u32 a;
            DecodeImmShift(type, Bits(i, 11, 7), st, a);
            amt = F("%uu", a);
        }
        if (logical && S) {
            s += F(", co; op2 = a32_shift_c(%s, %u, %s, c->c, &co);", R(Rm, pc).c_str(), st,
                   amt.c_str());
            carry_update = " c->c = co;";
        } else {
            s += F(" = a32_shift(%s, %u, %s, c->c);", R(Rm, pc).c_str(), st, amt.c_str());
        }
    }

    const std::string rn = R(Rn, pc);
    std::string res;
    switch (opc) {
    case 0: case 8: res = rn + " & op2"; break;
    case 1: case 9: res = rn + " ^ op2"; break;
    case 12: res = rn + " | op2"; break;
    case 13: res = "op2"; break;
    case 14: res = rn + " & ~op2"; break;
    case 15: res = "~op2"; break;
    case 2: case 10: res = F("a32_awc(c, %s, ~op2, 1, %u)", rn.c_str(), S); break;
    case 3: res = F("a32_awc(c, ~%s, op2, 1, %u)", rn.c_str(), S); break;
    case 4: case 11: res = F("a32_awc(c, %s, op2, 0, %u)", rn.c_str(), S); break;
    case 5: res = F("a32_awc(c, %s, op2, c->c, %u)", rn.c_str(), S); break;
    case 6: res = F("a32_awc(c, %s, ~op2, c->c, %u)", rn.c_str(), S); break;
    case 7: res = F("a32_awc(c, ~%s, op2, c->c, %u)", rn.c_str(), S); break;
    }
    s += " uint32_t res = " + res + ";";
    if (logical && S) s += " a32_nz(c, res);" + carry_update;
    if (!test) {
        if (Rd == 15) {
            s += " " + WritePcAndReturn("res");
            t.terminates = true;
        } else {
            s += F(" c->r[%u] = res;", Rd);
        }
    } else {
        s += " (void)res;";
    }
    s += " }";
    t.body = s;
    return t;
}

// MOVW / MOVT
inline Tr MoveWide(u32 i) {
    Tr t;
    const u32 Rd = Bits(i, 15, 12), imm = (Bits(i, 19, 16) << 12) | Bits(i, 11, 0);
    if (Rd == 15) { t.handled = false; return t; }
    if (Bit(i, 22)) t.body = F("c->r[%u] = (c->r[%u] & 0xFFFFu) | 0x%08Xu;", Rd, Rd, imm << 16);
    else t.body = F("c->r[%u] = 0x%08Xu;", Rd, imm);
    return t;
}

// ---------------------------------------------------------------------------
// Multiplies
// ---------------------------------------------------------------------------
inline Tr Multiply(u32 i) {
    Tr t;
    const u32 op = Bits(i, 23, 21), S = Bit(i, 20);
    const u32 A = Bits(i, 19, 16), B = Bits(i, 15, 12), Rm = Bits(i, 11, 8), Rn = Bits(i, 3, 0);
    if (A == 15 || Rm == 15 || Rn == 15) { t.handled = false; return t; }
    std::string s;
    switch (op) {
    case 0: // MUL Rd=A
        s = F("{ uint32_t r = c->r[%u] * c->r[%u]; c->r[%u] = r;%s }", Rn, Rm, A, S ? " a32_nz(c, r);" : "");
        break;
    case 1: // MLA Rd=A, Ra=B
        if (B == 15) { t.handled = false; return t; }
        s = F("{ uint32_t r = c->r[%u] * c->r[%u] + c->r[%u]; c->r[%u] = r;%s }", Rn, Rm, B, A,
              S ? " a32_nz(c, r);" : "");
        break;
    case 2: // UMAAL
        if (S || B == 15) { t.handled = false; return t; }
        s = F("{ uint64_t r = (uint64_t)c->r[%u] * c->r[%u] + c->r[%u] + c->r[%u]; c->r[%u] = (uint32_t)r; "
              "c->r[%u] = (uint32_t)(r >> 32); }",
              Rn, Rm, A, B, B, A);
        break;
    case 3: // MLS
        if (S || B == 15) { t.handled = false; return t; }
        s = F("c->r[%u] = c->r[%u] - c->r[%u] * c->r[%u];", A, B, Rn, Rm);
        break;
    case 4: case 5: case 6: case 7: { // UMULL UMLAL SMULL SMLAL: RdHi=A RdLo=B
        if (B == 15) { t.handled = false; return t; }
        const bool sgn = op >= 6, acc = op & 1;
        std::string prod = sgn ? F("(uint64_t)((int64_t)(int32_t)c->r[%u] * (int32_t)c->r[%u])", Rn, Rm)
                               : F("(uint64_t)c->r[%u] * c->r[%u]", Rn, Rm);
        s = "{ uint64_t r = " + prod;
        if (acc) s += F(" + (((uint64_t)c->r[%u] << 32) | c->r[%u])", A, B);
        s += F("; c->r[%u] = (uint32_t)r; c->r[%u] = (uint32_t)(r >> 32);%s }", B, A,
               S ? " a32_nz64(c, r);" : "");
        break;
    }
    }
    t.body = s;
    return t;
}

// SMLA<x><y>, SMLAW<y>, SMULW<y>, SMLAL<x><y>, SMUL<x><y>
inline Tr HalfwordMultiply(u32 i) {
    Tr t;
    const u32 op1 = Bits(i, 22, 21), A = Bits(i, 19, 16), B = Bits(i, 15, 12), Rm = Bits(i, 11, 8),
              Rn = Bits(i, 3, 0), x = Bit(i, 5), y = Bit(i, 6);
    if (A == 15 || Rm == 15 || Rn == 15 || (op1 != 3 && !(op1 == 1 && x) && B == 15)) { t.handled = false; return t; }
    auto half = [](u32 r, u32 top) {
        return top ? F("(int32_t)(int16_t)(c->r[%u] >> 16)", r) : F("(int32_t)(int16_t)c->r[%u]", r);
    };
    std::string s;
    switch (op1) {
    case 0: // SMLAxy Rd=A Ra=B
        s = F("{ int64_t r = (int64_t)(%s * %s) + (int32_t)c->r[%u]; if (r != (int32_t)r) c->q = 1; "
              "c->r[%u] = (uint32_t)r; }",
              half(Rn, x).c_str(), half(Rm, y).c_str(), B, A);
        break;
    case 1:
        if (x == 0) // SMLAWy
            s = F("{ int64_t r = (((int64_t)(int32_t)c->r[%u] * %s) >> 16) + (int32_t)c->r[%u]; "
                  "if (r != (int32_t)r) c->q = 1; c->r[%u] = (uint32_t)r; }",
                  Rn, half(Rm, y).c_str(), B, A);
        else // SMULWy
            s = F("c->r[%u] = (uint32_t)(((int64_t)(int32_t)c->r[%u] * %s) >> 16);", A, Rn,
                  half(Rm, y).c_str());
        break;
    case 2: // SMLALxy RdHi=A RdLo=B
        s = F("{ uint64_t r = (uint64_t)(int64_t)(%s * %s) + (((uint64_t)c->r[%u] << 32) | c->r[%u]); "
              "c->r[%u] = (uint32_t)r; c->r[%u] = (uint32_t)(r >> 32); }",
              half(Rn, x).c_str(), half(Rm, y).c_str(), A, B, B, A);
        break;
    case 3: // SMULxy
        s = F("c->r[%u] = (uint32_t)(%s * %s);", A, half(Rn, x).c_str(), half(Rm, y).c_str());
        break;
    }
    t.body = s;
    return t;
}

// ---------------------------------------------------------------------------
// Miscellaneous (MRS/MSR, BX, BLX, CLZ, QADD...)
// ---------------------------------------------------------------------------
inline Tr Misc(u32 i, u32 pc) {
    Tr t;
    const u32 op = Bits(i, 22, 21), op2 = Bits(i, 6, 4), Rm = Bits(i, 3, 0), Rd = Bits(i, 15, 12),
              Rn = Bits(i, 19, 16);
    switch (op2) {
    case 0:
        if ((op & 1) == 0) { // MRS
            if (Bit(i, 22) || Rd == 15) break; // SPSR
            t.body = F("c->r[%u] = a32_get_apsr(c);", Rd);
            return t;
        } else { // MSR (register), APSR only
            if (Bit(i, 22) || Bits(i, 17, 16)) break;
            const u32 mask = Bits(i, 19, 18);
            t.body = F("a32_set_apsr(c, %s, %u);", R(Rm, pc).c_str(), mask);
            return t;
        }
    case 1:
        if (op == 1) { // BX
            t.body = WritePcAndReturn(R(Rm, pc));
            t.terminates = true;
            return t;
        }
        if (op == 3) { // CLZ
            if (Rd == 15 || Rm == 15) break;
            t.body = F("c->r[%u] = a32_clz(c->r[%u]);", Rd, Rm);
            return t;
        }
        break;
    case 2:
        if (op == 1) { // BXJ behaves as BX without Jazelle
            t.body = WritePcAndReturn(R(Rm, pc));
            t.terminates = true;
            return t;
        }
        break;
    case 3:
        if (op == 1) { // BLX (register)
            if (Rm == 15) break;
            t.body = F("{ uint32_t tgt = c->r[%u]; c->r[14] = %s; %s }", Rm, PcConst(pc + 4).c_str(),
                       WritePcAndReturn("tgt").c_str());
            t.terminates = true;
            return t;
        }
        break;
    case 5: { // QADD / QSUB / QDADD / QDSUB
        if (Rd == 15 || Rn == 15 || Rm == 15) break;
        std::string m = F("(int64_t)(int32_t)c->r[%u]", Rm), n = F("(int64_t)(int32_t)c->r[%u]", Rn);
        if (op & 2) n = F("(int64_t)(int32_t)a32_ssat(c, 2 * %s, 32)", n.c_str());
        t.body = F("c->r[%u] = a32_ssat(c, %s %s %s, 32);", Rd, m.c_str(), (op & 1) ? "-" : "+", n.c_str());
        return t;
    }
    default:
        break;
    }
    t.handled = false;
    return t;
}

// ---------------------------------------------------------------------------
// Loads and stores
// ---------------------------------------------------------------------------

// Shared addressing: builds "uint32_t base, off, addr" and the writeback statement.
struct Addr {
    std::string setup; // declares base/off/addr
    std::string wb;    // writeback statement (may be empty)
};
inline Addr Addressing(u32 i, u32 pc, u32 Rn, const std::string& off, bool literal_align) {
    const u32 P = Bit(i, 24), U = Bit(i, 23), W = Bit(i, 21);
    Addr a;
    const std::string base = literal_align ? RAlign(Rn, pc) : R(Rn, pc);
    a.setup = "uint32_t base = " + base + "; uint32_t off = " + off + "; uint32_t oa = " +
              (U ? "base + off" : "base - off") + "; uint32_t addr = " + (P ? "oa" : "base") + ";";
    if (!P || W) a.wb = F(" c->r[%u] = oa;", Rn);
    return a;
}

inline Tr LoadStoreWordByte(u32 i, u32 pc) {
    Tr t;
    const u32 B = Bit(i, 22), L = Bit(i, 20), Rn = Bits(i, 19, 16), Rt = Bits(i, 15, 12);
    const u32 P = Bit(i, 24), W = Bit(i, 21);
    const bool wback = !P || W;
    if (wback && (Rn == 15 || Rn == Rt)) { t.handled = false; return t; }
    std::string off;
    if (!Bit(i, 25)) {
        off = F("0x%Xu", Bits(i, 11, 0));
    } else {
        const u32 Rm = Bits(i, 3, 0);
        if (Rm == 15) { t.handled = false; return t; }
        u32 st, amt;
        DecodeImmShift(Bits(i, 6, 5), Bits(i, 11, 7), st, amt);
        off = (st == 0 && amt == 0) ? F("c->r[%u]", Rm) : F("a32_shift(c->r[%u], %u, %uu, c->c)", Rm, st, amt);
    }
    Addr a = Addressing(i, pc, Rn, off, Rn == 15);
    std::string s = "{ " + a.setup;
    if (L) {
        s += B ? " uint32_t v = a32_read8(c, addr);" : " uint32_t v = a32_read32(c, addr);";
        s += a.wb;
        if (Rt == 15) {
            if (B) { t.handled = false; return t; }
            s += " " + WritePcAndReturn("v");
            t.terminates = true;
        } else {
            s += F(" c->r[%u] = v;", Rt);
        }
    } else {
        s += B ? F(" a32_write8(c, addr, (uint8_t)%s);", R(Rt, pc).c_str())
               : F(" a32_write32(c, addr, %s);", R(Rt, pc).c_str());
        s += a.wb;
    }
    s += " }";
    t.body = s;
    return t;
}

// STRH/LDRH/LDRD/LDRSB/STRD/LDRSH
inline Tr ExtraLoadStore(u32 i, u32 pc) {
    Tr t;
    const u32 op2 = Bits(i, 6, 5), L = Bit(i, 20), Rn = Bits(i, 19, 16), Rt = Bits(i, 15, 12);
    const u32 P = Bit(i, 24), W = Bit(i, 21);
    const bool wback = !P || W;
    std::string off;
    if (Bit(i, 22)) {
        off = F("0x%Xu", (Bits(i, 11, 8) << 4) | Bits(i, 3, 0));
    } else {
        const u32 Rm = Bits(i, 3, 0);
        if (Rm == 15) { t.handled = false; return t; }
        off = F("c->r[%u]", Rm);
    }
    const bool dual = (op2 == 2 && !L) || (op2 == 3 && !L);
    if (Rt == 15 || (dual && (Rt & 1 || Rt == 14))) { t.handled = false; return t; }
    if (wback && (Rn == 15 || Rn == Rt || (dual && Rn == Rt + 1))) { t.handled = false; return t; }
    Addr a = Addressing(i, pc, Rn, off, Rn == 15);
    std::string s = "{ " + a.setup;
    if (op2 == 1) {
        if (L) s += " uint32_t v = a32_read16(c, addr);" + a.wb + F(" c->r[%u] = v;", Rt);
        else s += F(" a32_write16(c, addr, (uint16_t)c->r[%u]);", Rt) + a.wb;
    } else if (op2 == 2) {
        if (L) s += " uint32_t v = (uint32_t)(int32_t)(int8_t)a32_read8(c, addr);" + a.wb + F(" c->r[%u] = v;", Rt);
        else s += " uint32_t v0 = a32_read32(c, addr), v1 = a32_read32(c, addr + 4);" + a.wb +
                  F(" c->r[%u] = v0; c->r[%u] = v1;", Rt, Rt + 1);
    } else {
        if (L) s += " uint32_t v = (uint32_t)(int32_t)(int16_t)a32_read16(c, addr);" + a.wb + F(" c->r[%u] = v;", Rt);
        else s += F(" a32_write32(c, addr, c->r[%u]); a32_write32(c, addr + 4, c->r[%u]);", Rt, Rt + 1) + a.wb;
    }
    s += " }";
    t.body = s;
    return t;
}

// LDREX/STREX family, ARMv8 LDAEX/STLEX and LDA/STL. Exclusive accesses go
// through the host's exclusive monitor (suyu's per-core monitor) so recompiled
// code and the JIT fallback contend correctly across cores.
inline Tr Exclusive(u32 i) {
    Tr t;
    if (Bits(i, 9, 8) == 0) { // LDA/LDAB/LDAH, STL/STLB/STLH
        const u32 op = Bits(i, 22, 21), L = Bit(i, 20), Rn = Bits(i, 19, 16);
        if (op == 1 || Rn == 15) { t.handled = false; return t; }
        static const char* rdf[4] = {"a32_read32", "", "a32_read8", "a32_read16"};
        static const char* wrf[4] = {"a32_write32", "", "a32_write8", "a32_write16"};
        static const char* cast[4] = {"", "", "(uint8_t)", "(uint16_t)"};
        if (L) {
            const u32 Rt = Bits(i, 15, 12);
            if (Rt == 15) { t.handled = false; return t; }
            t.body = F("c->r[%u] = %s(c, c->r[%u]);", Rt, rdf[op], Rn);
        } else {
            const u32 Rt = Bits(i, 3, 0);
            if (Rt == 15) { t.handled = false; return t; }
            t.body = F("%s(c, c->r[%u], %sc->r[%u]);", wrf[op], Rn, cast[op], Rt);
        }
        return t;
    }
    if (Bits(i, 9, 8) != 3 && Bits(i, 9, 8) != 2) { t.handled = false; return t; }
    const u32 op = Bits(i, 22, 21), L = Bit(i, 20), Rn = Bits(i, 19, 16), Rd = Bits(i, 15, 12), Rt = Bits(i, 3, 0);
    if (Rn == 15 || Rd == 15) { t.handled = false; return t; }
    static const unsigned size[4] = {4, 8, 1, 2};
    if (L) { // LDREX{,D,B,H} / LDAEX{,D,B,H}
        if (op == 1) {
            if (Rd & 1 || Rd == 14) { t.handled = false; return t; }
            t.body = F("{ uint64_t v = a32_excl_read(c, c->r[%u], 8); c->r[%u] = (uint32_t)v; c->r[%u] = (uint32_t)(v >> 32); }",
                       Rn, Rd, Rd + 1);
        } else {
            t.body = F("c->r[%u] = (uint32_t)a32_excl_read(c, c->r[%u], %u);", Rd, Rn, size[op]);
        }
        return t;
    }
    // STREX{,D,B,H} / STLEX{,D,B,H}: Rd receives 0 on success, 1 on failure
    if (Rt == 15 || Rd == Rn || Rd == Rt || (op == 1 && (Rt & 1 || Rt == 14 || Rd == Rt + 1))) {
        t.handled = false;
        return t;
    }
    const std::string val = op == 1 ? F("((uint64_t)c->r[%u] << 32) | c->r[%u]", Rt + 1, Rt) : F("c->r[%u]", Rt);
    t.body = F("c->r[%u] = a32_excl_write(c, c->r[%u], %u, %s);", Rd, Rn, size[op], val.c_str());
    return t;
}

// LDM/STM (including PUSH/POP)
inline Tr BlockTransfer(u32 i, u32 pc) {
    Tr t;
    const u32 P = Bit(i, 24), U = Bit(i, 23), S = Bit(i, 22), W = Bit(i, 21), L = Bit(i, 20);
    const u32 Rn = Bits(i, 19, 16), list = Bits(i, 15, 0);
    if (S || Rn == 15 || list == 0) { t.handled = false; return t; }
    u32 cnt = 0; // portable popcount (the lifter is also built with MSVC)
    for (u32 b = list; b; b &= b - 1) cnt++;
    std::string s = F("{ uint32_t base = c->r[%u]; uint32_t a = base", Rn);
    if (!U) s += F(" - %uu", 4 * cnt);
    if (P == U) s += " + 4u"; // IB or DB
    s += ";";
    const bool wb = W && !(L && (list >> Rn) & 1);
    const std::string wbs = wb ? F(" c->r[%u] = base %c %uu;", Rn, U ? '+' : '-', 4 * cnt) : "";
    if (L) {
        for (u32 r = 0; r < 15; ++r)
            if ((list >> r) & 1) s += F(" c->r[%u] = a32_read32(c, a); a += 4;", r);
        if ((list >> 15) & 1) {
            s += " uint32_t npc = a32_read32(c, a);" + wbs + " " + WritePcAndReturn("npc");
            t.terminates = true;
        } else {
            s += wbs;
        }
    } else {
        for (u32 r = 0; r < 16; ++r) {
            if (!((list >> r) & 1)) continue;
            if (r == Rn) s += " a32_write32(c, a, base); a += 4;";
            else s += F(" a32_write32(c, a, %s); a += 4;", R(r, pc).c_str());
        }
        s += wbs;
    }
    s += " }";
    t.body = s;
    return t;
}

// ---------------------------------------------------------------------------
// Media instructions (extend, reverse, saturate, bitfield, divide, SMMUL)
// ---------------------------------------------------------------------------
inline Tr Media(u32 i, u32 pc) {
    Tr t;
    t.handled = false;
    const u32 op1 = Bits(i, 24, 20), op2 = Bits(i, 7, 5);
    const u32 Rd = Bits(i, 15, 12), Rn = Bits(i, 19, 16), Rm = Bits(i, 3, 0), A = Bits(i, 15, 12);
    (void)pc;
    // Packing, unpacking, saturation, reversal: bits 27:23 = 01101
    if ((op1 >> 3) == 1) {
        const u32 o = op1 & 7;
        if (Rd == 15) return t;
        if ((o == 2 || o == 3) && (op2 & 1) == 0) { // SSAT
            const u32 sat = Bits(i, 20, 16) + 1, sh = Bit(i, 6);
            u32 st, amt;
            DecodeImmShift(sh << 1, Bits(i, 11, 7), st, amt);
            if (Rm == 15) return t;
            t.body = F("c->r[%u] = a32_ssat(c, (int64_t)(int32_t)a32_shift(c->r[%u], %u, %uu, 0), %u);", Rd, Rm, st, amt, sat);
            t.handled = true;
            return t;
        }
        if ((o == 6 || o == 7) && (op2 & 1) == 0) { // USAT
            const u32 sat = Bits(i, 20, 16), sh = Bit(i, 6);
            u32 st, amt;
            DecodeImmShift(sh << 1, Bits(i, 11, 7), st, amt);
            if (Rm == 15) return t;
            t.body = F("c->r[%u] = a32_usat(c, (int64_t)(int32_t)a32_shift(c->r[%u], %u, %uu, 0), %u);", Rd, Rm, st, amt, sat);
            t.handled = true;
            return t;
        }
        if (o == 0 && (op2 & 1) == 0) { // PKHBT / PKHTB
            if (Rn == 15 || Rm == 15) return t;
            const u32 tb = Bit(i, 6);
            u32 st, amt;
            DecodeImmShift(tb << 1, Bits(i, 11, 7), st, amt);
            std::string sh = F("a32_shift(c->r[%u], %u, %uu, 0)", Rm, st, amt);
            if (tb) t.body = F("c->r[%u] = (c->r[%u] & 0xFFFF0000u) | (%s & 0xFFFFu);", Rd, Rn, sh.c_str());
            else t.body = F("c->r[%u] = (%s & 0xFFFF0000u) | (c->r[%u] & 0xFFFFu);", Rd, sh.c_str(), Rn);
            t.handled = true;
            return t;
        }
        if (op2 == 3) { // extends: SXTAB16/SXTB16, SXTAB/SXTB, SXTAH/SXTH, UXT*
            if (Rm == 15) return t;
            const u32 rot = Bits(i, 11, 10) * 8;
            std::string v = rot ? F("a32_ror(c->r[%u], %u)", Rm, rot) : F("c->r[%u]", Rm);
            std::string ext;
            switch (o) {
            case 2: ext = F("(uint32_t)(int32_t)(int8_t)%s", v.c_str()); break;
            case 3: ext = F("(uint32_t)(int32_t)(int16_t)%s", v.c_str()); break;
            case 6: ext = F("(uint32_t)(uint8_t)%s", v.c_str()); break;
            case 7: ext = F("(uint32_t)(uint16_t)%s", v.c_str()); break;
            case 0: case 4: {
                const bool sg = o == 0;
                std::string lo = sg ? "(uint32_t)(uint16_t)(int16_t)(int8_t)x" : "(x & 0xFFu)";
                std::string hi = sg ? "((uint32_t)(uint16_t)(int16_t)(int8_t)(x >> 16) << 16)" : "(x & 0x00FF0000u)";
                if (Rn == 15)
                    t.body = F("{ uint32_t x = %s; c->r[%u] = %s | %s; }", v.c_str(), Rd, hi.c_str(), lo.c_str());
                else
                    t.body = F("{ uint32_t x = %s; uint32_t lo = (c->r[%u] + %s) & 0xFFFFu; "
                               "uint32_t hi = ((c->r[%u] >> 16) + (%s >> 16)) & 0xFFFFu; c->r[%u] = (hi << 16) | lo; }",
                               v.c_str(), Rn, lo.c_str(), Rn, hi.c_str(), Rd);
                t.handled = true;
                return t;
            }
            default: return t;
            }
            t.body = (Rn == 15) ? F("c->r[%u] = %s;", Rd, ext.c_str())
                                : F("c->r[%u] = c->r[%u] + %s;", Rd, Rn, ext.c_str());
            t.handled = true;
            return t;
        }
        if (Rm == 15) return t;
        if (o == 3 && op2 == 1) { t.body = F("c->r[%u] = a32_rev(c->r[%u]);", Rd, Rm); t.handled = true; return t; }
        if (o == 3 && op2 == 5) { t.body = F("c->r[%u] = a32_rev16(c->r[%u]);", Rd, Rm); t.handled = true; return t; }
        if (o == 7 && op2 == 1) { t.body = F("c->r[%u] = a32_rbit(c->r[%u]);", Rd, Rm); t.handled = true; return t; }
        if (o == 7 && op2 == 5) { t.body = F("c->r[%u] = a32_revsh(c->r[%u]);", Rd, Rm); t.handled = true; return t; }
        if (o == 0 && op2 == 5) { // SEL
            if (Rn == 15) return t;
            t.body = F("{ uint32_t r = 0; for (int k = 0; k < 4; k++) { uint32_t m = 0xFFu << (8 * k); "
                       "r |= ((c->ge >> k) & 1) ? (c->r[%u] & m) : (c->r[%u] & m); } c->r[%u] = r; }",
                       Rn, Rm, Rd);
            t.handled = true;
            return t;
        }
        return t;
    }
    // Signed multiplies / divide: bits 27:23 = 01110
    if ((op1 >> 3) == 2) {
        const u32 o = op1 & 7;
        const u32 D = Bits(i, 19, 16); // destination for these encodings
        if (D == 15 || Rm == 15 || Bits(i, 11, 8) == 15) return t;
        const u32 Rmm = Bits(i, 11, 8), Rnn = Bits(i, 3, 0);
        if (o == 1 && op2 == 0) { t.body = F("c->r[%u] = a32_sdiv(c->r[%u], c->r[%u]);", D, Rnn, Rmm); t.handled = true; return t; }
        if (o == 3 && op2 == 0) { t.body = F("c->r[%u] = a32_udiv(c->r[%u], c->r[%u]);", D, Rnn, Rmm); t.handled = true; return t; }
        if (o == 5) { // SMMUL/SMMLA (op2 00x), SMMLS (op2 11x)
            const u32 round = Bit(i, 5);
            const std::string prod = F("(int64_t)(int32_t)c->r[%u] * (int32_t)c->r[%u]", Rnn, Rmm);
            const std::string rnd = round ? " + 0x80000000LL" : "";
            if ((op2 >> 1) == 0) {
                if (A == 15) t.body = F("c->r[%u] = (uint32_t)((uint64_t)(%s%s) >> 32);", D, prod.c_str(), rnd.c_str());
                else t.body = F("c->r[%u] = (uint32_t)((((uint64_t)c->r[%u] << 32) + (uint64_t)(%s%s)) >> 32);", D, A, prod.c_str(), rnd.c_str());
                t.handled = true;
                return t;
            }
            if ((op2 >> 1) == 3 && A != 15) {
                t.body = F("c->r[%u] = (uint32_t)((((uint64_t)c->r[%u] << 32) - (uint64_t)(%s)%s) >> 32);", D, A, prod.c_str(), round ? " + 0x80000000ull" : "");
                t.handled = true;
                return t;
            }
        }
        if (o == 0 && op2 < 4) { // SMLAD/SMUAD (op2 00x), SMLSD/SMUSD (op2 01x)
            const u32 X = Bit(i, 5);
            std::string m = X ? F("a32_ror(c->r[%u], 16)", Rmm) : F("c->r[%u]", Rmm);
            std::string p1 = F("((int64_t)(int16_t)c->r[%u] * (int16_t)(%s))", Rnn, m.c_str());
            std::string p2 = F("((int64_t)(int16_t)(c->r[%u] >> 16) * (int16_t)((%s) >> 16))", Rnn, m.c_str());
            const char* op = (op2 >> 1) ? "-" : "+";
            std::string acc = (A == 15) ? "" : F(" + (int32_t)c->r[%u]", A);
            t.body = F("{ int64_t r = %s %s %s%s; if (r != (int32_t)r) c->q = 1; c->r[%u] = (uint32_t)r; }",
                       p1.c_str(), op, p2.c_str(), acc.c_str(), D);
            t.handled = true;
            return t;
        }
        return t;
    }
    // Bitfield / USAD8: bits 27:23 = 01111
    if ((op1 >> 3) == 3) {
        const u32 o = op1 & 7;
        const u32 lsb = Bits(i, 11, 7), hi5 = Bits(i, 20, 16);
        if (Rd == 15 && !(o == 0 && op2 == 0)) return t;
        if ((o >> 1) == 1 && (op2 & 3) == 2) { // SBFX
            if (Rm == 15 || lsb + hi5 > 31) return t;
            const u32 w = hi5 + 1;
            t.body = F("c->r[%u] = (uint32_t)((int32_t)(c->r[%u] << %u) >> %u);", Rd, Rm, 32 - lsb - w, 32 - w);
            t.handled = true;
            return t;
        }
        if ((o >> 1) == 3 && (op2 & 3) == 2) { // UBFX
            if (Rm == 15 || lsb + hi5 > 31) return t;
            const u32 w = hi5 + 1;
            const u32 mask = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1);
            t.body = F("c->r[%u] = (c->r[%u] >> %u) & 0x%08Xu;", Rd, Rm, lsb, mask);
            t.handled = true;
            return t;
        }
        if ((o >> 1) == 2 && (op2 & 3) == 0) { // BFC / BFI (msb = hi5)
            if (hi5 < lsb) return t;
            const u32 w = hi5 - lsb + 1;
            const u32 mask = (w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1)) << lsb;
            if (Rm == 15) t.body = F("c->r[%u] &= 0x%08Xu;", Rd, ~mask);
            else t.body = F("c->r[%u] = (c->r[%u] & 0x%08Xu) | ((c->r[%u] << %u) & 0x%08Xu);", Rd, Rd, ~mask, Rm, lsb, mask);
            t.handled = true;
            return t;
        }
        if (o == 0 && op2 == 0) { // USAD8 / USADA8 (Rd at 19:16, Ra at 15:12)
            const u32 D = Bits(i, 19, 16), Rmm = Bits(i, 11, 8), Rnn = Bits(i, 3, 0);
            if (D == 15 || Rmm == 15 || Rnn == 15) return t;
            std::string acc = (A == 15) ? "0" : F("c->r[%u]", A);
            t.body = F("{ uint32_t s = %s; for (int k = 0; k < 32; k += 8) { int d = (int)((c->r[%u] >> k) & 0xFF) - "
                       "(int)((c->r[%u] >> k) & 0xFF); s += (uint32_t)(d < 0 ? -d : d); } c->r[%u] = s; }",
                       acc.c_str(), Rnn, Rmm, D);
            t.handled = true;
            return t;
        }
        return t;
    }
    return t;
}

// ---------------------------------------------------------------------------
// VFP
// ---------------------------------------------------------------------------
inline u32 SReg(u32 v4, u32 b) { return (v4 << 1) | b; }
inline u32 DReg(u32 v4, u32 b) { return (b << 4) | v4; }

inline Tr VfpDataProcessing(u32 i) {
    Tr t;
    const u32 sz = Bit(i, 8), D = Bit(i, 22), N = Bit(i, 7), M = Bit(i, 5), op = Bit(i, 6);
    const u32 Vd = Bits(i, 15, 12), Vn = Bits(i, 19, 16), Vm = Bits(i, 3, 0);
    const u32 opc1 = (Bit(i, 23) << 2) | Bits(i, 21, 20);
    const u32 d = sz ? DReg(Vd, D) : SReg(Vd, D);
    const u32 n = sz ? DReg(Vn, N) : SReg(Vn, N);
    const u32 m = sz ? DReg(Vm, M) : SReg(Vm, M);
    const char* ty = sz ? "double" : "float";
    const char* G = sz ? "a32_gd" : "a32_gs";
    const char* S = sz ? "a32_sd" : "a32_ss";
    auto rd = [&](u32 r) { return F("%s(c, %u)", G, r); };
    auto wr = [&](u32 r, const std::string& e) { return F("%s(c, %u, (%s)(%s));", S, r, ty, e.c_str()); };
    const std::string Dv = rd(d), Nv = rd(n), Mv = rd(m);
    const std::string prod = "(" + std::string(ty) + ")(" + Nv + " * " + Mv + ")";
    t.handled = true;
    // Arithmetic goes through a32_<op>_{s,d}, which fix NaN results up to
    // ARM's rules (default NaN 0x7FC00000, signalling NaNs first). FPNeg is a
    // plain sign flip, NaNs included, which is what C's unary minus does.
    const char* X = sz ? "_d" : "_s";
    auto mul = [&] { return F("a32_mul%s(%s, %s)", X, Nv.c_str(), Mv.c_str()); };
    switch (opc1) {
    case 0: // VMLA: FPAdd(d, n*m) / VMLS: FPAdd(d, -(n*m))
        t.body = wr(d, F("a32_add%s(%s, %s%s)", X, Dv.c_str(), op ? "-" : "", mul().c_str()));
        return t;
    case 1: // VNMLS (op=0): FPAdd(-d, n*m) / VNMLA (op=1): FPAdd(-d, -(n*m))
        t.body = wr(d, F("a32_add%s(-%s, %s%s)", X, Dv.c_str(), op ? "-" : "", mul().c_str()));
        return t;
    case 2: // VMUL / VNMUL
        t.body = wr(d, (op ? "-" : "") + mul());
        return t;
    case 3: // VADD / VSUB
        t.body = wr(d, F("a32_%s%s(%s, %s)", op ? "sub" : "add", X, Nv.c_str(), Mv.c_str()));
        return t;
    case 4: // VDIV
        if (op) break;
        t.body = wr(d, F("a32_div%s(%s, %s)", X, Nv.c_str(), Mv.c_str()));
        return t;
    case 5: // VFNMS (op=0): FPMulAdd(-d, n, m) / VFNMA (op=1): FPMulAdd(-d, -n, m)
        t.body = wr(d, F("a32_fma%s(-%s, %s%s, %s)", X, Dv.c_str(), op ? "-" : "", Nv.c_str(), Mv.c_str()));
        return t;
    case 6: // VFMA (op=0): FPMulAdd(d, n, m) / VFMS (op=1): FPMulAdd(d, -n, m)
        t.body = wr(d, F("a32_fma%s(%s, %s%s, %s)", X, Dv.c_str(), op ? "-" : "", Nv.c_str(), Mv.c_str()));
        return t;
    case 7:
        break;
    }
    if (opc1 != 7) { t.handled = false; return t; }
    const u32 opc2 = Bits(i, 19, 16), opc3 = Bits(i, 7, 6);
    if ((opc3 & 1) == 0) { // VMOV immediate
        const u32 imm8 = (Bits(i, 19, 16) << 4) | Bits(i, 3, 0);
        const u32 b7 = imm8 >> 7, b6 = (imm8 >> 6) & 1, b54 = (imm8 >> 4) & 3, lo = imm8 & 15;
        if (sz) {
            u64 bits = ((u64)b7 << 63) | ((u64)(b6 ^ 1) << 62) | ((u64)(b6 ? 0xFF : 0) << 54) | ((u64)b54 << 52) |
                       ((u64)lo << 48);
            t.body = F("c->ext.d[%u] = 0x%016llXull;", d, (unsigned long long)bits);
        } else {
            u32 bits = (b7 << 31) | ((b6 ^ 1) << 30) | ((b6 ? 0x1Fu : 0) << 25) | (b54 << 23) | (lo << 19);
            t.body = F("c->ext.s[%u] = 0x%08Xu;", d, bits);
        }
        return t;
    }
    switch (opc2) {
    case 0:
        if (opc3 == 1) { t.body = sz ? F("c->ext.d[%u] = c->ext.d[%u];", d, m) : F("c->ext.s[%u] = c->ext.s[%u];", d, m); return t; }
        // VABS: clear the sign bit (exact, NaN-safe)
        t.body = sz ? F("c->ext.d[%u] = c->ext.d[%u] & 0x7FFFFFFFFFFFFFFFull;", d, m)
                    : F("c->ext.s[%u] = c->ext.s[%u] & 0x7FFFFFFFu;", d, m);
        return t;
    case 1:
        if (opc3 == 1) { // VNEG: flip the sign bit
            t.body = sz ? F("c->ext.d[%u] = c->ext.d[%u] ^ 0x8000000000000000ull;", d, m)
                        : F("c->ext.s[%u] = c->ext.s[%u] ^ 0x80000000u;", d, m);
            return t;
        }
        t.body = wr(d, F("a32_nan1%s(%s(%s), %s)", sz ? "_d" : "_s", sz ? "sqrt" : "sqrtf", Mv.c_str(), Mv.c_str()));
        return t;
    case 4: // VCMP(E)
        t.body = F("a32_fcmp(c, (double)%s, (double)%s);", Dv.c_str(), Mv.c_str());
        return t;
    case 5: // VCMP(E) with zero
        if (Bits(i, 5, 0) != 0) break;
        t.body = F("a32_fcmp(c, (double)%s, 0.0);", Dv.c_str());
        return t;
    case 6: // VRINTR (op=0, current rounding mode) / VRINTZ (op=1)
        t.body = wr(d, F("a32_nan1%s(%s(%s), %s)", sz ? "_d" : "_s",
                         Bit(i, 7) ? (sz ? "trunc" : "truncf") : (sz ? "nearbyint" : "nearbyintf"), Mv.c_str(), Mv.c_str()));
        return t;
    case 7: // VCVT between double and single, VRINTX
        if (opc3 == 1) {
            t.body = wr(d, F("a32_nan1%s(%s(%s), %s)", sz ? "_d" : "_s", sz ? "nearbyint" : "nearbyintf", Mv.c_str(), Mv.c_str()));
            return t;
        }
        if (opc3 != 3) break;
        if (sz) t.body = F("a32_ss(c, %u, (float)a32_gd(c, %u));", SReg(Vd, D), m);
        else t.body = F("a32_sd(c, %u, (double)a32_gs(c, %u));", DReg(Vd, D), m);
        return t;
    case 8: { // VCVT integer -> floating point; source is always an S register
        const u32 sm = SReg(Vm, M);
        const bool sgn = Bit(i, 7);
        std::string src = sgn ? F("(int32_t)c->ext.s[%u]", sm) : F("c->ext.s[%u]", sm);
        t.body = wr(d, src);
        return t;
    }
    case 12: case 13: { // VCVT(R) floating point -> integer; dest is always an S register
        const u32 sd = SReg(Vd, D);
        const bool sgn = opc2 & 1, rz = Bit(i, 7);
        t.body = F("c->ext.s[%u] = %s((double)%s, %u);", sd, sgn ? "a32_f2s" : "a32_f2u", Mv.c_str(), rz);
        return t;
    }
    default:
        break;
    }
    t.handled = false;
    return t;
}

// Extension register load/store and 64-bit core<->extension transfers.
inline Tr VfpLoadStore(u32 i, u32 pc) {
    Tr t;
    const u32 sz = Bit(i, 8), P = Bit(i, 24), U = Bit(i, 23), D = Bit(i, 22), W = Bit(i, 21), L = Bit(i, 20);
    const u32 Rn = Bits(i, 19, 16), Vd = Bits(i, 15, 12), imm8 = Bits(i, 7, 0);
    // 64-bit transfers: bits 24:21 = 0010
    if (Bits(i, 24, 21) == 2) {
        const u32 Rt2 = Bits(i, 19, 16), Rt = Bits(i, 15, 12), M = Bit(i, 5), Vm = Bits(i, 3, 0);
        if (Rt == 15 || Rt2 == 15 || (L && Rt == Rt2)) { t.handled = false; return t; }
        if (sz) {
            const u32 dm = DReg(Vm, M);
            t.body = L ? F("c->r[%u] = (uint32_t)c->ext.d[%u]; c->r[%u] = (uint32_t)(c->ext.d[%u] >> 32);", Rt, dm, Rt2, dm)
                       : F("c->ext.d[%u] = ((uint64_t)c->r[%u] << 32) | c->r[%u];", dm, Rt2, Rt);
        } else {
            const u32 sm = SReg(Vm, M);
            if (sm == 31) { t.handled = false; return t; }
            t.body = L ? F("c->r[%u] = c->ext.s[%u]; c->r[%u] = c->ext.s[%u];", Rt, sm, Rt2, sm + 1)
                       : F("c->ext.s[%u] = c->r[%u]; c->ext.s[%u] = c->r[%u];", sm, Rt, sm + 1, Rt2);
        }
        return t;
    }
    const u32 first = sz ? DReg(Vd, D) : SReg(Vd, D);
    if (P && !W) { // VLDR / VSTR
        std::string base = RAlign(Rn, pc);
        std::string addr = F("%s %c 0x%Xu", base.c_str(), U ? '+' : '-', imm8 * 4);
        if (sz) t.body = L ? F("c->ext.d[%u] = a32_read64(c, %s);", first, addr.c_str())
                           : F("a32_write64(c, %s, c->ext.d[%u]);", addr.c_str(), first);
        else t.body = L ? F("c->ext.s[%u] = a32_read32(c, %s);", first, addr.c_str())
                        : F("a32_write32(c, %s, c->ext.s[%u]);", addr.c_str(), first);
        return t;
    }
    // VLDM / VSTM / VPUSH / VPOP
    if (P == U || Rn == 15) { t.handled = false; return t; }
    if (sz && (imm8 & 1)) { t.handled = false; return t; } // FLDMX/FSTMX
    const u32 regs = sz ? imm8 / 2 : imm8;
    if (regs == 0 || first + regs > (sz ? 32u : 32u)) { t.handled = false; return t; }
    std::string s = F("{ uint32_t base = c->r[%u]; uint32_t a = %s;", Rn,
                      U ? "base" : F("base - 0x%Xu", imm8 * 4).c_str());
    for (u32 k = 0; k < regs; ++k) {
        if (sz) s += L ? F(" c->ext.d[%u] = a32_read64(c, a); a += 8;", first + k)
                       : F(" a32_write64(c, a, c->ext.d[%u]); a += 8;", first + k);
        else s += L ? F(" c->ext.s[%u] = a32_read32(c, a); a += 4;", first + k)
                    : F(" a32_write32(c, a, c->ext.s[%u]); a += 4;", first + k);
    }
    if (W) s += F(" c->r[%u] = base %c 0x%Xu;", Rn, U ? '+' : '-', imm8 * 4);
    s += " }";
    t.body = s;
    return t;
}

// VMOV core<->single, VMRS, VMSR
inline Tr VfpTransfer(u32 i) {
    Tr t;
    const u32 A = Bits(i, 23, 21), L = Bit(i, 20), Rt = Bits(i, 15, 12), C = Bit(i, 8);
    if (C == 0 && A == 0 && Bits(i, 6, 5) == 0 && Bits(i, 3, 0) == 0) {
        const u32 sn = SReg(Bits(i, 19, 16), Bit(i, 7));
        if (Rt == 15) { t.handled = false; return t; }
        t.body = L ? F("c->r[%u] = c->ext.s[%u];", Rt, sn) : F("c->ext.s[%u] = c->r[%u];", sn, Rt);
        return t;
    }
    if (C == 0 && A == 7 && Bits(i, 19, 16) == 1) { // FPSCR
        if (L) t.body = (Rt == 15) ? "a32_vmrs_nzcv(c);" : F("c->r[%u] = c->fpscr;", Rt);
        else t.body = (Rt == 15) ? "" : F("c->fpscr = c->r[%u];", Rt);
        if (!L && Rt == 15) t.handled = false;
        return t;
    }
    t.handled = false;
    return t;
}

// ---------------------------------------------------------------------------
// ARMv8 AArch32 floating point (unconditional encodings)
// ---------------------------------------------------------------------------
inline Tr VfpV8(u32 i) {
    Tr t;
    const u32 sz = Bit(i, 8), D = Bit(i, 22), N = Bit(i, 7), M = Bit(i, 5);
    const u32 Vd = Bits(i, 15, 12), Vn = Bits(i, 19, 16), Vm = Bits(i, 3, 0);
    const u32 d = sz ? DReg(Vd, D) : SReg(Vd, D);
    const u32 n = sz ? DReg(Vn, N) : SReg(Vn, N);
    const u32 m = sz ? DReg(Vm, M) : SReg(Vm, M);
    const char* ty = sz ? "double" : "float";
    const char* G = sz ? "a32_gd" : "a32_gs";
    const char* S = sz ? "a32_sd" : "a32_ss";
    if ((i & 0xFF800E50u) == 0xFE000A00u) { // VSEL<cc>: EQ, VS, GE, GT
        static const char* cc[4] = {"c->z", "c->v", "(c->n == c->v)", "(!c->z && c->n == c->v)"};
        t.body = sz ? F("c->ext.d[%u] = %s ? c->ext.d[%u] : c->ext.d[%u];", d, cc[Bits(i, 21, 20)], n, m)
                    : F("c->ext.s[%u] = %s ? c->ext.s[%u] : c->ext.s[%u];", d, cc[Bits(i, 21, 20)], n, m);
        return t;
    }
    if ((i & 0xFFB00E10u) == 0xFE800A00u) { // VMAXNM / VMINNM
        t.body = F("%s(c, %u, %s%s(%s(c, %u), %s(c, %u)));", S, d, Bit(i, 6) ? "a32_minnm" : "a32_maxnm", sz ? "" : "_s", G, n, G, m);
        return t;
    }
    if ((i & 0xFFBC0ED0u) == 0xFEB80A40u) { // VRINT{A,N,P,M}
        // libm's floor/ceil pass a signalling NaN through; ARM quiets it.
        t.body = F("%s(c, %u, a32_nan1%s((%s)a32_round(%s(c, %u), %u), %s(c, %u)));", S, d, sz ? "_d" : "_s", ty, G, m,
                   Bits(i, 17, 16), G, m);
        return t;
    }
    if ((i & 0xFFBC0E50u) == 0xFEBC0A40u) { // VCVT{A,N,P,M} to integer (dest is an S register)
        const u32 sd = SReg(Vd, D);
        t.body = F("c->ext.s[%u] = %s(a32_round(%s(c, %u), %u), 1);", sd, Bit(i, 7) ? "a32_f2s" : "a32_f2u", G, m, Bits(i, 17, 16));
        return t;
    }
    t.handled = false;
    return t;
}

// ---------------------------------------------------------------------------
// CP15 accesses user code makes: TLS registers, legacy barriers, CNTFRQ
// ---------------------------------------------------------------------------
inline Tr Cp15(u32 i) {
    Tr t;
    const u32 Rt = Bits(i, 15, 12), L = Bit(i, 20);
    const u32 key = i & 0x00EF00EFu; // opc1, CRn, coproc, opc2, CRm (drops L and Rt)
    if (key == 0x000D0060u || key == 0x000D0040u) { // TPIDRURO (opc2 3) / TPIDRURW (opc2 2), c13 c0
        const char* reg = Bits(i, 7, 5) == 3 ? "tpidruro" : "tpidrurw";
        if (Rt == 15) { t.handled = false; return t; }
        if (L) t.body = F("c->r[%u] = c->%s;", Rt, reg);
        else if (Bits(i, 7, 5) == 2) t.body = F("c->tpidrurw = c->r[%u];", Rt);
        else t.handled = false;
        return t;
    }
    if (!L && (key == 0x000700AAu || key == 0x0007008Au || key == 0x00070085u)) { t.body = "/* cp15 barrier */"; return t; }
    if (L && key == 0x000E0000u && Rt != 15) { t.body = F("c->r[%u] = 19200000u; /* CNTFRQ */", Rt); return t; }
    t.handled = false;
    return t;
}

// ---------------------------------------------------------------------------
// Advanced SIMD (NEON): the handful of forms MHGU's modules use
// ---------------------------------------------------------------------------
inline u64 AdvSimdExpandImm(u32 op, u32 cmode, u32 imm8, bool& ok) {
    ok = true;
    const u64 v = imm8;
    auto rep2 = [](u64 x) { return x | (x << 32); };
    auto rep4 = [](u64 x) { return x | (x << 16) | (x << 32) | (x << 48); };
    switch (cmode >> 1) {
    case 0: return rep2(v);
    case 1: return rep2(v << 8);
    case 2: return rep2(v << 16);
    case 3: return rep2(v << 24);
    case 4: return rep4(v);
    case 5: return rep4(v << 8);
    case 6: return (cmode & 1) ? rep2((v << 16) | 0xFFFF) : rep2((v << 8) | 0xFF);
    default:
        if (!(cmode & 1) && !op) { u64 r = 0; for (int k = 0; k < 8; k++) r |= v << (8 * k); return r; }
        if (!(cmode & 1) && op) { u64 r = 0; for (int k = 0; k < 8; k++) if ((imm8 >> k) & 1) r |= 0xFFull << (8 * k); return r; }
        if ((cmode & 1) && !op) {
            const u32 b7 = imm8 >> 7, b6 = (imm8 >> 6) & 1;
            const u32 f = (b7 << 31) | ((b6 ^ 1) << 30) | ((b6 ? 0x1Fu : 0) << 25) | ((imm8 & 0x3F) << 19);
            return rep2(f);
        }
        ok = false;
        return 0;
    }
}

// ---- helpers for the element-wise forms below. Operands are copied into local arrays first
// (_n, _m, _d: up to four D registers each) and results gathered in _r, so destinations may
// overlap sources.
namespace neon {
inline unsigned long long Mask(u32 es) { return es >= 64 ? ~0ull : (1ull << es) - 1; }
inline std::string Lane(const char* arr, u32 e, u32 es) {
    const u32 bit = e * es;
    return F("((%s[%u] >> %u) & 0x%llXull)", arr, bit / 64, bit % 64, Mask(es));
}
inline std::string SetLane(const char* arr, u32 e, u32 es, const std::string& v) {
    const u32 bit = e * es;
    return F("%s[%u] = (%s[%u] & ~(0x%llXull << %u)) | (((uint64_t)(%s) & 0x%llXull) << %u); ", arr, bit / 64, arr,
             bit / 64, Mask(es), bit % 64, v.c_str(), Mask(es), bit % 64);
}
// value of an es-bit lane as int64_t (sign-extended) or uint64_t
inline std::string Ext(const std::string& x, u32 es, bool is_signed) {
    if (!is_signed) return "(uint64_t)" + x;
    if (es == 64) return "(int64_t)" + x;
    return F("((int64_t)((%s) << %u) >> %u)", x.c_str(), 64 - es, 64 - es);
}
inline std::string Load(const char* arr, u32 reg, u32 cnt) {
    return F("uint64_t %s[4] = {0, 0, 0, 0}; memcpy(%s, &c->ext.d[%u], %u); ", arr, arr, reg, 8 * cnt);
}
inline std::string Store(const char* arr, u32 reg, u32 cnt) { return F("memcpy(&c->ext.d[%u], %s, %u); ", reg, arr, 8 * cnt); }
// saturate the int64 expression v (already computed in "v") to es bits, setting FPSCR.QC
inline std::string Sat(u32 es, bool to_signed) {
    if (to_signed) {
        const long long hi = (long long)((1ull << (es - 1)) - 1), lo = -hi - 1;
        return F("if (v > %lldLL) { v = %lldLL; c->fpscr |= 0x08000000u; } else if (v < %lldLL) { v = %lldLL; c->fpscr |= "
                 "0x08000000u; } ",
                 hi, hi, lo, lo);
    }
    const long long hi = (long long)((1ull << es) - 1);
    return F("if (v > %lldLL) { v = %lldLL; c->fpscr |= 0x08000000u; } else if (v < 0) { v = 0; c->fpscr |= 0x08000000u; } ",
             hi, hi);
}
} // namespace neon

// Advanced SIMD forms added for the SDK (crypto, structure loads/stores, widening/narrowing,
// float arithmetic). Returns false when i is none of them.
inline bool NeonExtra(u32 i, Tr& t) {
    using namespace neon;
    const u32 D = Bit(i, 22), N = Bit(i, 7), M = Bit(i, 5);
    const u32 d = DReg(Bits(i, 15, 12), D), n = DReg(Bits(i, 19, 16), N), m = DReg(Bits(i, 3, 0), M);
    const u32 U = Bit(i, 24), A = Bits(i, 11, 8), sz = Bits(i, 21, 20);
    const auto fail = [&] { t.handled = false; return true; };

    // ---- crypto
    if ((i & 0xFFBF0F10u) == 0xF3B00300u) { // AESE / AESD / AESMC / AESIMC
        if ((d | m) & 1) return fail();
        t.body = F("a32_aes(c, %u, %u, %u);", d, m, Bits(i, 7, 6));
        return true;
    }
    if ((i & 0xFFBF0FD0u) == 0xF3B902C0u || (i & 0xFFBF0F90u) == 0xF3BA0380u) { // SHA1H / SHA1SU1, SHA256SU0
        if ((d | m) & 1) return fail();
        const u32 op = (i & 0x000F0000u) == 0x00090000u ? 0 : Bit(i, 6) ? 2 : 1;
        t.body = F("a32_sha2(c, %u, %u, %u);", d, m, op);
        return true;
    }
    if ((i & 0xFE800F50u) == 0xF2000C40u) { // SHA1C/P/M/SU0, SHA256H/H2/SU1
        if (((d | n | m) & 1) || (U && sz == 3)) return fail();
        t.body = F("a32_sha3(c, %u, %u, %u, %u);", d, n, m, U ? 4 + sz : sz);
        return true;
    }

    // ---- two registers, miscellaneous
    if ((i & 0xFFB30F10u) == 0xF3B20200u) { // VMOVN, VQMOVUN, VQMOVN.S, VQMOVN.U (Qm -> Dd)
        const u32 size = Bits(i, 19, 18), op = Bits(i, 7, 6), es = 8u << size;
        if (size == 3 || (m & 1)) return fail();
        std::string s = "{ " + Load("_m", m, 2) + "uint64_t _r[4] = {0, 0, 0, 0}; ";
        for (u32 e = 0; e < 64 / es; ++e) {
            const std::string x = Lane("_m", e, 2 * es);
            if (op == 0) { s += SetLane("_r", e, es, x); continue; }
            if (op == 3) {
                s += F("{ uint64_t uv = %s; if (uv > 0x%llXull) { uv = 0x%llXull; c->fpscr |= 0x08000000u; } ", x.c_str(), Mask(es), Mask(es)) +
                     SetLane("_r", e, es, "uv") + "} ";
                continue;
            }
            s += "{ int64_t v = " + Ext(x, 2 * es, true) + "; " + Sat(es, op == 2) + SetLane("_r", e, es, "v") + "} ";
        }
        t.body = s + Store("_r", d, 1) + "}";
        return true;
    }
    if ((i & 0xFFB30810u) == 0xF3B10000u && Bits(i, 9, 7) <= 4) { // VCGT/VCGE/VCEQ/VCLE/VCLT #0
        const u32 size = Bits(i, 19, 18), op = Bits(i, 9, 7), Qb = Bit(i, 6), regs = Qb ? 2 : 1, es = 8u << size;
        const bool flt = Bit(i, 10);
        if (size == 3 || (flt && size != 2) || (Qb && ((d | m) & 1))) return fail();
        std::string s = "{ " + Load("_m", m, regs) + "uint64_t _r[4] = {0, 0, 0, 0}; ";
        static const char* rel[5] = {">", ">=", "==", "<=", "<"};
        for (u32 e = 0; e < regs * 64 / es; ++e) {
            const std::string x = Lane("_m", e, es);
            std::string v;
            if (flt) {
                static const char* call[5] = {"a32_nfcmp((uint32_t)%s, 0, 2)", "a32_nfcmp((uint32_t)%s, 0, 1)",
                                              "a32_nfcmp((uint32_t)%s, 0, 0)", "a32_nfcmp(0, (uint32_t)%s, 1)",
                                              "a32_nfcmp(0, (uint32_t)%s, 2)"};
                v = F(call[op], x.c_str());
            } else {
                v = F("(%s %s 0 ? 0x%llXull : 0)", Ext(x, es, true).c_str(), rel[op], Mask(es));
            }
            s += SetLane("_r", e, es, v);
        }
        t.body = s + Store("_r", d, regs) + "}";
        return true;
    }
    if ((i & 0xFFB30E10u) == 0xF3B20000u) { // VSWP / VTRN / VUZP / VZIP
        const u32 op = Bits(i, 8, 7), size = Bits(i, 19, 18), Qb = Bit(i, 6), regs = Qb ? 2 : 1, es = 8u << size;
        if ((Qb && ((d | m) & 1)) || (op && size == 3) || (op >= 2 && !Qb && size == 2) || d == m) return fail();
        const u32 cnt = regs * 64 / es;
        std::string s = "{ " + Load("_d", d, regs) + Load("_m", m, regs) + "uint64_t _rd[4], _rm[4]; ";
        if (op == 0) {
            s += F("memcpy(_rd, _m, %u); memcpy(_rm, _d, %u); ", 8 * regs, 8 * regs);
        } else {
            s += F("memcpy(_rd, _d, %u); memcpy(_rm, _m, %u); ", 8 * regs, 8 * regs);
            // element k of the concatenation d:m
            const auto cat = [&](u32 k) { return k < cnt ? Lane("_d", k, es) : Lane("_m", k - cnt, es); };
            for (u32 e = 0; e < cnt; ++e) {
                if (op == 1) { // VTRN: swap Dd[2k+1] with Dm[2k]
                    if (e & 1) s += SetLane("_rd", e, es, Lane("_m", e - 1, es));
                    else s += SetLane("_rm", e, es, Lane("_d", e + 1, es));
                } else if (op == 2) { // VUZP: even elements to Dd, odd to Dm
                    s += SetLane("_rd", e, es, cat(2 * e)) + SetLane("_rm", e, es, cat(2 * e + 1));
                } else { // VZIP: interleave
                    const u32 lo = 2 * e, hi = 2 * e + 1;
                    const auto zip = [&](u32 k) { return (k & 1) ? Lane("_m", k / 2, es) : Lane("_d", k / 2, es); };
                    if (lo < cnt) s += SetLane("_rd", lo, es, zip(lo)) + SetLane("_rd", hi, es, zip(hi));
                    else s += SetLane("_rm", lo - cnt, es, zip(lo)) + SetLane("_rm", hi - cnt, es, zip(hi));
                }
            }
        }
        t.body = s + Store("_rd", d, regs) + Store("_rm", m, regs) + "}";
        return true;
    }
    if ((i & 0xFFB30F10u) == 0xF3B00500u) { // VCNT.8 (bit 7 = 0) / VMVN (bit 7 = 1)
        const u32 Qb = Bit(i, 6), regs = Qb ? 2 : 1;
        if (Bits(i, 19, 18) != 0 || (Qb && ((d | m) & 1))) return fail();
        std::string s = "{ " + Load("_m", m, regs);
        for (u32 k = 0; k < regs; ++k) {
            if (Bit(i, 7)) {
                s += F("c->ext.d[%u] = ~_m[%u]; ", d + k, k);
            } else {
                s += F("{ uint64_t x = _m[%u]; x = x - ((x >> 1) & 0x5555555555555555ull); x = (x & 0x3333333333333333ull) + "
                       "((x >> 2) & 0x3333333333333333ull); c->ext.d[%u] = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full; } ",
                       k, d + k);
            }
        }
        t.body = s + "}";
        return true;
    }
    if ((i & 0xFFB30F10u) == 0xF3B10700u) { // VABS.F32 / VNEG.F32: sign bit only
        const u32 Qb = Bit(i, 6), regs = Qb ? 2 : 1;
        if (Bits(i, 19, 18) != 2 || (Qb && ((d | m) & 1))) return fail();
        std::string s;
        for (u32 k = 0; k < regs; ++k)
            s += F("c->ext.d[%u] = c->ext.d[%u] %s; ", d + k, m + k, Bit(i, 7) ? "^ 0x8000000080000000ull" : "& 0x7FFFFFFF7FFFFFFFull");
        t.body = s;
        return true;
    }
    if ((i & 0xFFB30E10u) == 0xF3B30400u) { // VRECPE / VRSQRTE
        const u32 Qb = Bit(i, 6), regs = Qb ? 2 : 1;
        if (!Bit(i, 8) || Bits(i, 19, 18) != 2 || (Qb && ((d | m) & 1))) return fail(); // unsigned forms: JIT
        std::string s = "{ uint32_t _r[4]; ";
        for (u32 k = 0; k < 2 * regs; ++k) s += F("_r[%u] = %s(c->ext.s[%u]); ", k, Bit(i, 7) ? "a32_nfrsqrte" : "a32_nfrecpe", 2 * m + k);
        for (u32 k = 0; k < 2 * regs; ++k) s += F("c->ext.s[%u] = _r[%u]; ", 2 * d + k, k);
        t.body = s + "}";
        return true;
    }
    if ((i & 0xFFB00C10u) == 0xF3B00800u) { // VTBL / VTBX
        const u32 len = Bits(i, 9, 8) + 1, tbx = Bit(i, 6);
        if (n + len > 32) return fail();
        std::string s = F("{ uint8_t _t[32], _ix[8], _o[8]; memcpy(_t, &c->ext.d[%u], %u); memcpy(_ix, &c->ext.d[%u], 8); "
                          "memcpy(_o, &c->ext.d[%u], 8); for (int k = 0; k < 8; k++) _o[k] = _ix[k] < %u ? _t[_ix[k]] : %s; "
                          "memcpy(&c->ext.d[%u], _o, 8); }",
                          n, 8 * len, m, d, 8 * len, tbx ? "_o[k]" : "0", d);
        t.body = s;
        return true;
    }

    // ---- three registers of the same length (the forms the base Neon() lacks)
    if ((i & 0xFE800000u) == 0xF2000000u) {
        const u32 Bb = Bit(i, 4), Qb = Bit(i, 6), regs = Qb ? 2 : 1, es = 8u << sz;
        if (Qb && ((d | n | m) & 1)) return fail();
        const auto lanes3 = [&](const std::function<std::string(u32)>& body) {
            std::string s = "{ " + Load("_n", n, regs) + Load("_m", m, regs) + Load("_d", d, regs) +
                            "uint64_t _r[4] = {0, 0, 0, 0}; ";
            for (u32 e = 0; e < regs * 64 / es; ++e) s += body(e);
            return s + Store("_r", d, regs) + "}";
        };
        if (A == 6 && sz != 3) { // VMAX / VMIN (integer)
            t.body = lanes3([&](u32 e) {
                const std::string x = Ext(Lane("_n", e, es), es, !U), y = Ext(Lane("_m", e, es), es, !U);
                return SetLane("_r", e, es, "(" + x + (Bb ? " < " : " > ") + y + " ? " + x + " : " + y + ")");
            });
            return true;
        }
        if ((A == 0 || A == 2) && Bb && sz == 3) { // VQADD / VQSUB, 64-bit lanes
            t.body = lanes3([&](u32 e) {
                const std::string x = Lane("_n", e, 64), y = Lane("_m", e, 64);
                std::string b = "{ const uint64_t x = " + x + ", y = " + y + "; uint64_t r = x " + (A ? "-" : "+") + " y; ";
                if (U) b += A ? "if (x < y) { r = 0; c->fpscr |= 0x08000000u; } " : "if (r < x) { r = ~0ull; c->fpscr |= 0x08000000u; } ";
                else b += std::string("if ((int64_t)(") + (A ? "(x ^ y) & (x ^ r)" : "(x ^ r) & (y ^ r)") +
                          ") < 0) { r = (int64_t)x < 0 ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull; c->fpscr |= 0x08000000u; } ";
                return b + SetLane("_r", e, 64, "r") + "} ";
            });
            return true;
        }
        if ((A == 0 || A == 2) && Bb && sz != 3) { // VQADD / VQSUB
            t.body = lanes3([&](u32 e) {
                return "{ int64_t v = " + Ext(Lane("_n", e, es), es, !U) + (A ? " - " : " + ") + Ext(Lane("_m", e, es), es, !U) +
                       "; " + Sat(es, !U) + SetLane("_r", e, es, "v") + "} ";
            });
            return true;
        }
        if (A == 9 && !Bb && sz != 3) { // VMLA / VMLS (integer)
            t.body = lanes3([&](u32 e) {
                return SetLane("_r", e, es, Lane("_d", e, es) + (U ? " - " : " + ") + Lane("_n", e, es) + " * " + Lane("_m", e, es));
            });
            return true;
        }
        if (A == 11 && Bb && !U && sz != 3) { // VPADD (integer)
            if (Qb) return fail();
            const u32 half = 32 / es;
            t.body = lanes3([&](u32 e) {
                const char* src = e < half ? "_n" : "_m";
                const u32 k = (e % half) * 2;
                return SetLane("_r", e, es, Lane(src, k, es) + " + " + Lane(src, k + 1, es));
            });
            return true;
        }
        if (A >= 13 && !Bit(i, 20)) { // float: sz bit 20 must be 0 (F32); bit 21 selects the op
            const u32 op = Bit(i, 21);
            const auto words = [&](const std::function<std::string(u32)>& f) {
                std::string s = "{ uint32_t _r[4]; ";
                for (u32 k = 0; k < 2 * regs; ++k) s += F("_r[%u] = ", k) + f(k) + "; ";
                for (u32 k = 0; k < 2 * regs; ++k) s += F("c->ext.s[%u] = _r[%u]; ", 2 * d + k, k);
                return s + "}";
            };
            const auto S = [](u32 reg, u32 k) { return F("c->ext.s[%u]", 2 * reg + k); };
            if (A == 13 && !Bb && !U) { // VADD / VSUB
                t.body = words([&](u32 k) { return F("%s(%s, %s)", op ? "a32_nfsub" : "a32_nfadd", S(n, k).c_str(), S(m, k).c_str()); });
                return true;
            }
            if (A == 13 && !Bb && U) {
                if (op) { // VABD
                    t.body = words([&](u32 k) { return F("a32_nfabd(%s, %s)", S(n, k).c_str(), S(m, k).c_str()); });
                    return true;
                }
                if (Qb) return fail(); // VPADD
                t.body = words([&](u32 k) {
                    const u32 src = k == 0 ? n : m;
                    return F("a32_nfadd(%s, %s)", S(src, 0).c_str(), S(src, 1).c_str());
                });
                return true;
            }
            if (A == 13 && Bb && !U) { // VMLA / VMLS
                t.body = words([&](u32 k) {
                    return F("a32_nfmla(%s, %s, %s, %u)", S(d, k).c_str(), S(n, k).c_str(), S(m, k).c_str(), op);
                });
                return true;
            }
            if (A == 13 && Bb && U && !op) { // VMUL
                t.body = words([&](u32 k) { return F("a32_nfmul(%s, %s)", S(n, k).c_str(), S(m, k).c_str()); });
                return true;
            }
            if (A == 14 && !(Bb == 0 && U == 0 && op)) { // VCEQ / VCGE / VCGT / VACGE / VACGT
                if (Bb && !U) return fail();
                const u32 cmp = !U ? 0 : !Bb ? 1 + op : 3 + op;
                t.body = words([&](u32 k) { return F("a32_nfcmp(%s, %s, %u)", S(n, k).c_str(), S(m, k).c_str(), cmp); });
                return true;
            }
            if (A == 15 && Bb && !U) { // VRECPS / VRSQRTS
                t.body = words([&](u32 k) { return F("a32_nfstep(%s, %s, %u)", S(n, k).c_str(), S(m, k).c_str(), op); });
                return true;
            }
        }
        return false;
    }

    // ---- three registers of different lengths: VADDL/W, VSUBL/W, VMLAL, VMLSL, VMULL
    if ((i & 0xFE800050u) == 0xF2800000u && sz != 3) {
        const bool wide = A == 1 || A == 3;
        if (!(A <= 3 || A == 8 || A == 10 || A == 12) || (d & 1) || (wide && (n & 1))) return false;
        const u32 es = 8u << sz;
        std::string s = "{ " + Load("_n", n, wide ? 2 : 1) + Load("_m", m, 1) + Load("_r", d, 2);
        for (u32 e = 0; e < 64 / es; ++e) {
            const std::string a = wide ? Lane("_n", e, 2 * es) : Ext(Lane("_n", e, es), es, !U);
            const std::string b = Ext(Lane("_m", e, es), es, !U);
            std::string v;
            if (A <= 3) v = "(uint64_t)" + a + ((A & 2) ? " - (uint64_t)" : " + (uint64_t)") + b;
            else {
                const std::string p = U ? "(" + a + " * " + b + ")" : "(uint64_t)(" + a + " * " + b + ")";
                v = A == 12 ? p : Lane("_r", e, 2 * es) + (A == 10 ? " - " : " + ") + p;
            }
            s += SetLane("_r", e, 2 * es, v);
        }
        t.body = s + Store("_r", d, 2) + "}";
        return true;
    }

    // ---- two registers and a scalar: VMLA/VMLS/VMUL (int and float), VMLAL/VMLSL/VMULL
    if ((i & 0xFE800050u) == 0xF2800040u && sz != 3 && sz != 0) {
        const bool lng = A == 2 || A == 6 || A == 10;
        const bool flt = A == 1 || A == 5 || A == 9;
        if (!(lng || flt || A == 0 || A == 4) || (flt && sz != 2)) return false;
        const u32 es = 8u << sz;
        const u32 vm = sz == 1 ? Bits(i, 2, 0) : Bits(i, 3, 0), idx = sz == 1 ? (M << 1) | Bit(i, 3) : M;
        const u32 regs = lng ? 2 : (U ? 2 : 1);
        if ((lng && (d & 1)) || (!lng && regs == 2 && ((d | n) & 1))) return fail();
        const std::string sc = F("((c->ext.d[%u] >> %u) & 0x%llXull)", vm, idx * es, Mask(es));
        if (flt) {
            std::string s = F("{ const uint32_t S = (uint32_t)%s; uint32_t _r[4]; ", sc.c_str());
            for (u32 k = 0; k < 2 * regs; ++k) {
                const std::string nk = F("c->ext.s[%u]", 2 * n + k), dk = F("c->ext.s[%u]", 2 * d + k);
                s += F("_r[%u] = ", k) +
                     (A == 9 ? F("a32_nfmul(%s, S)", nk.c_str()) : F("a32_nfmla(%s, %s, S, %u)", dk.c_str(), nk.c_str(), A == 5)) + "; ";
            }
            for (u32 k = 0; k < 2 * regs; ++k) s += F("c->ext.s[%u] = _r[%u]; ", 2 * d + k, k);
            t.body = s + "}";
            return true;
        }
        if (lng) {
            std::string s = "{ " + Load("_n", n, 1) + Load("_r", d, 2) + F("const uint64_t S = %s; ", sc.c_str());
            for (u32 e = 0; e < 64 / es; ++e) {
                const std::string a = Ext(Lane("_n", e, es), es, !U), b = Ext("S", es, !U);
                const std::string p = U ? "(" + a + " * " + b + ")" : "(uint64_t)(" + a + " * " + b + ")";
                s += SetLane("_r", e, 2 * es, A == 10 ? p : Lane("_r", e, 2 * es) + (A == 6 ? " - " : " + ") + p);
            }
            t.body = s + Store("_r", d, 2) + "}";
            return true;
        }
        std::string s = "{ " + Load("_n", n, regs) + Load("_r", d, regs) + F("const uint64_t S = %s; ", sc.c_str());
        for (u32 e = 0; e < regs * 64 / es; ++e)
            s += SetLane("_r", e, es, Lane("_r", e, es) + (A == 4 ? " - " : " + ") + Lane("_n", e, es) + " * S");
        t.body = s + Store("_r", d, regs) + "}";
        return true;
    }

    // ---- two registers and a shift amount (right shifts, narrowing, widening)
    if ((i & 0xFE800010u) == 0xF2800010u && !(Bits(i, 21, 19) == 0 && !Bit(i, 7))) {
        const u32 L = Bit(i, 7), imm6 = Bits(i, 21, 16), Qb = Bit(i, 6);
        const u32 es = L ? 64 : imm6 >= 32 ? 32 : imm6 >= 16 ? 16 : 8;
        // x >> s for an es-bit lane value v (int64 if signed), optionally rounded
        const auto shr = [](const std::string& v, u32 s, bool round) {
            if (s >= 64) return std::string("0");
            if (!round) return F("(%s >> %u)", v.c_str(), s);
            return F("((%s >> %u) + ((%s >> %u) & 1))", v.c_str(), s, v.c_str(), s - 1);
        };
        if (A <= 3) { // VSHR / VSRA / VRSHR / VRSRA
            const u32 regs = Qb ? 2 : 1, s = L ? 64 - imm6 : 2 * es - imm6;
            if (Qb && ((d | m) & 1)) return fail();
            std::string str = "{ " + Load("_m", m, regs) + Load("_r", d, regs);
            for (u32 e = 0; e < regs * 64 / es; ++e) {
                std::string v = Ext(Lane("_m", e, es), es, !U);
                // a signed shift by the full width keeps the sign: shift by width-1 instead (and round)
                std::string r;
                if (!U && s == es) r = (A & 2) ? "0" : F("(%s >> %u)", v.c_str(), es - 1);
                else if (U && s == es && es == 64) r = (A & 2) ? F("(%s >> 63)", v.c_str()) : "0";
                else r = shr(v, s, A & 2);
                str += SetLane("_r", e, es, (A & 1) ? Lane("_r", e, es) + " + (uint64_t)" + r : "(uint64_t)" + r);
            }
            t.body = str + Store("_r", d, regs) + "}";
            return true;
        }
        if ((A == 8 || A == 9) && !L) { // VSHRN/VRSHRN, VQSHRUN/VQRSHRUN, VQSHRN/VQRSHRN
            if (m & 1 || es == 64) return fail();
            const u32 s = 2 * es - imm6;
            const bool src_signed = A == 8 ? (U != 0) : !U; // VQSHRUN: signed source
            const bool sat = !(A == 8 && !U), dst_signed = A == 9 && !U;
            std::string str = "{ " + Load("_m", m, 2) + "uint64_t _r[4] = {0, 0, 0, 0}; ";
            for (u32 e = 0; e < 64 / es; ++e) {
                const std::string v = Ext(Lane("_m", e, 2 * es), 2 * es, src_signed);
                std::string body = F("{ int64_t v = (int64_t)%s; ", shr(v, s, Qb).c_str());
                if (sat) {
                    if (src_signed || dst_signed) body += Sat(es, dst_signed);
                    else body = F("{ uint64_t uv = %s; int64_t v; if (uv > 0x%llXull) { v = (int64_t)0x%llXull; c->fpscr |= 0x08000000u; } else v = (int64_t)uv; ",
                                  shr(v, s, Qb).c_str(), Mask(es), Mask(es));
                }
                str += body + SetLane("_r", e, es, "v") + "} ";
            }
            t.body = str + Store("_r", d, 1) + "}";
            return true;
        }
        if (A == 10 && !Qb && !L && es < 64) { // VSHLL / VMOVL
            if (d & 1) return fail();
            const u32 s = imm6 - es;
            std::string str = "{ " + Load("_m", m, 1) + "uint64_t _r[4] = {0, 0, 0, 0}; ";
            for (u32 e = 0; e < 64 / es; ++e)
                str += SetLane("_r", e, 2 * es, F("((uint64_t)%s << %u)", Ext(Lane("_m", e, es), es, !U).c_str(), s));
            t.body = str + Store("_r", d, 2) + "}";
            return true;
        }
        return false;
    }

    // ---- VREV64 / VREV32 / VREV16
    if ((i & 0xFFB30E10u) == 0xF3B00000u && Bits(i, 8, 7) < 3) {
        const u32 es = 8u << Bits(i, 19, 18), group = 64u >> Bits(i, 8, 7), Qb = Bit(i, 6), regs = Qb ? 2 : 1;
        if (es >= group || (Qb && ((d | m) & 1))) return fail();
        const u32 per = group / es;
        std::string s = "{ " + Load("_m", m, regs) + "uint64_t _r[4] = {0, 0, 0, 0}; ";
        for (u32 e = 0; e < regs * 64 / es; ++e) {
            const u32 src = (e / per) * per + (per - 1 - e % per);
            s += SetLane("_r", e, es, Lane("_m", src, es));
        }
        t.body = s + Store("_r", d, regs) + "}";
        return true;
    }

    // ---- element and structure loads/stores the base Neon() lacks
    if ((i & 0xFF100000u) == 0xF4000000u) {
        const u32 Ls = Bit(i, 21), Rn = Bits(i, 19, 16), Rm = Bits(i, 3, 0);
        if (Rn == 15) return false;
        static const char* rdf[4] = {"a32_read8", "a32_read16", "a32_read32", "a32_read64"};
        static const char* wrf[4] = {"a32_write8", "a32_write16", "a32_write32", "a32_write64"};
        static const char* cst[4] = {"(uint8_t)", "(uint16_t)", "(uint32_t)", "(uint64_t)"};
        const auto lane_rw = [&](u32 reg, u32 e, u32 size, u32 off) {
            const u32 es = 8u << size;
            if (Ls) return F("{ uint64_t _v = %s(c, a + %u); ", rdf[size], off) + SetLane("c->ext.d", reg * 64 / es + e, es, "_v") + "} ";
            return F("%s(c, a + %u, %s(c->ext.d[%u] >> %u)); ", wrf[size], off, cst[size], reg, e * es);
        };
        const auto writeback = [&](u32 bytes) {
            if (Rm == 13) return F("c->r[%u] = a + %u; ", Rn, bytes);
            if (Rm != 15) return F("c->r[%u] = a + c->r[%u]; ", Rn, Rm);
            return std::string();
        };
        if (!Bit(i, 23)) { // multiple n-element structures (VLD1/VST1 of 1-4 registers stay in Neon())
            const u32 type = Bits(i, 11, 8), size = Bits(i, 7, 6);
            u32 nst, inc, regs;
            switch (type) {
            case 0: nst = 4, inc = 1, regs = 1; break;
            case 1: nst = 4, inc = 2, regs = 1; break;
            case 3: nst = 2, inc = 2, regs = 2; break;
            case 4: nst = 3, inc = 1, regs = 1; break;
            case 5: nst = 3, inc = 2, regs = 1; break;
            case 8: nst = 2, inc = 1, regs = 1; break;
            case 9: nst = 2, inc = 2, regs = 1; break;
            default: return false;
            }
            if (size == 3 || d + (nst - 1) * inc + regs - 1 >= 32) return fail();
            const u32 es = 8u << size, eb = es / 8;
            std::string s = F("{ uint32_t a = c->r[%u]; ", Rn);
            u32 off = 0;
            for (u32 r = 0; r < regs; ++r)
                for (u32 e = 0; e < 64 / es; ++e)
                    for (u32 k = 0; k < nst; ++k, off += eb) s += lane_rw(d + r + k * inc, e, size, off);
            t.body = s + writeback(off) + "}";
            return true;
        }
        const u32 nst = Bits(i, 9, 8) + 1;
        if (Bits(i, 11, 10) == 3) { // VLDn to all lanes
            const u32 T = Bit(i, 5);
            u32 size = Bits(i, 7, 6);
            if (size == 3 && nst == 4) size = 2; // VLD4.32 with 128-bit alignment
            if (!Ls || size == 3) return fail();
            const u32 es = 8u << size, eb = es / 8, inc = nst == 1 ? 1 : T + 1, copies = nst == 1 ? T + 1 : 1;
            if (d + (nst - 1) * inc + copies - 1 >= 32) return fail();
            static const unsigned long long rep[3] = {0x0101010101010101ull, 0x0001000100010001ull, 0x0000000100000001ull};
            std::string s = F("{ uint32_t a = c->r[%u]; ", Rn);
            for (u32 k = 0; k < nst; ++k) {
                s += F("{ const uint64_t _v = (uint64_t)%s(c, a + %u) * 0x%llXull; ", rdf[size], k * eb, rep[size]);
                for (u32 cpy = 0; cpy < copies; ++cpy) s += F("c->ext.d[%u] = _v; ", d + k * inc + cpy);
                s += "} ";
            }
            t.body = s + writeback(nst * eb) + "}";
            return true;
        }
        if (nst == 1) return false; // single lane VLD1/VST1: Neon()
        const u32 size = Bits(i, 11, 10), ia = Bits(i, 7, 4);
        const u32 idx = size == 0 ? ia >> 1 : size == 1 ? ia >> 2 : ia >> 3;
        const u32 inc = size == 0 ? 1 : size == 1 ? ((ia & 2) ? 2 : 1) : ((ia & 4) ? 2 : 1);
        const u32 eb = 1u << size;
        if (d + (nst - 1) * inc >= 32) return fail();
        std::string s = F("{ uint32_t a = c->r[%u]; ", Rn);
        for (u32 k = 0; k < nst; ++k) s += lane_rw(d + k * inc, idx, size, k * eb);
        t.body = s + writeback(nst * eb) + "}";
        return true;
    }
    return false;
}

inline Tr Neon(u32 i) {
    Tr t;
    if (NeonExtra(i, t)) return t;
    t = Tr{};
    const u32 D = Bit(i, 22), N = Bit(i, 7), M = Bit(i, 5), Q = Bit(i, 6);
    const u32 d = DReg(Bits(i, 15, 12), D), n = DReg(Bits(i, 19, 16), N), m = DReg(Bits(i, 3, 0), M);
    // VMUL (integer, by scalar): Q is bit 24 here and Vm/M encode a scalar register + lane
    if ((i & 0xFE800F50u) == 0xF2800840u && Bits(i, 21, 20) != 0 && Bits(i, 21, 20) != 3) {
        const u32 sz = Bits(i, 21, 20), es = 8u << sz, sregs = Bit(i, 24) ? 2 : 1;
        const u32 vm = sz == 1 ? Bits(i, 2, 0) : Bits(i, 3, 0);
        const u32 lane = sz == 1 ? (M << 1) | Bit(i, 3) : M;
        if (sregs == 2 && ((d | n) & 1)) { t.handled = false; return t; }
        const unsigned long long mask = (1ull << es) - 1;
        std::string s = F("{ uint64_t _r[2]; const uint64_t S = (c->ext.d[%u] >> %u) & 0x%llXull; for (int k = 0; "
                          "k < %u; ++k) { const uint64_t a = c->ext.d[%u + k]; uint64_t r = 0; for (int s = 0; s < 64; "
                          "s += %u) r |= ((((a >> s) & 0x%llXull) * S) & 0x%llXull) << s; _r[k] = r; } ",
                          vm, lane * es, mask, sregs, n, es, mask, mask);
        for (u32 k = 0; k < sregs; ++k) s += F("c->ext.d[%u] = _r[%u]; ", d + k, k);
        t.body = s + "}";
        return t;
    }
    const u32 regs = Q ? 2 : 1;
    const bool load_store = Bits(i, 27, 24) == 4;
    const bool mod_imm = (i & 0xFEB80090u) == 0xF2800010u;
    // two-register misc and VDUP (scalar) have no Vn; bits 19:16 hold size/opcode/index there
    const bool no_n = (i & 0xFFB00010u) == 0xF3B00000u;
    // two registers and a shift amount: bits 21:16 hold the immediate, not Vn
    const bool shift_imm = !mod_imm && (i & 0xFE800010u) == 0xF2800010u;
    const u32 qregs = mod_imm ? d : (no_n || shift_imm) ? (Bits(i, 11, 8) == 12 && no_n ? d : (d | m)) : (d | n | m);
    if (!load_store && Q && (qregs & 1)) { t.handled = false; return t; }
    // Integer lanes: for each 64-bit half, apply `expr` to every esize-bit lane. x/y are the zero-
    // extended lanes of Dn/Dm, sx/sy the sign-extended ones, M the lane mask. Results go through
    // temporaries so Qd may overlap Qn/Qm.
    auto lanes = [&](u32 esize, u32 src_n, const char* expr) {
        const unsigned long long mask = esize == 64 ? ~0ull : (1ull << esize) - 1;
        std::string s = F("{ uint64_t _r[2]; for (int k = 0; k < %u; ++k) { uint64_t a = c->ext.d[%u + k], "
                          "b = c->ext.d[%u + k], r = 0; for (int s = 0; s < 64; s += %u) { const uint64_t M = "
                          "0x%llXull, x = (a >> s) & M, y = (b >> s) & M; const int64_t sx = (int64_t)(x << %u) "
                          ">> %u, sy = (int64_t)(y << %u) >> %u; (void)y; (void)sx; (void)sy; r |= ((uint64_t)(%s) "
                          "& M) << s; } _r[k] = r; } ",
                          regs, src_n, m, esize, mask, 64 - esize, 64 - esize, 64 - esize, 64 - esize, expr);
        for (u32 k = 0; k < regs; ++k) s += F("c->ext.d[%u] = _r[%u]; ", d + k, k);
        return s + "}";
    };
    // VABS/VNEG (integer, two-register misc)
    if ((i & 0xFFB30F10u) == 0xF3B10300u) {
        const u32 size = Bits(i, 19, 18);
        if (size == 3) { t.handled = false; return t; }
        t.body = lanes(8u << size, m, Bit(i, 7) ? "-x" : "sx < 0 ? -x : x");
        return t;
    }
    // VCGT/VCGE (integer register), VCEQ, VTST (three registers same length)
    if ((i & 0xFE800E00u) == 0xF2000200u || (i & 0xFE800F10u) == 0xF2000810u) {
        const u32 U = Bit(i, 24), size = Bits(i, 21, 20), ge = Bit(i, 4);
        if (size == 3 || (Bits(i, 11, 8) == 2)) { t.handled = false; return t; } // 0010 = VQSUB
        const char* expr;
        if (Bits(i, 11, 8) == 8) expr = U ? "x == y ? M : 0" : "(x & y) ? M : 0";
        else if (U) expr = ge ? "x >= y ? M : 0" : "x > y ? M : 0";
        else expr = ge ? "sx >= sy ? M : 0" : "sx > sy ? M : 0";
        t.body = lanes(8u << size, n, expr);
        return t;
    }
    // VADD/VSUB (integer, three registers same length)
    if ((i & 0xFE800F10u) == 0xF2000800u) {
        const u32 size = Bits(i, 21, 20);
        t.body = lanes(8u << size, n, Bit(i, 24) ? "x - y" : "x + y");
        return t;
    }
    // VMUL (integer, three registers same length; U=1 is the polynomial form)
    if ((i & 0xFF800F10u) == 0xF2000910u && Bits(i, 21, 20) != 3) {
        t.body = lanes(8u << Bits(i, 21, 20), n, "x * y");
        return t;
    }
    // VSHL (register): shifts each Dm lane by the signed low byte of the matching Dn lane;
    // negative shifts go right (arithmetic for .S), shifts past the lane width give 0 or the sign
    if ((i & 0xFE800F10u) == 0xF2000400u) {
        const u32 size = Bits(i, 21, 20), es = 8u << size;
        const std::string expr =
            Bit(i, 24)
                ? F("(int8_t)x >= 0 ? ((int8_t)x >= %u ? 0 : y << (int8_t)x) : (-(int8_t)x >= %u ? 0 : y >> -(int8_t)x)", es, es)
                : F("(int8_t)x >= 0 ? ((int8_t)x >= %u ? 0 : y << (int8_t)x) : (uint64_t)(sy >> (-(int8_t)x >= %u ? %u : -(int8_t)x))",
                    es, es, es - 1);
        t.body = lanes(es, n, expr.c_str());
        return t;
    }
    // VSHL (immediate): L:imm6 gives the lane size (highest set bit) and the shift
    if (shift_imm && (i & 0xFF800F10u) == 0xF2800510u) {
        const u32 imm6 = Bits(i, 21, 16);
        u32 es;
        if (Bit(i, 7)) es = 64;
        else if (imm6 & 0x20) es = 32;
        else if (imm6 & 0x10) es = 16;
        else es = 8;
        const u32 shift = Bit(i, 7) ? imm6 : imm6 - es;
        t.body = lanes(es, m, F("y << %u", shift).c_str());
        return t;
    }
    // one-register modified immediate: VMOV/VMVN/VORR/VBIC
    if (mod_imm) {
        const u32 op = Bit(i, 5), cmode = Bits(i, 11, 8);
        const u32 imm8 = (Bit(i, 24) << 7) | (Bits(i, 18, 16) << 4) | Bits(i, 3, 0);
        bool ok;
        u64 imm = AdvSimdExpandImm(op, cmode, imm8, ok);
        if (!ok) { t.handled = false; return t; }
        std::string s;
        const bool orr_bic = (cmode & 1) && cmode < 12;
        for (u32 k = 0; k < regs; ++k) {
            if (orr_bic) s += op ? F("c->ext.d[%u] &= 0x%016llXull; ", d + k, (unsigned long long)~imm)
                                 : F("c->ext.d[%u] |= 0x%016llXull; ", d + k, (unsigned long long)imm);
            else if (op && cmode != 14) s += F("c->ext.d[%u] = 0x%016llXull; ", d + k, (unsigned long long)~imm);
            else s += F("c->ext.d[%u] = 0x%016llXull; ", d + k, (unsigned long long)imm);
        }
        t.body = s;
        return t;
    }
    // VAND/VBIC/VORR/VORN/VEOR register (bitwise, whole D regs)
    if ((i & 0xFE800F10u) == 0xF2000110u) {
        const u32 U = Bit(i, 24), sz = Bits(i, 21, 20);
        const char* ops[2][4] = {{"%s & %s", "%s & ~%s", "%s | %s", "%s | ~%s"}, {"%s ^ %s", "", "", ""}};
        if (U && sz != 0) { // VBSL / VBIT / VBIF: bitwise select between two of Dd, Dn, Dm
            const char* sel[4] = {"", "(D & N) | (~D & Mm)", "(N & Mm) | (D & ~Mm)", "(D & Mm) | (N & ~Mm)"};
            std::string s = "{ uint64_t _t[2]; ";
            for (u32 k = 0; k < regs; ++k)
                s += F("{ const uint64_t D = c->ext.d[%u], N = c->ext.d[%u], Mm = c->ext.d[%u]; _t[%u] = %s; } ",
                       d + k, n + k, m + k, k, sel[sz]);
            for (u32 k = 0; k < regs; ++k) s += F("c->ext.d[%u] = _t[%u]; ", d + k, k);
            t.body = s + "}";
            return t;
        }
        std::string s;
        for (u32 k = 0; k < regs; ++k) {
            std::string a = F("c->ext.d[%u]", n + k), b = F("c->ext.d[%u]", m + k);
            s += F("c->ext.d[%u] = ", d + k) + F(ops[U][sz], a.c_str(), b.c_str()) + "; ";
        }
        // compute into temporaries first when destination overlaps a source of a later lane
        t.body = "{ uint64_t _t[2]; " + [&] {
            std::string x;
            for (u32 k = 0; k < regs; ++k) {
                std::string a = F("c->ext.d[%u]", n + k), b = F("c->ext.d[%u]", m + k);
                x += F("_t[%u] = ", k) + F(ops[U][sz], a.c_str(), b.c_str()) + "; ";
            }
            for (u32 k = 0; k < regs; ++k) x += F("c->ext.d[%u] = _t[%u]; ", d + k, k);
            return x;
        }() + "}";
        return t;
    }
    // VMAX/VMIN.F32
    if ((i & 0xFF800F10u) == 0xF2000F00u && !Bit(i, 20)) {
        const u32 is_min = Bit(i, 21);
        std::string s = "{ uint32_t _r[4]; ";
        for (u32 k = 0; k < 2 * regs; ++k)
            s += F("_r[%u] = a32_neon_fmaxmin(c->ext.s[%u], c->ext.s[%u], %u); ", k, 2 * n + k, 2 * m + k, is_min);
        for (u32 k = 0; k < 2 * regs; ++k) s += F("c->ext.s[%u] = _r[%u]; ", 2 * d + k, k);
        t.body = s + "}";
        return t;
    }
    // VEXT
    if ((i & 0xFFB00010u) == 0xF2B00000u) {
        const u32 imm4 = Bits(i, 11, 8);
        if (!Q && imm4 > 7) { t.handled = false; return t; }
        const u32 bytes = 8 * regs;
        std::string s = "{ uint8_t _in[32], _out[16]; ";
        s += F("memcpy(_in, &c->ext.d[%u], %u); memcpy(_in + %u, &c->ext.d[%u], %u); ", n, bytes, bytes, m, bytes);
        s += F("memcpy(_out, _in + %u, %u); memcpy(&c->ext.d[%u], _out, %u); }", imm4, bytes, d, bytes);
        t.body = s;
        return t;
    }
    // VDUP (scalar)
    if ((i & 0xFFB00F90u) == 0xF3B00C00u) {
        const u32 imm4 = Bits(i, 19, 16);
        u32 esize, idx;
        if (imm4 & 1) { esize = 8; idx = imm4 >> 1; }
        else if (imm4 & 2) { esize = 16; idx = imm4 >> 2; }
        else if (imm4 & 4) { esize = 32; idx = imm4 >> 3; }
        else { t.handled = false; return t; }
        const u64 mask = esize == 32 ? 0xFFFFFFFFull : ((1ull << esize) - 1);
        std::string s = F("{ uint64_t e = (c->ext.d[%u] >> %u) & 0x%llXull, r = 0; for (int k = 0; k < 64; k += %u) r |= e << k; ",
                          m, idx * esize, (unsigned long long)mask, esize);
        for (u32 k = 0; k < regs; ++k) s += F("c->ext.d[%u] = r; ", d + k);
        t.body = s + "}";
        return t;
    }
    // VLD1/VST1 single element to/from one lane
    if ((i & 0xFF900300u) == 0xF4800000u && Bits(i, 11, 10) != 3) {
        const u32 L = Bit(i, 21), Rn = Bits(i, 19, 16), Rm = Bits(i, 3, 0), size = Bits(i, 11, 10);
        const u32 ia = Bits(i, 7, 4);
        const u32 idx = size == 0 ? ia >> 1 : size == 1 ? ia >> 2 : ia >> 3;
        const u32 ebytes = 1u << size, shift = idx * ebytes * 8;
        if (Rn == 15) { t.handled = false; return t; }
        static const char* rdf[3] = {"a32_read8", "a32_read16", "a32_read32"};
        static const char* wrf[3] = {"a32_write8", "a32_write16", "a32_write32"};
        static const char* cst[3] = {"(uint8_t)", "(uint16_t)", "(uint32_t)"};
        const u64 mask = (ebytes == 4 ? 0xFFFFFFFFull : ((1ull << (8 * ebytes)) - 1)) << shift;
        std::string s = F("{ uint32_t a = c->r[%u]; ", Rn);
        if (L) s += F("c->ext.d[%u] = (c->ext.d[%u] & 0x%016llXull) | ((uint64_t)%s(c, a) << %u); ", d, d,
                      (unsigned long long)~mask, rdf[size], shift);
        else s += F("%s(c, a, %s(c->ext.d[%u] >> %u)); ", wrf[size], cst[size], d, shift);
        if (Rm == 13) s += F("c->r[%u] = a + %u; ", Rn, ebytes);
        else if (Rm != 15) s += F("c->r[%u] = a + c->r[%u]; ", Rn, Rm);
        t.body = s + "}";
        return t;
    }
    // VLD1/VST1 (multiple single elements, 1-4 registers)
    if ((i & 0xFF900000u) == 0xF4000000u) {
        const u32 L = Bit(i, 21), Rn = Bits(i, 19, 16), Rm = Bits(i, 3, 0), type = Bits(i, 11, 8);
        u32 cnt = type == 7 ? 1 : type == 10 ? 2 : type == 6 ? 3 : type == 2 ? 4 : 0;
        if (!cnt || Rn == 15 || d + cnt > 32) { t.handled = false; return t; }
        std::string s = F("{ uint32_t a = c->r[%u]; ", Rn);
        for (u32 k = 0; k < cnt; ++k)
            s += L ? F("c->ext.d[%u] = a32_read64(c, a + %u); ", d + k, 8 * k)
                   : F("a32_write64(c, a + %u, c->ext.d[%u]); ", 8 * k, d + k);
        if (Rm == 13) s += F("c->r[%u] = a + %u; ", Rn, 8 * cnt);
        else if (Rm != 15) s += F("c->r[%u] = a + c->r[%u]; ", Rn, Rm);
        t.body = s + "}";
        return t;
    }
    t.handled = false;
    return t;
}

// VDUP (core register) and VMOV between a core register and a 32-bit scalar
inline Tr NeonCoreTransfer(u32 i) {
    Tr t;
    const u32 Rt = Bits(i, 15, 12), dreg = DReg(Bits(i, 19, 16), Bit(i, 7));
    if (Rt == 15) { t.handled = false; return t; }
    if ((i & 0x0F900F5Fu) == 0x0E800B10u) { // VDUP.<size> Dd/Qd, Rt
        const u32 be = (Bit(i, 22) << 1) | Bit(i, 5), Q = Bit(i, 21);
        if (be == 3 || (Q && (dreg & 1))) { t.handled = false; return t; }
        const u32 esize = be == 0 ? 32 : be == 1 ? 16 : 8;
        std::string s = F("{ uint64_t e = c->r[%u] & 0x%llXull, r = 0; for (int k = 0; k < 64; k += %u) r |= e << k; ",
                          Rt, (unsigned long long)(esize == 32 ? 0xFFFFFFFFull : ((1ull << esize) - 1)), esize);
        for (u32 k = 0; k < (Q ? 2u : 1u); ++k) s += F("c->ext.d[%u] = r; ", dreg + k);
        t.body = s + "}";
        return t;
    }
    if ((i & 0x0FC00F7Fu) == 0x0E000B10u) { // VMOV.32 Dd[x], Rt / Rt, Dn[x]
        const u32 idx = Bit(i, 21), s_ = 2 * dreg + idx;
        t.body = Bit(i, 20) ? F("c->r[%u] = c->ext.s[%u];", Rt, s_) : F("c->ext.s[%u] = c->r[%u];", s_, Rt);
        return t;
    }
    // VMOV.{S,U}{8,16} Rt, Dn[x] / VMOV.{8,16} Dd[x], Rt: opc1:opc2 = 1xxx bytes, 0xx1 halfwords
    if ((i & 0x0F000F1Fu) == 0x0E000B10u && (Bit(i, 20) || !Bit(i, 23))) {
        const u32 opc1 = Bits(i, 22, 21), opc2 = Bits(i, 6, 5);
        u32 es, idx;
        if (opc1 & 2) { es = 8; idx = ((opc1 & 1) << 2) | opc2; }
        else if (opc2 & 1) { es = 16; idx = ((opc1 & 1) << 1) | (opc2 >> 1); }
        else { t.handled = false; return t; }
        const u32 sh = idx * es;
        const u64 mask = (1ull << es) - 1;
        if (Bit(i, 20)) {
            t.body = Bit(i, 23) ? F("c->r[%u] = (uint32_t)((c->ext.d[%u] >> %u) & 0x%llXull);", Rt, dreg, sh, (unsigned long long)mask)
                                : F("c->r[%u] = (uint32_t)(int32_t)(int%u_t)((c->ext.d[%u] >> %u) & 0x%llXull);", Rt, es, dreg, sh,
                                    (unsigned long long)mask);
        } else {
            t.body = F("c->ext.d[%u] = (c->ext.d[%u] & ~(0x%llXull << %u)) | ((uint64_t)(c->r[%u] & 0x%llXull) << %u);", dreg, dreg,
                       (unsigned long long)mask, sh, Rt, (unsigned long long)mask, sh);
        }
        return t;
    }
    t.handled = false;
    return t;
}

// ---------------------------------------------------------------------------
// Top-level decoder
// ---------------------------------------------------------------------------
inline Tr Decode(u32 i, u32 pc) {
    Tr t;
    const u32 cond = i >> 28;
    if (cond == 15) {
        // Unconditional space
        if ((i & 0xFE000000u) == 0xFA000000u) { // BLX (immediate) -> Thumb
            const s32 imm = ((s32)(i << 8) >> 6) | (Bit(i, 24) << 1);
            t.body = F("c->r[14] = %s; c->thumb = 1; c->r[15] = %s; return;", PcConst(pc + 4).c_str(),
                       PcConst((u32)(pc + 8 + imm)).c_str());
            t.terminates = true;
            return t;
        }
        if ((i & 0xFFFFFFF0u) == 0xF57FF040u || (i & 0xFFFFFFF0u) == 0xF57FF050u ||
            (i & 0xFFFFFFF0u) == 0xF57FF060u) { t.body = "/* barrier */"; return t; }
        if (i == 0xF57FF01Fu) { t.body = "a32_clrex(c);"; return t; }
        if (Bits(i, 27, 26) == 1 && Bits(i, 21, 20) == 1 && Bits(i, 15, 12) == 15) { t.body = "/* PLD/PLI */"; return t; }
        if (Bits(i, 27, 24) == 0xE && Bits(i, 11, 9) == 5) return VfpV8(i);
        return Neon(i);
    }
    const u32 op1 = Bits(i, 27, 25);
    // CRC32{C}{B,H,W}
    if ((i & 0x0F900DF0u) == 0x01000040u) {
        const u32 sz = Bits(i, 22, 21), Rn = Bits(i, 19, 16), Rd = Bits(i, 15, 12), Rm = Bits(i, 3, 0);
        if (sz == 3 || Rn == 15 || Rd == 15 || Rm == 15) { t.handled = false; return t; }
        t.body = F("c->r[%u] = a32_crc32(c->r[%u], c->r[%u], %u, %u);", Rd, Rn, Rm, 1u << sz, Bit(i, 9));
        return t;
    }
    // UDF: permanently undefined (compiler traps). Hand it to the host.
    if ((i & 0x0FF000F0u) == 0x07F000F0u) {
        t.body = F("c->r[15] = %s; a32_unhandled(c, 0x%08Xu, c->r[15]); return;", PcConst(pc).c_str(), i);
        t.terminates = true;
        return t;
    }
    switch (op1) {
    case 0: case 1: {
        const bool imm = op1 == 1;
        const u32 op = Bits(i, 24, 20);
        if (!imm) {
            const u32 b74 = Bits(i, 7, 4);
            if (b74 == 9) {
                if (Bit(i, 24) == 0) return Multiply(i);
                if (Bits(i, 23, 20) >= 8 && Bits(i, 11, 10) == 3) return Exclusive(i);
                t.handled = false; // SWP
                return t;
            }
            if ((b74 & 9) == 9) return ExtraLoadStore(i, pc); // 1011, 1101, 1111
            if ((op & 0x19) == 0x10) { // 10xx0: misc / halfword multiply
                if (Bit(i, 7) == 0) return Misc(i, pc);
                if (Bit(i, 4) == 0) return HalfwordMultiply(i);
                t.handled = false;
                return t;
            }
            return DataProcessing(i, pc);
        }
        if (op == 0x10 || op == 0x14) return MoveWide(i);
        if ((op & 0x1B) == 0x12) { // MSR immediate & hints
            if (Bits(i, 19, 16) == 0) { t.body = "/* hint */"; return t; }
            if (Bit(i, 22) || Bits(i, 17, 16)) { t.handled = false; return t; }
            bool k;
            u32 cc;
            t.body = F("a32_set_apsr(c, 0x%08Xu, %u);", ExpandImm(Bits(i, 11, 0), k, cc), Bits(i, 19, 18));
            return t;
        }
        return DataProcessing(i, pc);
    }
    case 2:
        return LoadStoreWordByte(i, pc);
    case 3:
        if (Bit(i, 4) == 0) return LoadStoreWordByte(i, pc);
        return Media(i, pc);
    case 4:
        return BlockTransfer(i, pc);
    case 5: {
        const s32 imm = (s32)(i << 8) >> 6;
        const u32 target = (u32)(pc + 8 + imm);
        if (Bit(i, 24)) t.body = F("c->r[14] = %s; c->r[15] = %s; return;", PcConst(pc + 4).c_str(), PcConst(target).c_str());
        else t.body = F("c->r[15] = %s; return;", PcConst(target).c_str());
        t.terminates = true;
        return t;
    }
    case 6:
        if (Bits(i, 11, 9) == 5) return VfpLoadStore(i, pc);
        if ((i & 0x0FF00F00u) == 0x0C500F00u && Bits(i, 3, 0) == 14 && Bits(i, 7, 4) <= 1) { // MRRC p15 c14: counters
            const u32 Rt = Bits(i, 15, 12), Rt2 = Bits(i, 19, 16);
            if (Rt == 15 || Rt2 == 15 || Rt == Rt2) { t.handled = false; return t; }
            t.body = F("{ uint64_t v = a32_read_counter(c); c->r[%u] = (uint32_t)v; c->r[%u] = (uint32_t)(v >> 32); }", Rt, Rt2);
            return t;
        }
        t.handled = false;
        return t;
    case 7:
        if (Bit(i, 24)) { // SVC
            t.body = F("c->r[15] = %s; a32_svc(c, 0x%Xu); return;", PcConst(pc + 4).c_str(), Bits(i, 23, 0));
            t.terminates = true;
            return t;
        }
        if (Bits(i, 11, 9) == 5) {
            if (Bit(i, 4) == 0) return VfpDataProcessing(i);
            if (Bit(i, 8)) return NeonCoreTransfer(i);
            return VfpTransfer(i);
        }
        if (Bits(i, 11, 8) == 15 && Bit(i, 4)) return Cp15(i);
        t.handled = false;
        return t;
    }
    t.handled = false;
    return t;
}

// Is this instruction a block terminator (possibly writes PC)? Used by code
// discovery; mirrors the `terminates` flag of Decode without generating code.
inline bool IsTerminator(u32 i, u32 pc) {
    Tr t = Decode(i, pc);
    return !t.handled || t.terminates;
}

// Translate one instruction into `out`. Returns true if the block stays open.
// `unhandled` (optional) reports whether the instruction fell back.
inline bool Translate(u32 i, u32 pc, std::string& out, bool* unhandled = nullptr) {
    Tr t = Decode(i, pc);
    if (unhandled) *unhandled = !t.handled;
    if (!t.handled) {
        out += F("    c->r[15] = %s; a32_unhandled(c, 0x%08Xu, c->r[15]); return;\n", PcConst(pc).c_str(), i);
        return false;
    }
    const u32 cond = i >> 28;
    const bool always = cond >= 14;
    if (t.body.empty()) t.body = "/* nop */";
    if (always) {
        out += "    " + t.body + "\n";
    } else {
        out += F("    if (%s) { ", CondExpr(cond)) + t.body + " }\n";
    }
    if (t.terminates) {
        if (!always) out += F("    c->r[15] = %s; return;\n", PcConst(pc + 4).c_str());
        return false;
    }
    return true;
}

} // namespace a32recomp
