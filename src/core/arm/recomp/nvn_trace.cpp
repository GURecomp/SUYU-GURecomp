// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
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
#include "core/arm/recomp/nvn_rec.h"
#include "core/arm/recomp/nvn_shadow.h"
#include "core/arm/recomp/nvn_trace.h"
#include "core/arm/recomp/game_settings.h"
#include "core/arm/recomp/recomp_hooks.h"
#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/stall_ring.h"
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
    int helper{-1};                // runtime-library helper (kL2D...), else -1
    bool probe{};                  // observed address (probe_pcs), not an NVN entry point
    std::unordered_map<ArgKey, u64, ArgHash> args;
    u64 args_dropped{};
    std::unordered_map<ArgKey, u64, ArgHash> args_stack; // r1-r3 + [sp], [sp+4]
    u64 args_stack_dropped{};
};

struct ProbeStat {
    u64 hits{};
    std::map<u32, u64> callers;
    std::vector<std::string> samples;
};
std::map<u32, ProbeStat> g_probe;  // guest address -> what was seen there (guarded by g_lock)


// ---- runtime-library helpers the game calls hot (found with probe_pcs; behaviour by observation,
// each checked word for word against the guest routine before it is switched on) -----------------
enum Helper : int { kL2D = 0, kD2LZ = 1, kMemcpy = 2, kHelperCount = 3 };
struct HelperInfo {
    const char* name;
    u32 sdk_offset;
    bool proven;  // verified; only these replace the guest routine
};
// int64 -> double (r1:r0 -> r1:r0), double -> int64 (r1:r0 -> r1:r0), memcpy/memmove(dst, src, n)
constexpr HelperInfo kHelpers[kHelperCount] = {
    {"helper_int64_to_double", 0x3f40e0, true},
    {"helper_double_to_int64", 0x3f3e1c, true},
    {"helper_memcpy", 0x3f91dc, true},
};

bool HelperPredict(int kind, const u32* r, Memory::Memory& mem, u32* out, std::vector<u8>* snap) {
    if (kind == kL2D) {
        const s64 v = static_cast<s64>(r[0] | (u64{r[1]} << 32));
        const double d = static_cast<double>(v);
        u64 bits;
        std::memcpy(&bits, &d, 8);
        out[0] = static_cast<u32>(bits);
        out[1] = static_cast<u32>(bits >> 32);
        return true;
    }
    if (kind == kD2LZ) {
        const u64 bits = r[0] | (u64{r[1]} << 32);
        double d;
        std::memcpy(&d, &bits, 8);
        if (!std::isfinite(d) || std::fabs(d) >= 9.0e18) return false;
        const u64 v = static_cast<u64>(static_cast<s64>(d));
        out[0] = static_cast<u32>(v);
        out[1] = static_cast<u32>(v >> 32);
        return true;
    }
    // memcpy / memmove: returns dst; the destination ends up holding what the source held
    const u32 n = r[2];
    if (n == 0 || n > 65536 || !mem.IsValidVirtualAddressRange(r[0], n) ||
        !mem.IsValidVirtualAddressRange(r[1], n)) {
        return false;
    }
    out[0] = r[0];
    out[1] = r[1];
    if (snap) {
        snap->resize(n);
        mem.ReadBlock(r[1], snap->data(), n);
    }
    return true;
}

bool NativeHelper(int kind, u32* r, Memory::Memory& mem) {
    u32 out[2];
    if (!HelperPredict(kind, r, mem, out, nullptr)) return false;
    if (kind == kMemcpy) {
        thread_local std::vector<u8> buf;
        buf.resize(r[2]);
        mem.ReadBlock(r[1], buf.data(), r[2]);
        mem.WriteBlock(r[0], buf.data(), r[2]);
        return true;  // r0 (dst) is already the result
    }
    r[0] = out[0];
    r[1] = out[1];
    return true;
}
template <int K>
bool NativeHelperK(u32* r, Memory::Memory& mem) {
    return NativeHelper(K, r, mem);
}
const NvnTrace::Replacement kHelperNative[kHelperCount] = {NativeHelperK<kL2D>, NativeHelperK<kD2LZ>,
                                                           NativeHelperK<kMemcpy>};

