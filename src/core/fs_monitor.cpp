// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "core/fs_monitor.h"

namespace Core::FsMonitor {

namespace {

constexpr double kBurstGap = 0.050;    // reads closer than this belong to one burst
constexpr double kHitch = 0.050;       // a frame longer than this is a hitch
constexpr size_t kMaxRanges = 48;      // offset ranges kept per burst
constexpr size_t kMaxBursts = 4000;    // most recent bursts kept
constexpr size_t kMaxHitches = 2000;

struct Range {
    u64 offset;
    u64 length;
};

struct Burst {
    u64 id;
    double start;
    double end;
    u64 reads;
    u64 bytes;
    double read_s;
    std::vector<Range> ranges;
    u64 dropped_ranges;
};

struct Hitch {
    double at;
    double gap_ms;
    s64 burst; // id of the burst that overlapped the frame, -1 if none
    std::string guest; // what the guest ran meanwhile (HitchProbe)
};

std::atomic<bool> g_enabled{false};
std::atomic<HitchProbe> g_probe{nullptr};
std::mutex g_lock;
const auto g_start = std::chrono::steady_clock::now();
std::deque<Burst> g_bursts;
std::deque<Hitch> g_hitches;
u64 g_next_burst = 0;
u64 g_reads = 0, g_bytes = 0, g_frames = 0, g_hitch_count = 0;
double g_read_s = 0, g_last_frame = -1;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

void WriteLocked() {
    const auto path =
        Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "fs_monitor.txt";
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# file load monitor: RomFS reads grouped into bursts (gaps under 50 ms), frames over "
           "50 ms as hitches. Offsets are RomFS offsets (tools/a32recomp/fs_monitor_map.py).\n";
    out << fmt::format("# uptime {:.1f} s: {} reads, {:.1f} MB, {:.2f} s reading, {} bursts, "
                       "{} frames, {} hitches\n",
                       Now(), g_reads, g_bytes / 1048576.0, g_read_s, g_next_burst, g_frames,
                       g_hitch_count);
    out << "\n## hitches: at_s\tframe_ms\tburst\tguest during the frame (core busy %, hottest "
           "blocks: samples idx:offset)\n";
    for (const Hitch& h : g_hitches) {
        out << fmt::format("{:.2f}\t{:.1f}\t{}\t{}\n", h.at, h.gap_ms,
                           h.burst < 0 ? std::string{"-"} : std::to_string(h.burst), h.guest);
    }
    out << "\n## bursts: id\tstart_s\twall_ms\treads\tKB\tread_ms\tranges (offset+length, hex)\n";
    for (const Burst& b : g_bursts) {
        out << fmt::format("{}\t{:.2f}\t{:.1f}\t{}\t{}\t{:.1f}\t", b.id, b.start,
                           (b.end - b.start) * 1000.0, b.reads, b.bytes / 1024,
                           b.read_s * 1000.0);
        for (const Range& r : b.ranges) {
            out << fmt::format("{:x}+{:x} ", r.offset, r.length);
        }
        if (b.dropped_ranges != 0) {
            out << fmt::format("(+{} more)", b.dropped_ranges);
        }
        out << "\n";
    }
}

void StartTimer() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                std::scoped_lock lk{g_lock};
                WriteLocked();
            }
        }).detach();
    });
}

} // namespace

void Enable() {
    g_enabled.store(true, std::memory_order_relaxed);
    StartTimer();
}

void OnRead(u64 offset, u64 length, double seconds) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const double now = Now();
    std::scoped_lock lk{g_lock};
    ++g_reads;
    g_bytes += length;
    g_read_s += seconds;
    if (g_bursts.empty() || now - seconds - g_bursts.back().end > kBurstGap) {
        g_bursts.push_back({g_next_burst++, now - seconds, now, 0, 0, 0.0, {}, 0});
        if (g_bursts.size() > kMaxBursts) {
            g_bursts.pop_front();
        }
    }
    Burst& b = g_bursts.back();
    b.end = now;
    ++b.reads;
    b.bytes += length;
    b.read_s += seconds;
    if (!b.ranges.empty() && b.ranges.back().offset + b.ranges.back().length == offset) {
        b.ranges.back().length += length;
    } else if (b.ranges.size() < kMaxRanges) {
        b.ranges.push_back({offset, length});
    } else {
        ++b.dropped_ranges;
    }
}

void SetHitchProbe(HitchProbe probe) {
    g_probe.store(probe);
}

void OnFrame() {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now_tp = std::chrono::steady_clock::now();
    const double now = std::chrono::duration<double>(now_tp - g_start).count();
    std::scoped_lock lk{g_lock};
    ++g_frames;
    if (g_last_frame >= 0 && now - g_last_frame > kHitch) {
        ++g_hitch_count;
        s64 burst = -1;
        for (auto it = g_bursts.rbegin(); it != g_bursts.rend(); ++it) {
            if (it->end < g_last_frame - kBurstGap) {
                break;
            }
            if (it->start <= now) {
                burst = static_cast<s64>(it->id);
                break;
            }
        }
        std::string guest;
        if (const auto probe = g_probe.load()) {
            guest = probe(g_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::duration<double>(g_last_frame)),
                          now_tp);
        }
        g_hitches.push_back({now, (now - g_last_frame) * 1000.0, burst, std::move(guest)});
        if (g_hitches.size() > kMaxHitches) {
            g_hitches.pop_front();
        }
    }
    g_last_frame = now;
}

void Flush() {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lk{g_lock};
    WriteLocked();
}

} // namespace Core::FsMonitor
