// a32disc.h - code discovery for 32-bit ARM NSO modules.
//
// suyu's AArch64 path walks .text linearly. That is not safe for ARM code:
// compilers put literal pools (constants loaded with `ldr rX, [pc, #imm]`)
// between functions, and MHGU's main module carries ~930 KB of them. Decoding
// those words as instructions produces garbage blocks.
//
// Instead this follows control flow from known entry points:
//   * the module entry (start of .text)
//   * exported function symbols (.dynsym)
//   * relocation targets that point into .text (vtables, function pointers,
//     init arrays): R_ARM_RELATIVE addends and locally defined ABS32/GLOB_DAT/
//     JUMP_SLOT symbols
//   * direct branch/call targets found while walking
// and marks every PC-relative literal it sees as data.
#pragma once

#include "a32_to_c.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace a32recomp {

struct NsoImage {
    std::vector<u8> mem;        // flat module image (text, rodata, data, bss) at offset 0
    u32 text_off = 0, text_size = 0, ro_off = 0, ro_size = 0, data_off = 0, data_size = 0;
    u32 Word(u32 a) const { u32 v = 0; if (a + 4 <= mem.size()) memcpy(&v, &mem[a], 4); return v; }
    bool InText(u32 a) const { return a >= text_off && a < text_off + text_size; }
};

// Parse an NSO whose segments are stored uncompressed (hactool --uncompressed).
inline bool LoadNso(const std::vector<u8>& f, NsoImage& img) {
    if (f.size() < 0x100 || memcmp(f.data(), "NSO0", 4) != 0) return false;
    auto rd = [&](size_t o) { u32 v; memcpy(&v, &f[o], 4); return v; };
    u32 foff[3], moff[3], size[3];
    for (int k = 0; k < 3; ++k) {
        foff[k] = rd(0x10 + k * 0x10);
        moff[k] = rd(0x14 + k * 0x10);
        size[k] = rd(0x18 + k * 0x10);
    }
    const u32 bss = rd(0x3C);
    img.mem.assign(moff[2] + size[2] + bss, 0);
    for (int k = 0; k < 3; ++k) {
        if (foff[k] + size[k] > f.size()) return false;
        memcpy(&img.mem[moff[k]], &f[foff[k]], size[k]);
    }
    img.text_off = moff[0]; img.text_size = size[0];
    img.ro_off = moff[1]; img.ro_size = size[1];
    img.data_off = moff[2]; img.data_size = size[2];
    return true;
}

struct Discovery {
    enum : u8 { Unknown = 0, Code = 1, Data = 2 };
    std::vector<u8> kind;             // per text word
    std::set<u32> block_starts;
    std::set<u32> thumb_targets;      // BLX/BX into Thumb code (not walked yet)
    std::map<std::string, size_t> roots; // root counts by source
    size_t unhandled_stops = 0;       // walks that hit an untranslatable instruction
};