struct VPending {
    u32 ret_pc, sp;
    int kind;
    u32 args[3];
    u32 pred[2];
    std::vector<u8> snap;
};
struct VStat {
    u64 match{}, mismatch{}, declined{};
    u64 pushed{}, pc_only{}, sp_only{};
    std::vector<std::string> examples;
};
u32 g_game_base{};
u32 g_sdk_base{};
std::vector<VPending> g_vpending;
VStat g_vstat[kHelperCount];

std::mutex g_lock;
std::unordered_map<std::string, NvnTrace::Replacement> g_replacements; // by entry name
std::vector<Pending> g_pending;                // lookups in flight
std::unordered_map<u32, Entry> g_entries;      // guest address -> NVN entry point
std::unordered_map<u32, int> g_lookup_fns;     // address -> which arg holds the name
int PendingCount() {
    return static_cast<int>(g_pending.size() + g_vpending.size()) + NvnCensus::Pending();
}
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

void NvnTrace::Attach(System& system_, const std::map<u64, std::string>& modules, bool lean_) {
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
    lean = lean_;
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
    if (!lean) {
        // probe_pcs: extra guest addresses to observe (nnSdk offsets).
        u32 sdk = 0;
        for (const auto& [base, name] : modules) {
            if (name.find("nnSdk") != std::string::npos) sdk = static_cast<u32>(base);
        }
        std::string list = GameSettings::ProbePcs();
        size_t at = 0;
        while (sdk != 0 && at < list.size()) {
            const size_t comma = list.find(',', at);
            const std::string tok = list.substr(at, comma == std::string::npos ? comma : comma - at);
            at = comma == std::string::npos ? list.size() : comma + 1;
            if (const u32 off = static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 16)); off != 0) {
                std::scoped_lock lk{g_lock};
                g_entries[sdk + off].names.push_back(fmt::format("probe_{:x}", off));
                g_entries[sdk + off].probe = true;
                Watch(sdk + off);
            }
        }
    }
    {
        u32 sdk = 0;
        for (const auto& [base, name] : modules) {
            if (name.find("nnSdk") != std::string::npos) sdk = static_cast<u32>(base);
        }
        g_sdk_base = sdk;
        for (const auto& [base, name] : modules) {
            if (name.find("Rabbit_Ro") != std::string::npos) g_game_base = static_cast<u32>(base);
        }
        for (int k = 0; sdk != 0 && k < kHelperCount; ++k) {
            const u32 addr = sdk + kHelpers[k].sdk_offset;
            const bool native = GameSettings::NativeNvn() && kHelpers[k].proven;
            if (lean) {
                if (native) {
                    const u32 n = fast_count.load(std::memory_order_relaxed);
                    fast[n] = {addr, kHelperNative[k]};
                    fast_count.store(n + 1, std::memory_order_release);
                    Watch(addr);
                }
                continue;
            }
            std::scoped_lock lk{g_lock};
            auto& e = g_entries[addr];
            e.names.push_back(kHelpers[k].name);
            e.helper = k;
            if (native) e.replace = kHelperNative[k];
            Watch(addr);
        }
    }
    NvnShadow::Start();
    if (GameSettings::NativeNvn()) {
        NvnShadow::RegisterNative();
    }
    if (GameSettings::NvnVulkanMode() != 0) {
        NvnRec::RegisterAll();
    } else {
        if (Kernel::StallRing::On()) NvnRec::RegisterPresentProbeOnly(); // Run M: the frame probe runs in suyu-only mode too
    }
    enabled.store(true, std::memory_order_release);
}

bool NvnTrace::HasReplacement(const std::string& name) {
    std::scoped_lock lk{g_lock};
    return g_replacements.contains(name);
}

namespace {
std::string ReplacementNames(uintptr_t fn) {
    std::scoped_lock lk{g_lock};
    std::string out;
    for (const auto& [name, f] : g_replacements) {
        if (reinterpret_cast<uintptr_t>(f) == fn) {
            out += (out.empty() ? "" : "|") + name;
        }
    }
    return out.empty() ? "?" : out;
}

/// Times one native hook call for the wiggle-hitch probe.
struct HookTimer {
    HookTimer(Core::System* sys_, uintptr_t fn_) : sys{sys_}, fn{fn_}, t0{Kernel::StallRing::On() ? Kernel::StallRing::NowMs() : 0.0} {}
    ~HookTimer() {
        if (!Kernel::StallRing::On()) return;
        auto& kernel = sys->Kernel();
        Kernel::StallRing::HookDone(fn, static_cast<u32>(Kernel::GetCurrentThread(kernel).GetThreadId()),
                                    static_cast<u8>(kernel.CurrentPhysicalCore().CoreIndex()), t0,
                                    Kernel::StallRing::NowMs());
    }
    Core::System* sys;
    uintptr_t fn;
    double t0;
};
} // namespace

