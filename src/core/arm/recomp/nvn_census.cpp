// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "core/arm/recomp/nvn_census.h"
#include "core/memory.h"

namespace Core::NvnCensus {

namespace {

constexpr u32 kObjectWords = 64;     // command buffer object words compared around a call
constexpr u32 kMaxCallBytes = 8192;  // larger advances are a jump to a new chunk
constexpr u64 kFirstCalls = 512;     // every call is sampled at first, then 1 in kRate
constexpr u64 kRate = 256;
constexpr size_t kMaxShapes = 24;    // distinct method sequences kept per entry point
constexpr size_t kExamples = 2;
constexpr u32 kArgs = 7;             // r0-r3, then the first three stack words

struct Sample {
    u32 ret_pc;
    u32 sp;
    std::chrono::steady_clock::time_point start;
    std::string name;
    std::array<u32, kArgs> args;
    std::array<u32, kObjectWords> before;
};

/// One method write: subchannel << 16 | method (in words, as dma_pusher numbers them).
using MethodSeq = std::vector<u32>;

struct Shape {
    u64 count{};
    std::vector<u32> first;     // value seen first, per method write
    std::vector<u8> varies;     // value differed between calls
    std::vector<u8> arg_match;  // bit a: value == args[a] in every call so far
    std::vector<std::pair<std::array<u32, kArgs>, std::vector<u32>>> examples;
};

struct Fn {
    u64 sampled{};
    u64 decoded{};
    u64 no_advance{};  // write pointer didn't move (state kept for later, or another buffer)
    u64 unreadable{};
    u32 min_words{~0u};
    u32 max_words{};
    std::map<MethodSeq, Shape> shapes;
    u64 shapes_dropped{};
};

std::vector<Sample> g_pending;
u64 g_end_checks{};
u64 g_expired{};
// A sample whose return never shows up (the call left by a long jump, or returned somewhere
// else) would keep every dispatch checking for it: give up on it after this long.
constexpr auto kSampleTimeout = std::chrono::milliseconds(200);
std::map<std::string, Fn> g_fns;
std::array<u64, kObjectWords> g_votes{};
int g_ptr_word = -1;

int Winner() {
    if (g_ptr_word >= 0) {
        return g_ptr_word;
    }
    u32 best = 0, second = 0;
    int at = -1;
    for (u32 k = 0; k < kObjectWords; ++k) {
        if (g_votes[k] > best) {
            second = static_cast<u32>(best);
            best = static_cast<u32>(g_votes[k]);
            at = static_cast<int>(k);
        } else if (g_votes[k] > second) {
            second = static_cast<u32>(g_votes[k]);
        }
    }
    if (best >= 64 && best >= 2 * second) {
        g_ptr_word = at;
    }
    return g_ptr_word;
}

/// Pushbuffer words -> method writes (formats as in video_core/dma_pusher.h).
void Decode(const std::vector<u32>& w, MethodSeq& seq, std::vector<u32>& values) {
    for (size_t i = 0; i < w.size();) {
        const u32 h = w[i];
        const u32 method = h & 0x1fff;
        const u32 sc = (h >> 13) & 7;
        const u32 count = (h >> 16) & 0x1fff;
        const u32 mode = h >> 29;
        const auto put = [&](u32 m, u32 v) {
            seq.push_back(sc << 16 | m);
            values.push_back(v);
        };
        if (mode == 4) { // inline: the value is in the header
            put(method, count);
            ++i;
            continue;
        }
        if (mode == 6 || mode == 7 || (count > 0 && i + count >= w.size())) {
            seq.push_back(0xffffffff); // not a header we know: stop here
            values.push_back(h);
            return;
        }
        for (u32 j = 0; j < count; ++j) {
            u32 m = method;
            if (mode == 0 || mode == 1) {
                m = method + j;
            } else if (mode == 5) {
                m = method + (j == 0 ? 0 : 1);
            }
            put(m, w[i + 1 + j]);
        }
        i += 1 + count;
    }
}

void Record(Fn& fn, const Sample& s, const MethodSeq& seq, const std::vector<u32>& values) {
    auto it = fn.shapes.find(seq);
    if (it == fn.shapes.end()) {
        if (fn.shapes.size() >= kMaxShapes) {
            ++fn.shapes_dropped;
            return;
        }
        Shape sh;
        sh.first = values;
        sh.varies.assign(values.size(), 0);
        sh.arg_match.assign(values.size(), 0xff);
        it = fn.shapes.emplace(seq, std::move(sh)).first;
    }
    Shape& sh = it->second;
    ++sh.count;
    for (size_t k = 0; k < values.size(); ++k) {
        if (values[k] != sh.first[k]) {
            sh.varies[k] = 1;
        }
        u8 match = 0;
        for (u32 a = 1; a < kArgs; ++a) { // r0 is the command buffer itself
            if (values[k] == s.args[a]) {
                match |= static_cast<u8>(1u << a);
            }
        }
        sh.arg_match[k] &= match;
    }
    if (sh.examples.size() < kExamples) {
        sh.examples.emplace_back(s.args, values);
    }
}

} // namespace

bool Wants(const std::string& name) {
    return name.starts_with("nvnCommandBuffer") && name != "nvnCommandBufferGetMemoryCallback" &&
           name != "nvnCommandBufferGetMemoryCallbackData";
}

bool Begin(const std::string& name, u64 call, const u32* regs, Memory::Memory& mem) {
    if (call >= kFirstCalls && call % kRate != 0) {
        return false;
    }
    const u32 cmd = regs[0];
    if (cmd == 0 || !mem.IsValidVirtualAddressRange(cmd, kObjectWords * 4) ||
        !mem.IsValidVirtualAddressRange(regs[13], 12)) {
        return false;
    }
    Sample s{regs[14] & ~1u, regs[13], std::chrono::steady_clock::now(), name, {}, {}};
    for (u32 a = 0; a < 4; ++a) {
        s.args[a] = regs[a];
    }
    for (u32 a = 4; a < kArgs; ++a) {
        s.args[a] = mem.Read32(regs[13] + 4 * (a - 4));
    }
    mem.ReadBlock(cmd, s.before.data(), kObjectWords * 4);
    g_pending.push_back(std::move(s));
    return true;
}

bool End(u32 pc, const u32* regs, Memory::Memory& mem) {
    // The call has returned when its return address runs with the caller's stack pointer.
    const auto it = std::find_if(g_pending.begin(), g_pending.end(), [&](const Sample& s) {
        return s.ret_pc == pc && s.sp == regs[13];
    });
    if (it == g_pending.end()) {
        if ((++g_end_checks & 1023) == 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto old = std::remove_if(g_pending.begin(), g_pending.end(),
                                            [&](const Sample& p) { return now - p.start > kSampleTimeout; });
            g_expired += static_cast<u64>(g_pending.end() - old);
            g_pending.erase(old, g_pending.end());
        }
        return false;
    }
    const Sample s = std::move(*it);
    g_pending.erase(it);

    Fn& fn = g_fns[s.name];
    ++fn.sampled;
    std::array<u32, kObjectWords> after{};
    mem.ReadBlock(s.args[0], after.data(), kObjectWords * 4);
    for (u32 k = 0; k < kObjectWords; ++k) {
        const u32 d = after[k] - s.before[k];
        if (d != 0 && d <= kMaxCallBytes && (d & 3) == 0) {
            ++g_votes[k];
        }
    }
    const int ptr = Winner();
    if (ptr < 0) {
        return true;
    }
    const u32 from = s.before[ptr];
    const u32 bytes = after[ptr] - from;
    if (bytes == 0 || bytes > kMaxCallBytes || (bytes & 3) != 0) {
        ++fn.no_advance;
        return true;
    }
    if (!mem.IsValidVirtualAddressRange(from, bytes)) {
        ++fn.unreadable;
        return true;
    }
    std::vector<u32> words(bytes / 4);
    mem.ReadBlock(from, words.data(), bytes);
    MethodSeq seq;
    std::vector<u32> values;
    Decode(words, seq, values);
    ++fn.decoded;
    fn.min_words = std::min(fn.min_words, bytes / 4);
    fn.max_words = std::max(fn.max_words, bytes / 4);
    Record(fn, s, seq, values);
    return true;
}

int Pending() {
    return static_cast<int>(g_pending.size());
}

void Flush(const std::filesystem::path& file) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# NVN method census: GPU methods the guest driver records per command buffer call\n";
    out << "# method writes are sc.method(hex, in words)=value; aN = always argument N "
           "(1-3 = r1-r3, 4-6 = stack words), * = varies, otherwise the constant seen\n";
    out << fmt::format("# write pointer: command buffer word {} (votes:", g_ptr_word);
    std::vector<std::pair<u64, u32>> votes;
    for (u32 k = 0; k < kObjectWords; ++k) {
        if (g_votes[k] != 0) {
            votes.emplace_back(g_votes[k], k);
        }
    }
    std::sort(votes.rbegin(), votes.rend());
    for (size_t i = 0; i < std::min<size_t>(votes.size(), 6); ++i) {
        out << fmt::format(" w{}={}", votes[i].second, votes[i].first);
    }
    out << fmt::format("); samples given up (no return seen): {}\n\n", g_expired);
    for (const auto& [name, fn] : g_fns) {
        out << fmt::format("## {}: {} sampled, {} decoded, {} without commands, {} unreadable, "
                           "words/call {}-{}, {} shapes{}\n",
                           name, fn.sampled, fn.decoded, fn.no_advance, fn.unreadable,
                           fn.decoded ? fn.min_words : 0, fn.max_words, fn.shapes.size(),
                           fn.shapes_dropped ? fmt::format(" (+{} calls in dropped shapes)",
                                                           fn.shapes_dropped)
                                             : "");
        std::vector<std::pair<u64, const MethodSeq*>> order;
        for (const auto& [seq, sh] : fn.shapes) {
            order.emplace_back(sh.count, &seq);
        }
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [count, seq] : order) {
            const Shape& sh = fn.shapes.at(*seq);
            out << fmt::format("  {}x:", count);
            for (size_t k = 0; k < seq->size(); ++k) {
                const u32 m = (*seq)[k];
                if (m == 0xffffffff) {
                    out << fmt::format(" ?{:08x}", sh.first[k]);
                    continue;
                }
                out << fmt::format(" {}.{:03x}=", m >> 16, m & 0xffff);
                if (sh.arg_match[k] != 0) {
                    for (u32 a = 1; a < kArgs; ++a) {
                        if (sh.arg_match[k] & (1u << a)) {
                            out << fmt::format("a{}", a);
                            break;
                        }
                    }
                } else if (sh.varies[k]) {
                    out << "*";
                } else {
                    out << fmt::format("{:x}", sh.first[k]);
                }
            }
            out << "\n";
            for (const auto& [args, values] : sh.examples) {
                out << fmt::format("    e.g. args {:08x} {:08x} {:08x} | {:08x} {:08x} {:08x} ->",
                                   args[1], args[2], args[3], args[4], args[5], args[6]);
                for (const u32 v : values) {
                    out << fmt::format(" {:x}", v);
                }
                out << "\n";
            }
        }
        out << "\n";
    }
}

} // namespace Core::NvnCensus