namespace detail {

inline bool IsUncondB(u32 i) { return (i & 0x0F000000u) == 0x0A000000u && (i >> 28) == 0xE; }

// PC-relative literal access: returns size in bytes (0 if none) and the address.
inline u32 LiteralRef(u32 i, u32 pc, u32& addr) {
    const u32 base = (pc + 8) & ~3u;
    const u32 cond = i >> 28;
    if (cond == 15) return 0;
    // LDR/LDRB literal: 010P U B 0 1 1111 (P=1, W=0, L=1)
    if ((i & 0x0F3F0000u) == 0x051F0000u) {
        u32 imm = i & 0xFFF;
        addr = Bit(i, 23) ? base + imm : base - imm;
        return Bit(i, 22) ? 1 : 4;
    }
    // LDRH/LDRSB/LDRSH/LDRD literal (immediate form, Rn = 15, P=1, W=0)
    if ((i & 0x0F6F0090u) == 0x014F0090u && (i & 0x60)) {
        const u32 op2 = Bits(i, 6, 5), L = Bit(i, 20);
        u32 imm = (Bits(i, 11, 8) << 4) | Bits(i, 3, 0);
        addr = Bit(i, 23) ? base + imm : base - imm;
        if (op2 == 1) return L ? 2 : 0;
        if (op2 == 2) return L ? 1 : 8;
        return L ? 2 : 0;
    }
    // VLDR literal
    if ((i & 0x0F3F0E00u) == 0x0D1F0A00u) {
        u32 imm = (i & 0xFF) * 4;
        addr = Bit(i, 23) ? base + imm : base - imm;
        return Bit(i, 8) ? 8 : 4;
    }
    return 0;
}

// Conservative: could this (untranslated) instruction change control flow?
inline bool MayWritePc(u32 i) {
    const u32 cond = i >> 28;
    if (cond == 15) {
        if (Bits(i, 27, 25) == 5) return true;                 // BLX imm
        if (Bits(i, 27, 25) == 4) return true;                 // RFE/SRS
        return false;                                          // NEON / ARMv8 FP / hints
    }
    if (Bits(i, 27, 25) == 5) return true;                     // B/BL
    if (Bits(i, 27, 25) == 4) return Bit(i, 15) || Bit(i, 22); // LDM with pc / exception return
    if (Bits(i, 27, 24) == 0xF) return true;                   // SVC
    if ((i & 0x0FF000F0u) == 0x07F000F0u) return true;         // UDF
    if (Bits(i, 27, 26) == 3) return Bits(i, 15, 12) == 15 && Bits(i, 11, 8) >= 14 && Bit(i, 20); // MRC to APSR is fine, but be safe
    return Bits(i, 15, 12) == 15;                              // anything targeting r15
}

} // namespace detail