void NvnTrace::Replace(const std::string& name, Replacement fn) {
    std::scoped_lock lk{g_lock};
    Kernel::StallRing::g_name_of = &ReplacementNames;
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
        if (e.helper >= 0) {
            // Millions of calls a second: no census, no counting; verify only a sample.
            if (!e.replace && (e.calls <= 3000 || e.calls % 1024 == 0) && g_vpending.size() < 32) {
                VPending vp{regs[14] & ~1u, regs[13], e.helper, {regs[0], regs[1], regs[2]}, {}, {}};
                if (HelperPredict(e.helper, regs, mem, vp.pred, &vp.snap)) {
                    ++g_vstat[e.helper].pushed;
                    g_vpending.push_back(std::move(vp));
                    pending_returns.store(PendingCount(), std::memory_order_relaxed);
                } else {
                    ++g_vstat[e.helper].declined;
                }
            }
            replace = e.replace;
            goto helper_done;
        }
        if (e.probe) {
            auto& ps = g_probe[pc];
            ++ps.hits;
            ++ps.callers[regs[14]];
            if (ps.samples.size() < 40 && (ps.hits <= 16 || ps.hits % 50000 == 0)) {
                ps.samples.push_back(fmt::format("hit {} r0-r3 {:x} {:x} {:x} {:x} r12 {:x} sp {:x} lr {:x}", ps.hits,
                                                 regs[0], regs[1], regs[2], regs[3], regs[12], regs[13], regs[14]));
            }
        }
        if (e.observe) {
            e.observe(regs, mem);
        }
        replace = e.replace;
        if (e.census && !replace && !e.names.empty() &&
            NvnCensus::Begin(e.names.front(), e.calls - 1, regs, mem)) {
            pending_returns.store(PendingCount(),
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
helper_done:
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
        pending_returns.store(PendingCount(),
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
        HookTimer timer{system, reinterpret_cast<uintptr_t>(replace)};
        if (replace(regs, system->ApplicationMemory())) {
            regs[15] = regs[14] & ~1u; // return to the caller (ARM state only in MHGU)
            return true;
        }
    }
    return false;
}

bool NvnTrace::RunFast(Replacement fn, u32* regs) {
    HookTimer timer{system, reinterpret_cast<uintptr_t>(fn)};
    if (!fn(regs, system->ApplicationMemory())) {
        return false;
    }
    regs[15] = regs[14] & ~1u; // return to the caller (ARM state only in MHGU)
    return true;
}

void NvnTrace::OnMaybeReturn(u32 pc, const u32* regs) {
    std::scoped_lock lk{g_lock};
    for (auto vit = g_vpending.begin(); vit != g_vpending.end(); ++vit) {
        if (vit->ret_pc != pc || vit->sp != regs[13]) {
            if (vit->ret_pc == pc) ++g_vstat[vit->kind].pc_only;
            else if (vit->sp == regs[13]) ++g_vstat[vit->kind].sp_only;
            continue;
        }
        auto& st = g_vstat[vit->kind];
        bool ok = regs[0] == vit->pred[0];
        if (vit->kind == kMemcpy) {
            std::vector<u8> got(vit->snap.size());
            system->ApplicationMemory().ReadBlock(vit->args[0], got.data(), got.size());
            ok = ok && got == vit->snap;
        } else {
            ok = ok && regs[1] == vit->pred[1];
        }
        if (ok) {
            ++st.match;
        } else {
            ++st.mismatch;
            if (st.examples.size() < 8) {
                st.examples.push_back(fmt::format("  MISMATCH args {:x} {:x} {:x} predicted {:x} {:x} got {:x} {:x}",
                                                  vit->args[0], vit->args[1], vit->args[2], vit->pred[0],
                                                  vit->pred[1], regs[0], regs[1]));
            }
        }
        g_vpending.erase(vit);
        pending_returns.store(PendingCount(), std::memory_order_relaxed);
        break;
    }
    if (NvnCensus::Pending() != 0) {
        const bool ended = NvnCensus::End(pc, regs, system->ApplicationMemory());
        // Also after an expiry: End may have dropped samples whose return never came.
        pending_returns.store(PendingCount(),
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
    if (fn != 0 && lean && !EndsWith(it->name, "GetProcAddress")) {
        // Lean mode: only entry points with a native replacement are hooked at all.
        if (const auto r = g_replacements.find(it->name); r != g_replacements.end()) {
            const u32 n = fast_count.load(std::memory_order_relaxed);
            bool known = false;
            for (u32 i = 0; i < n; ++i) known = known || fast[i].pc == fn;
            if (!known && n < kMaxFast) {
                fast[n] = {fn, r->second};
                fast_count.store(n + 1, std::memory_order_release);
                Watch(fn);
            }
        }
    } else if (fn != 0) {
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
    pending_returns.store(PendingCount(),
                          std::memory_order_relaxed);
}

void NvnTrace::Flush(bool final) {
    if (lean) {
        // Lean mode writes one small proof-of-life file every 10 s and at exit.
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        if (final || now - last > std::chrono::seconds(10)) {
            last = now;
            std::ofstream out(UserFile("nvn_native.txt"), std::ios::trunc);
            if (GameSettings::NvnVulkanMode() != 0) NvnRec::Flush(UserFile("nvn_rec.txt"));
            out << "native NVN calls served: " << NvnShadow::NativeCalls() << '\n';
        }
        return;
    }
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
        std::scoped_lock lk{g_lock};
        std::ofstream hv(UserFile("nvn_helpers.txt"), std::ios::trunc);
        for (int k = 0; k < kHelperCount; ++k) {
            hv << fmt::format("{}: verify {} match, {} MISMATCH, {} declined; started {}, return seen with pc only {}, sp only {}, pending now {}{}", kHelpers[k].name, g_vstat[k].match,
                              g_vstat[k].mismatch, g_vstat[k].declined, g_vstat[k].pushed, g_vstat[k].pc_only, g_vstat[k].sp_only, g_vpending.size(), char(10));
            for (const auto& ex : g_vstat[k].examples) hv << ex << char(10);
        }
        static const struct { const char* name; u32 slot; } kSlots[] = {
            {"int64_to_double stub slot", 0x1838294}, {"double_to_int64 stub slot", 0x183861c},
            {"memcpy stub A slot", 0x1838054}, {"memcpy stub B slot", 0x1838034}};
        auto& m = system->ApplicationMemory();
        for (const auto& sl : kSlots) {
            const u32 v = g_game_base ? m.Read32(g_game_base + sl.slot) : 0;
            hv << fmt::format("{}: points to {:08x} = nnSdk+{:x} (game base {:x}, nnSdk base {:x}){}", sl.name, v,
                              v - g_sdk_base, g_game_base, g_sdk_base, char(10));
        }
    }
    if (!g_probe.empty()) {
        std::scoped_lock lk{g_lock};
        std::ofstream pp(UserFile("nvn_pcprobe.txt"), std::ios::trunc);
        auto& mem = system->ApplicationMemory();
        for (const auto& [pc, ps] : g_probe) {
            pp << fmt::format("## {:08x}: {} hits, {} distinct callers (return address: count, instruction before it){}", pc,
                              ps.hits, ps.callers.size(), char(10));
            std::vector<std::pair<u64, u32>> top;
            for (const auto& [lr, n] : ps.callers) top.emplace_back(n, lr);
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
                const u32 lr = top[i].second;
                const u32 insn = lr >= 4 ? mem.Read32(lr - 4) : 0;
                std::string target;
                if ((insn >> 24 & 0xf) == 0xb) {  // BL: where the call went
                    const s32 imm = static_cast<s32>(insn << 8) >> 6;
                    target = fmt::format(" = BL {:08x}", lr - 4 + 8 + static_cast<u32>(imm));
                }
                pp << fmt::format("  {:08x}: {} {:08x}{}{}", lr, top[i].first, insn, target, char(10));
            }
            for (const auto& line : ps.samples) pp << "  " << line << char(10);
        }
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
