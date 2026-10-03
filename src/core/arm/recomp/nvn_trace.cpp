// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "core/arm/recomp/nvn_census.h"
#include "core/arm/recomp/nvn_shadow.h"
#include "core/arm/recomp/nvn_trace.h"
#include "core/arm/recomp/recomp_hooks.h"
#include "core/core.h"
#include "core/memory.h"

namespace Core {

namespace {

struct Pending {
    u32 ret_pc;
    u32 sp;
    std::string name;
};

// Argument census: r1-r3 of each call (r0 is the object), plus for nvnProgramSetShaders the
// words its r2 points at (the per-stage shader table), so the shaders a program binds can be
// matched to the game's shader package offline. A second census adds the first two stack
// words (arguments 5-6 of calls that have them, junk for the rest); kept apart so the junk
// can't crowd out the register tuples. Distinct tuples per entry point are capped; calls
// past the cap are only counted.
constexpr size_t kArgWords = 3;
constexpr size_t kStackWords = 2;
constexpr size_t kDerefWords = 24;
constexpr size_t kMaxArgSets = 1024;
// Deref rows also keep r0 (the program object BindProgram is later handed), in the last slot.
using ArgKey = std::array<u32, kArgWords + kDerefWords + 1>;
static_assert(kStackWords <= kDerefWords);

struct ArgHash {
    size_t operator()(const ArgKey& k) const {
        u64 h = 0xcbf29ce484222325ull;
        for (const u32 w : k) {
            h = (h ^ w) * 0x100000001b3ull;
        }
        return static_cast<size_t>(h);
    }
};

struct Entry {
    std::vector<std::string> names; // several names can share one implementation
    u64 calls{};
    bool deref{};
    NvnShadow::Observer observe{}; // shadow layer (G1), null for calls it ignores
    NvnTrace::Replacement replace{}; // native stand-in (G3), null: the guest function runs
    bool census{};                 // command buffer call: sampled by the method census
    std::unordered_map<ArgKey, u64, ArgHash> args;
    u64 args_dropped{};
    std::unordered_map<ArgKey, u64, ArgHash> args_stack; // r1-r3 + [sp], [sp+4]
    u64 args_stack_dropped{};
};

std::mutex g_lock;
std::unordered_map<std::string, NvnTrace::Replacement> g_replacements; // by entry name
std::vector<Pending> g_pending;                // lookups in flight
std::unordered_map<u32, Entry> g_entries;      // guest address -> NVN entry point
std::unordered_map<u32, int> g_lookup_fns;     // address -> which arg holds the name
u64 g_frames{};
u32 g_present_addr{};
u32 g_acquire_addr{};
u64 g_acquires{};
// nvn_programs.txt: one line per nvnProgramSetShaders call (see OnHookedEntry).
constexpr size_t kMaxPrograms = 65536;
constexpr u32 kControlWords = 16;
std::vector<std::string> g_programs;
std::chrono::steady_clock::time_point g_last_flush{};

std::filesystem::path UserFile(const char* name) {
    return Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / name;
}

/// Finds an exported function in a loaded module by walking its MOD0 -> .dynamic.
u32 FindExport(Memory::Memory& mem, u64 base, const std::string& want) {
    const u32 mod0 = static_cast<u32>(base) + mem.Read32(base + 4);
    if (mem.Read32(mod0) != 0x30444F4D) { // "MOD0"
        return 0;
    }
    const u32 dyn = mod0 + mem.Read32(mod0 + 4);
    u32 symtab = 0, strtab = 0;
    for (u32 p = dyn; p < dyn + 0x1000; p += 8) {
        const u32 tag = mem.Read32(p), val = mem.Read32(p + 4);
        if (tag == 0) break;
        if (tag == 6 && !symtab) symtab = val;  // DT_SYMTAB
        if (tag == 5 && !strtab) strtab = val;  // DT_STRTAB
    }
    if (!symtab || strtab <= symtab) {
        return 0;
    }
    const u64 st = base + symtab, sr = base + strtab;
    const u32 count = (strtab - symtab) / 16;
    for (u32 i = 1; i < count; ++i) {
        const u64 e = st + u64{i} * 16;
        const u16 shndx = static_cast<u16>(mem.Read32(e + 12) >> 16);
        if (shndx == 0) continue;
        if (mem.ReadCString(sr + mem.Read32(e), 128) == want) {
            return static_cast<u32>(base) + mem.Read32(e + 4);
        }
    }
    return 0;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const std::string x{suffix};
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

} // namespace

u32 FindModuleExport(Memory::Memory& mem, u64 base, const std::string& name) {
    return FindExport(mem, base, name);
}

NvnTrace& NvnTrace::Get() {
    static NvnTrace instance;
    return instance;
}

void NvnTrace::Watch(u32 addr) {
    RecompHooks::Mark(addr); // chained branches must not skip a watched entry
    const u32 off = addr - watch_lo;
    if (off >= watch_span) {
        return;
    }
    bits[off >> 5].fetch_or(static_cast<u8>(1u << ((off >> 2) & 7)), std::memory_order_relaxed);
}

void NvnTrace::Attach(System& system_, const std::map<u64, std::string>& modules) {
    static std::mutex attach_lock;
    std::scoped_lock alk{attach_lock};
    if (attach_tried || modules.empty()) {
        return;
    }
    attach_tried = true;
    std::error_code ec;
    if (std::filesystem::exists(UserFile("nvn_trace_off"), ec)) {
        LOG_INFO(Core_ARM, "nvn trace: disabled (nvn_trace_off present)");
        return;
    }
    system = &system_;
    auto& mem = system_.ApplicationMemory();
    u32 boot = 0;
    for (const auto& [base, name] : modules) {
        if ((boot = FindExport(mem, base, "nvnBootstrapLoader")) != 0) {
            LOG_INFO(Core_ARM, "nvn trace: nvnBootstrapLoader in {} at {:#x}", name, boot);
            break;
        }
    }
    if (!boot) {
        LOG_INFO(Core_ARM, "nvn trace: no nvnBootstrapLoader export; not tracing");
        return;
    }
    // Watch every loaded module: NVN entry points live in the SDK, but the
    // bitmap is cheap (1 bit per word) and the range check stays one compare.
    watch_lo = static_cast<u32>(modules.begin()->first);
    watch_span = static_cast<u32>(modules.rbegin()->first) + 0x4000000u - watch_lo;
    bits = new std::atomic<u8>[(watch_span >> 5) + 1]();
    {
        std::scoped_lock lk{g_lock};
        g_lookup_fns[boot] = 0; // nvnBootstrapLoader(const char* name)
        g_entries[boot].names.push_back("nvnBootstrapLoader");
        g_last_flush = std::chrono::steady_clock::now();
    }
    Watch(boot);
    NvnShadow::Start();
    enabled.store(true, std::memory_order_release);
}

void NvnTrace::Replace(const std::string& name, Replacement fn) {
    std::scoped_lock lk{g_lock};
    g_replacements[name] = fn;
}

bool NvnTrace::OnHookedEntry(u32 pc, u32* regs) {
    bool flush_due = false;
    Replacement replace{};
    {
    std::scoped_lock lk{g_lock};
    if (const auto it = g_entries.find(pc); it != g_entries.end()) {
        auto& e = it->second;
        ++e.calls;
        auto& mem = system->ApplicationMemory();
        if (e.observe) {
            e.observe(regs, mem);
        }
        replace = e.replace;
        if (e.census && !replace && !e.names.empty() &&
            NvnCensus::Begin(e.names.front(), e.calls - 1, regs, mem)) {
            pending_returns.store(static_cast<int>(g_pending.size()) + NvnCensus::Pending(),
                                  std::memory_order_relaxed);
        }
        ArgKey key{};
        key[0] = regs[1];
        key[1] = regs[2];
        key[2] = regs[3];
        if (e.deref) {
            key[kArgWords + kDerefWords] = regs[0];
            for (size_t k = 0; regs[2] != 0 && k < kDerefWords; ++k) {
                key[kArgWords + k] = mem.Read32(regs[2] + static_cast<u32>(4 * k));
            }
            // Every program, uncapped: object, then per stage (16-byte entries as observed:
            // u64 GPU address of the code, u32 control pointer) the GPU address, control
            // pointer and the first bytes of the control data, to match against the package.
            if (regs[2] != 0 && g_programs.size() < kMaxPrograms) {
                std::string line = fmt::format("{:08x}\t{}", regs[0], regs[1]);
                for (u32 s = 0; s < std::min<u32>(regs[1], 6); ++s) {
                    const u32 entry = regs[2] + 16 * s;
                    const u64 gpu = mem.Read32(entry) | (u64{mem.Read32(entry + 4)} << 32);
                    const u32 control = mem.Read32(entry + 8);
                    line += fmt::format("\t{:x} {:08x} ", gpu, control);
                    for (u32 w = 0; control != 0 && w < kControlWords; ++w) {
                        line += fmt::format("{:08x}", mem.Read32(control + 4 * w));
                    }
                }
                g_programs.push_back(std::move(line));
            }
        }
        const auto count = [](auto& map, const ArgKey& k, u64& dropped) {
            if (const auto a = map.find(k); a != map.end()) {
                ++a->second;
            } else if (map.size() < kMaxArgSets) {
                map.emplace(k, 1);
            } else {
                ++dropped;
            }
        };
        count(e.args, key, e.args_dropped);
        ArgKey with_stack{};
        std::copy_n(key.begin(), kArgWords, with_stack.begin());
        with_stack[kArgWords] = mem.Read32(regs[13]);
        with_stack[kArgWords + 1] = mem.Read32(regs[13] + 4);
        count(e.args_stack, with_stack, e.args_stack_dropped);
    }
    if (pc == g_present_addr) {
        ++g_frames;
    }
    if (pc == g_acquire_addr) {
        ++g_acquires;
    }
    if (const auto lf = g_lookup_fns.find(pc); lf != g_lookup_fns.end()) {
        // Name argument: r0 for the bootstrap loader, r1 for obj->GetProcAddress(obj, name).
        std::string name = system->ApplicationMemory().ReadCString(regs[lf->second], 128);
        g_pending.push_back({regs[14], regs[13], std::move(name)});
        pending_returns.store(static_cast<int>(g_pending.size()) + NvnCensus::Pending(),
                              std::memory_order_relaxed);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_flush > std::chrono::seconds(5)) {
        g_last_flush = now;
        flush_due = true;
    }
    }
    if (flush_due) {
        Flush();
    }
    if (replace) {
        replace(regs, system->ApplicationMemory());
        regs[15] = regs[14] & ~1u; // return to the caller (ARM state only in MHGU)
        return true;
    }
    return false;
}

void NvnTrace::OnMaybeReturn(u32 pc, const u32* regs) {
    std::scoped_lock lk{g_lock};
    if (NvnCensus::Pending() != 0) {
        const bool ended = NvnCensus::End(pc, regs, system->ApplicationMemory());
        // Also after an expiry: End may have dropped samples whose return never came.
        pending_returns.store(static_cast<int>(g_pending.size()) + NvnCensus::Pending(),
                              std::memory_order_relaxed);
        if (ended) {
            return;
        }
    }
    const auto it = std::find_if(g_pending.begin(), g_pending.end(), [&](const Pending& p) {
        return p.ret_pc == pc && p.sp == regs[13];
    });
    if (it == g_pending.end()) {
        return;
    }
    const u32 fn = regs[0] & ~1u;
    if (fn != 0) {
        auto& e = g_entries[fn];
        if (std::find(e.names.begin(), e.names.end(), it->name) == e.names.end()) {
            e.names.push_back(it->name);
        }
        if (!e.observe) {
            e.observe = NvnShadow::Find(it->name);
        }
        if (!e.replace) {
            if (const auto r = g_replacements.find(it->name); r != g_replacements.end()) {
                e.replace = r->second;
            }
        }
        e.census = e.census || NvnCensus::Wants(it->name);
        if (EndsWith(it->name, "GetProcAddress")) {
            g_lookup_fns[fn] = 1;
        }
        if (it->name == "nvnQueuePresentTexture") {
            g_present_addr = fn;
        }
        if (it->name == "nvnWindowAcquireTexture") {
            g_acquire_addr = fn;
        }
        if (it->name == "nvnProgramSetShaders") {
            e.deref = true;
        }
        Watch(fn);
    }
    g_pending.erase(it);
    pending_returns.store(static_cast<int>(g_pending.size()) + NvnCensus::Pending(),
                          std::memory_order_relaxed);
}

void NvnTrace::Flush(bool final) {
    if (!enabled.load(std::memory_order_acquire)) {
        return;
    }
    // The shadow report copies its tables under the lock the draw observers take: every
    // 30 s (and at exit) rather than with every 5 s census rewrite.
    static std::chrono::steady_clock::time_point last_shadow{};
    if (const auto now = std::chrono::steady_clock::now(); final || now - last_shadow > std::chrono::seconds(30)) {
        last_shadow = now;
        NvnShadow::Flush(UserFile("nvn_shadow.txt"));
        std::scoped_lock lk{g_lock};
        NvnCensus::Flush(UserFile("nvn_methods.txt"));
    }
    {
        std::vector<std::string> programs;
        {
            std::scoped_lock lk{g_lock};
            programs = g_programs;
        }
        if (!programs.empty()) {
            std::ofstream prog(UserFile("nvn_programs.txt"), std::ios::trunc);
            prog << "# nvnProgramSetShaders calls: program object, stage count, then per stage the "
                    "code's GPU address, the control pointer and the first "
                 << kControlWords * 4 << " bytes of control data\n";
            for (const auto& line : programs) {
                prog << line << '\n';
            }
        }
    }
    struct Row {
        u64 calls;
        u32 addr;
        std::string names;
        bool deref;
        u64 dropped;
        std::vector<std::pair<u64, ArgKey>> args;
        u64 stack_dropped;
        std::vector<std::pair<u64, ArgKey>> args_stack;
    };
    std::vector<Row> rows;
    u64 frames;
    {
        std::scoped_lock lk{g_lock};
        // Frames are window acquires, one per rendered frame; presents undercount (the game
        // can present through a pointer looked up separately from the one recorded).
        frames = g_acquires ? g_acquires : g_frames;
        for (const auto& [addr, e] : g_entries) {
            std::string n;
            for (const auto& s : e.names) {
                n += (n.empty() ? "" : " / ") + s;
            }
            Row r{e.calls, addr, std::move(n), e.deref, e.args_dropped, {}, e.args_stack_dropped, {}};
            r.args.reserve(e.args.size());
            for (const auto& [k, c] : e.args) {
                r.args.emplace_back(c, k);
            }
            r.args_stack.reserve(e.args_stack.size());
            for (const auto& [k, c] : e.args_stack) {
                r.args_stack.emplace_back(c, k);
            }
            rows.push_back(std::move(r));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.calls != b.calls ? a.calls > b.calls : a.names < b.names;
    });
    std::ofstream out(UserFile("nvn_trace.txt"), std::ios::trunc);
    if (!out) {
        return;
    }
    size_t used = 0;
    for (const auto& r : rows) {
        used += r.calls != 0;
    }
    out << "# NVN census: " << rows.size() << " entry points looked up, " << used
        << " called; frames: " << frames << "\n"
        << "# calls\tper_frame\taddress\tname(s)\n";
    for (const auto& r : rows) {
        char addr[16];
        std::snprintf(addr, sizeof(addr), "%08x", r.addr);
        out << r.calls << '\t';
        if (frames) {
            char pf[32];
            std::snprintf(pf, sizeof(pf), "%.1f", double(r.calls) / double(frames));
            out << pf;
        } else {
            out << '-';
        }
        out << '\t' << addr << '\t' << r.names << '\n';
    }
    out.close();

    // nvn_args.txt: per called entry point, its distinct argument tuples, most frequent first.
    std::ofstream args(UserFile("nvn_args.txt"), std::ios::trunc);
    if (!args) {
        return;
    }
    args << "# NVN argument census: distinct (r1 r2 r3) per entry point, r0 being the object, then "
            "the same with [sp] [sp+4] added (\"+stack\", meaningful only for calls with more than "
            "four arguments); at most "
         << kMaxArgSets << " kept, later new tuples only counted\n"
         << "# nvnProgramSetShaders rows add the " << kDerefWords
         << " words r2 points at and r0 (the program object)\n";
    for (auto& r : rows) {
        if (r.calls == 0) {
            continue;
        }
        const auto by_count = [](const auto& a, const auto& b) { return a.first > b.first; };
        std::sort(r.args.begin(), r.args.end(), by_count);
        std::sort(r.args_stack.begin(), r.args_stack.end(), by_count);
        char head[32];
        std::snprintf(head, sizeof(head), "%08x", r.addr);
        args << "\n== " << r.names << " @" << head << ": " << r.calls << " calls, "
             << r.args.size() << " distinct";
        if (r.dropped) {
            args << " (cap hit; " << r.dropped << " calls with further tuples)";
        }
        args << '\n';
        for (const auto& [c, k] : r.args) {
            char line[512];
            int n = std::snprintf(line, sizeof(line), "%llu\t%08x %08x %08x",
                                  static_cast<unsigned long long>(c), k[0], k[1], k[2]);
            for (size_t w = 0; r.deref && w < kDerefWords; ++w) {
                n += std::snprintf(line + n, sizeof(line) - n, "%s%08x", w ? " " : "\t| ",
                                   k[kArgWords + w]);
            }
            if (r.deref) {
                std::snprintf(line + n, sizeof(line) - n, "\tr0 %08x", k[kArgWords + kDerefWords]);
            }
            args << line << '\n';
        }
        args << "-- +stack: " << r.args_stack.size() << " distinct, top 64";
        if (r.stack_dropped) {
            args << " (cap hit; " << r.stack_dropped << " calls with further tuples)";
        }
        args << '\n';
        // Most frequent 64 only: the file is rewritten every few seconds.
        for (size_t i = 0; i < std::min<size_t>(r.args_stack.size(), 64); ++i) {
            const auto& [c, k] = r.args_stack[i];
            char line[96];
            std::snprintf(line, sizeof(line), "%llu\t%08x %08x %08x %08x %08x\n",
                          static_cast<unsigned long long>(c), k[0], k[1], k[2], k[3], k[4]);
            args << line;
        }
    }
}

} // namespace Core