/// extra_roots: module-relative addresses known to be code from outside the image, e.g.
/// block misses a previous run of the exported game recorded (recomp_misses.txt).
/// sweep: how hard to look for functions nothing visibly refers to (reached only through
/// pointers built at run time). 0: push {..., lr} prologues only. 1: also the other common
/// openers (str lr, [sp, #-4]!; push without lr; vpush; sub sp) after a function end or a
/// literal pool. 2: also any translatable instruction in that position.
inline Discovery Discover(const NsoImage& img, bool aggressive = true,
                          const std::vector<u32>* extra_roots = nullptr, int sweep = 0) {
    Discovery d;
    const u32 nwords = img.text_size / 4;
    d.kind.assign(nwords, Discovery::Unknown);
    std::deque<std::pair<u32, bool>> work; // (address, hard root)
    auto idx = [&](u32 a) { return (a - img.text_off) / 4; };
    auto add = [&](u32 a, const char* why) {
        if ((a & 3) || !img.InText(a)) return;
        if (why) d.roots[why]++;
        work.push_back({a, true});
    };
    // Fall-through after a call or an untaken conditional branch. Weaker than a
    // real branch target: calls to noreturn functions are followed by literal
    // pools, so this never overrides a word already known to be data.
    auto add_soft = [&](u32 a) {
        if ((a & 3) || !img.InText(a)) return;
        work.push_back({a, false});
    };
    auto mark_data = [&](u32 a, u32 n) {
        for (u32 x = a & ~3u; x < a + n; x += 4)
            if (img.InText(x) && d.kind[idx(x)] == Discovery::Unknown) d.kind[idx(x)] = Discovery::Data;
    };

    // ---- roots: entry
    add(img.text_off, "entry");
    if (extra_roots) {
        for (const u32 a : *extra_roots) add(a, "runtime miss (previous run)");
    }

    // ---- roots: MOD0 -> .dynamic -> symbols and relocations
    const u32 mod0 = img.Word(img.text_off + 4);
    mark_data(img.text_off + 4, 4);
    if (mod0 + 8 <= img.mem.size() && memcmp(&img.mem[mod0], "MOD0", 4) == 0) {
        mark_data(mod0, 0x1C);
        const u32 dyn = mod0 + img.Word(mod0 + 4);
        std::map<u32, u32> dt;
        for (u32 p = dyn; p + 8 <= img.mem.size(); p += 8) {
            u32 tag = img.Word(p), val = img.Word(p + 4);
            if (tag == 0) break;
            if (!dt.count(tag)) dt[tag] = val;
        }
        const u32 DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_INIT = 12, DT_FINI = 13, DT_REL = 17,
                  DT_RELSZ = 18, DT_JMPREL = 23;
        if (dt.count(DT_INIT)) add(dt[DT_INIT], "init/fini");
        if (dt.count(DT_FINI)) add(dt[DT_FINI], "init/fini");
        const u32 symtab = dt.count(DT_SYMTAB) ? dt[DT_SYMTAB] : 0;
        const u32 strtab = dt.count(DT_STRTAB) ? dt[DT_STRTAB] : 0;
        auto sym_value = [&](u32 s, u16* shndx) {
            const u32 e = symtab + s * 16;
            if (shndx) { u16 v; memcpy(&v, &img.mem[e + 14], 2); *shndx = v; }
            return img.Word(e + 4);
        };
        if (symtab && strtab > symtab) {
            for (u32 s = 1; s < (strtab - symtab) / 16; ++s) {
                u16 shndx;
                u32 v = sym_value(s, &shndx);
                const u8 type = img.mem[symtab + s * 16 + 12] & 0xF;
                if (shndx != 0 && type == 2 /*STT_FUNC*/) add(v, "exported symbol");
            }
        }
        auto walk_rel = [&](u32 start, u32 size) {
            for (u32 p = start; p + 8 <= start + size && p + 8 <= img.mem.size(); p += 8) {
                const u32 off = img.Word(p), info = img.Word(p + 4), type = info & 0xFF, sym = info >> 8;
                if (img.InText(off)) mark_data(off, 4); // a relocated word is never an instruction
                if (type == 23) { // R_ARM_RELATIVE: addend stored in place
                    const u32 target = img.Word(off);
                    if (img.InText(target)) add(target & ~1u, "relocation (relative)");
                    if (img.InText(target) && (target & 1)) d.thumb_targets.insert(target & ~1u);
                } else if ((type == 2 || type == 21 || type == 22) && symtab) {
                    u16 shndx;
                    const u32 v = sym_value(sym, &shndx);
                    if (shndx != 0 && img.InText(v)) add(v, "relocation (symbol)");
                }
            }
        };
        if (dt.count(DT_REL) && dt.count(DT_RELSZ)) walk_rel(dt[DT_REL], dt[DT_RELSZ]);
        if (dt.count(DT_JMPREL) && dt.count(DT_PLTRELSZ)) walk_rel(dt[DT_JMPREL], dt[DT_PLTRELSZ]);
    }

    // ---- walk
    auto run_walk = [&]() {
    while (!work.empty()) {
        auto [pc, hard] = work.front();
        work.pop_front();
        if (!img.InText(pc)) continue;
        if (!hard && d.kind[idx(pc)] == Discovery::Data) continue;
        d.block_starts.insert(pc);
        const u32 start = pc;
        bool first = true;
        u32 lit_val[16];
        u16 lit_ok = 0; // registers holding a value loaded from a literal pool in this run
        for (;;) {
            if (!img.InText(pc)) break;
            u8& k = d.kind[idx(pc)];
            if (k == Discovery::Code) break;   // already walked from here
            if (k == Discovery::Data && !(first && hard)) break; // ran into a literal pool
            first = false;
            k = Discovery::Code;               // a real branch target wins over a literal guess
            const u32 i = img.Word(pc);
            const u32 cond = i >> 28;
            u32 lit;
            const u32 Rd_ = Bits(i, 15, 12);
            if (u32 n = detail::LiteralRef(i, pc, lit)) {
                mark_data(lit, n);
                // ldr rX, [pc, #imm] (word): remember the loaded value for PIC tracking
                if (n == 4 && (i & 0x0E500000u) == 0x04100000u && Rd_ != 15 && img.InText(lit)) {
                    lit_val[Rd_] = img.Word(lit);
                    lit_ok |= (u16)(1u << Rd_);
                } else if (Rd_ != 15) {
                    lit_ok &= (u16)~(1u << Rd_);
                }
            } else if ((i & 0x0FFF0FF0u) == 0x008F0000u && cond == 14) {
                // add rD, pc, rM  (position-independent address of code or data)
                const u32 Rm_ = Bits(i, 3, 0);
                if ((lit_ok >> Rm_) & 1) {
                    const u32 target = pc + 8 + lit_val[Rm_];
                    if (img.InText(target & ~1u)) { d.roots["pc-relative pointer"]++; add_soft(target & ~1u); }
                }
                lit_ok &= (u16)~(1u << Rd_);
            } else {
                lit_ok &= (u16)~(1u << Rd_); // conservatively forget the destination
                if ((i & 0x0E100000u) == 0x08100000u) lit_ok &= (u16)~Bits(i, 15, 0); // LDM
            }
            // direct branches
            if ((i & 0x0E000000u) == 0x0A000000u) {
                const u32 target = (u32)(pc + 8 + ((s32)(i << 8) >> 6));
                if (cond == 15) { // BLX imm -> Thumb
                    d.thumb_targets.insert(target | (Bit(i, 24) << 1));
                } else {
                    add(target, nullptr);
                }
            }
            Tr t = Decode(i, pc);
            if (!t.handled) {
                // A conditional word the lifter doesn't know, or one from the unconditional space
                // outside Advanced SIMD, is a data word the walk ran into (compilers don't emit
                // those), unless a run of the game recorded executing exactly here.
                const bool undef_space = cond == 15 && (i & 0x0E000000u) != 0x02000000u && (i & 0x0F000000u) != 0x04000000u;
                if ((cond < 14 || undef_space) && !(hard && pc == start)) {
                    k = Discovery::Data;
                    if (pc == start) d.block_starts.erase(pc);
                    break;
                }
                d.unhandled_stops++;
                if (!detail::MayWritePc(i)) { pc += 4; continue; } // keep walking past it
                break;
            }
            if (!t.terminates) { pc += 4; continue; }
            // terminator
            const bool call = ((i & 0x0F000000u) == 0x0B000000u) || cond == 15 ||
                              ((i & 0x0FFFFFF0u) == 0x012FFF30u); // BL, BLX imm, BLX reg
            // jump tables: add{cond} pc, pc, rX, lsl #2 followed by a run of `b` instructions
            if ((i & 0x0FFFFFF0u) == 0x008FF100u) {
                for (u32 e = pc + 8; img.InText(e) && detail::IsUncondB(img.Word(e)); e += 4) add(e, nullptr);
            }
            // ldr{cond} pc, [pc, rX, lsl #2]: table of relocated absolute addresses follows
            if ((i & 0x0FFFFFF0u) == 0x079FF100u) {
                for (u32 e = pc + 8; img.InText(e) && img.InText(img.Word(e) & ~1u); e += 4) mark_data(e, 4);
            }
            // relative switch table (the common MHGU form):
            //   cmp rI, #N; bhi default; lsl rI, rI, #2; adr rB, table; ldr rO, [rI, rB]; add pc, rO, rB
            // Each word of the table is an offset from the table to its case; the table ends
            // where the first case begins.
            if ((i & 0x0FF0FFF0u) == 0x0080F000u) {
                const u32 rn = Bits(i, 19, 16), rm = Bits(i, 3, 0);
                u32 base = 0, bound = ~0u;
                for (u32 back = 1; back <= 16 && img.InText(pc - 4 * back); ++back) {
                    const u32 p = pc - 4 * back, w = img.Word(p);
                    if (!base && (w & 0x0FFF0000u) == 0x028F0000u &&
                        (Bits(w, 15, 12) == rn || Bits(w, 15, 12) == rm)) { // adr rB, label
                        const u32 rot = Bits(w, 11, 8) * 2, imm8 = w & 0xFF;
                        base = p + 8 + (rot ? (imm8 >> rot) | (imm8 << (32 - rot)) : imm8);
                    }
                    if (bound == ~0u && (w & 0x0FF0F000u) == 0x03500000u && Bits(w, 11, 8) == 0)
                        bound = (w & 0xFF) + 1; // nearest cmp rI, #imm8 (earlier ones test other things)
                }
                if (base && img.InText(base)) {
                    u32 end = ~0u, n = 0;
                    // Offsets may be negative (cases placed before the table); without a cmp
                    // bound those must stay within 1 MB so a following instruction isn't read
                    // as one.
                    for (u32 e = base; e < end && n < bound && img.InText(e); e += 4, ++n) {
                        const u32 off = img.Word(e), target = base + off;
                        const bool back = (s32)off < 0;
                        if ((off & 3) || off == 0 || !img.InText(target) || (target >= e && target < e + 4)) break;
                        if (back && bound == ~0u && (s32)off < -0x100000) break;
                        if (!back) end = std::min(end, target);
                        mark_data(e, 4);
                        add(target, "relative switch case");
                    }
                }
            }
            // computed jump into an unrolled ladder (compiler-rt's __aeabi_uidiv & co.):
            //   add rX, pc, #imm; sub rX, rX, rK, lsl #a; [sub rX, rX, rK, lsl #b ...]; bx rX
            // lands on base - k * stride for some k; every such target after the bx is a
            // ladder step (the ladder runs from just after the bx up to the base).
            if ((i & 0x0FFFFFF0u) == 0x012FFF10u) {
                const u32 rx = Bits(i, 3, 0);
                u32 base = 0, stride = 0, rk = 16;
                for (u32 back = 1; back <= 8 && img.InText(pc - 4 * back); ++back) {
                    const u32 p = pc - 4 * back, w = img.Word(p);
                    if ((w & 0x0FE00070u) == 0x00400000u && Bits(w, 19, 16) == rx &&
                        Bits(w, 15, 12) == rx && (rk == 16 || Bits(w, 3, 0) == rk)) {
                        rk = Bits(w, 3, 0); // sub rX, rX, rK, lsl #s
                        stride += 1u << Bits(w, 11, 7);
                        continue;
                    }
                    if ((w & 0x0FFFF000u) == (0x028F0000u | (rx << 12))) { // add rX, pc, #imm
                        const u32 rot = Bits(w, 11, 8) * 2, imm8 = w & 0xFF;
                        base = p + 8 + (rot ? (imm8 >> rot) | (imm8 << (32 - rot)) : imm8);
                        break;
                    }
                    if (Bits(w, 15, 12) == rx || Bits(w, 3, 0) == rx) break; // rX used otherwise
                }
                if (base && stride && base > pc && base - pc <= 0x1000 && img.InText(base)) {
                    for (u32 t2 = base; t2 > pc; t2 -= stride) {
                        add(t2, "computed ladder step");
                        if (t2 < stride) break;
                    }
                }
            }
            if (call || cond != 14 || t.body.find("a32_svc") != std::string::npos) {
                add_soft(pc + 4); // execution continues after a call / untaken branch
            }
            break;
        }
    }
    };
    run_walk();

    // ---- prologue scan: push {..., lr} right after a function end or a literal pool
    auto ends_function = [&](u32 a) {
        const u32 i = img.Word(a);
        if ((i >> 28) != 14) return false;
        if ((i & 0x0FFFFFF0u) == 0x012FFF10u) return true;                  // bx rM
        if ((i & 0x0F000000u) == 0x0A000000u) return true;                  // b
        if ((i & 0x0FFF8000u) == 0x08BD8000u) return true;                  // pop {..., pc}
        if ((i & 0x0E50F000u) == 0x0410F000u) return true;                  // ldr pc, [...] (incl. pop {pc}, PLT)
        if ((i & 0x0FF000F0u) == 0x07F000F0u) return true;                  // udf (trap after a noreturn call)
        return false;
    };
    // Two phases: first only prologues at a clear boundary (after data or a
    // function end), iterated to a fixpoint; then, optionally, every remaining
    // push {..., lr} in unreached territory (code only called from other
    // unreached code, which is often dead but may still run).
    for (int phase = 0; phase < (aggressive ? 2 : 1); ++phase) {
        for (int pass = 0; pass < 64; ++pass) {
            size_t added = 0;
            for (u32 w = 1; w < nwords; ++w) {
                if (d.kind[w] != Discovery::Unknown) continue;
                const u32 a = img.text_off + w * 4, i = img.Word(a);
                if ((i & 0xFFFF4000u) != 0xE92D4000u) continue;                // push {..., lr}
                if (phase == 1 || d.kind[w - 1] == Discovery::Data || ends_function(a - 4)) {
                    add(a, phase ? "prologue scan (aggressive)" : "prologue scan");
                    added++;
                }
            }
            if (!added) break;
            run_walk();
        }
    }

    // ---- boundary sweep: function starts without a push {..., lr}
    const auto is_opener = [](u32 i) {
        return i == 0xE52DE004u ||                 // str lr, [sp, #-4]!  (push {lr})
               (i & 0xFFFF0000u) == 0xE92D0000u || // push {...}
               (i & 0xFFBF0E00u) == 0xED2D0A00u || // vpush
               (i & 0xFFFFF000u) == 0xE24DD000u;   // sub sp, sp, #imm
    };
    for (int round = 0; sweep > 0 && round < 32; ++round) {
        for (int pass = 0; pass < 64; ++pass) {
            size_t added = 0;
            for (u32 w = 1; w < nwords; ++w) {
                if (d.kind[w] != Discovery::Unknown) continue;
                const u32 a = img.text_off + w * 4, i = img.Word(a);
                if (d.kind[w - 1] != Discovery::Data && !ends_function(a - 4)) continue;
                bool take = is_opener(i);
                if (!take && sweep >= 2 && (i >> 28) == 14 && i != 0 && i != 0xE320F000u) {
                    // any instruction that doesn't jump; one the lifter can't translate still
                    // starts a block (it runs on the JIT, the rest of the block natively)
                    // C++ catch handlers (reached only through unwind tables) open with a call
                    // and empty functions are a lone bx lr
                    take = !detail::MayWritePc(i) || (i & 0x0E000000u) == 0x0A000000u || i == 0xE12FFF1Eu;
                }
                if (take) {
                    add(a, sweep >= 2 ? "boundary sweep" : "prologue scan (other openers)");
                    added++;
                }
            }
            if (!added) break;
            run_walk();
        }
        if (sweep < 2) break;
        // Cleanup: whatever no walk reached and that opens with words no compiler emits as a
        // function's first instruction (condition other than "always": table entries, float
        // constants, zero words; and the alignment nop) is data or padding. Each run's leading
        // stretch of such words is marked, which puts the code behind it on a boundary for
        // the next sweep round; repeat until nothing changes.
        size_t marked = 0;
        for (u32 w = 0; w < nwords;) {
            if (d.kind[w] != Discovery::Unknown) {
                ++w;
                continue;
            }
            for (; w < nwords && d.kind[w] == Discovery::Unknown; ++w) {
                const u32 i = img.Word(img.text_off + w * 4);
                if ((i >> 28) == 14 && i != 0xE320F000u) break;
                d.kind[w] = Discovery::Data;
                marked++;
            }
            while (w < nwords && d.kind[w] == Discovery::Unknown) ++w;
        }
        d.roots["cleanup: data / padding words"] += marked;
        if (!marked) break;
    }

    return d;
}

} // namespace a32recomp
