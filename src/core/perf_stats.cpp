// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <functional>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include <fmt/chrono.h>
#include <fmt/ranges.h>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/arm/recomp/game_settings.h"
#include "core/arm/recomp/mod_host.h"
#include "core/fs_monitor.h"
#include "core/perf_stats.h"

using namespace std::chrono_literals;
using DoubleSecs = std::chrono::duration<double, std::chrono::seconds::period>;
using std::chrono::duration_cast;
using std::chrono::microseconds;

// Purposefully ignore the first five frames, as there's a significant amount of overhead in
// booting that we shouldn't account for
constexpr std::size_t IgnoreFrames = 5;

namespace Core {

PerfStats::PerfStats(u64 title_id_) : title_id(title_id_) {}

PerfStats::~PerfStats() {
    if (!Settings::values.record_frame_times || title_id == 0) {
        return;
    }

    const std::time_t t = std::time(nullptr);
    std::ostringstream stream;
    std::copy(perf_history.begin() + IgnoreFrames, perf_history.begin() + current_index,
              std::ostream_iterator<double>(stream, "\n"));

    const auto path = Common::FS::GetSuyuPath(Common::FS::SuyuPath::LogDir);
    // %F Date format expanded is "%Y-%m-%d"
    const auto filename = fmt::format("{}_{:016X}.csv",
        [&] {
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&t), "%F-%H-%M");
            return oss.str();
        }(),
        title_id);

    const auto filepath = path / filename;

    if (Common::FS::CreateParentDir(filepath)) {
        Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::TextFile);
        void(file.WriteString(stream.str()));
    }
}

void PerfStats::BeginSystemFrame() {
    std::scoped_lock lock{object_mutex};

    frame_begin = Clock::now();
}

namespace {
using Clock = std::chrono::steady_clock;
// user/frame_stats.txt: renderer-independent A/B metric. One S line per second (frames, frame
// interval avg/p99/max in ms, process CPU % of one core), and frame_stats_total.txt rewritten every
// 10 s with whole-run avg fps and 1% / 0.1% lows. Same code runs for the suyu renderer
// (nvn_vulkan = off) and ours (present), so the numbers compare directly.
struct FrameStats {
    std::vector<double> run;
    std::vector<double> sec;
    Clock::time_point sec_start{};
    Clock::time_point run_start{};
    double cpu_prev_s = 0.0;
    int s_since_total = 0;
    Common::FS::IOFile log;
    bool open = false;

    static double ProcessCpuSeconds() {
#ifdef _WIN32
        FILETIME c, e, k, u;
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            return 0.0;
        }
        const auto to_s = [](const FILETIME& f) {
            return static_cast<double>((static_cast<u64>(f.dwHighDateTime) << 32) |
                                       f.dwLowDateTime) / 1e7;
        };
        return to_s(k) + to_s(u);
#else
        return 0.0;
#endif
    }

    static double Pct(std::vector<double> v, double p) {
        if (v.empty()) {
            return 0.0;
        }
        const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
        std::nth_element(v.begin(), v.begin() + i, v.end());
        return v[i];
    }

    void Add(Clock::time_point now, double interval_ms) {
        if (!open) {
            const auto dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir);
            log = Common::FS::IOFile(dir / "frame_stats.txt", Common::FS::FileAccessMode::Write,
                                     Common::FS::FileType::TextFile);
            open = true;
            sec_start = run_start = now;
            cpu_prev_s = ProcessCpuSeconds();
            void(log.WriteString(fmt::format(
                "# frame_stats: nvn_vulkan mode {} suyu_render {}\n"
                "# S = per second: frames, interval ms avg/p99/max, process cpu pct of one core\n",
                GameSettings::NvnVulkanMode(), GameSettings::SuyuRender() ? 1 : 0)));
            return;
        }
        sec.push_back(interval_ms);
        run.push_back(interval_ms);
        const double el = std::chrono::duration<double>(now - sec_start).count();
        if (el < 1.0) {
            return;
        }
        double sum = 0.0, mx = 0.0;
        for (double d : sec) {
            sum += d;
            mx = std::max(mx, d);
        }
        const double cpu = ProcessCpuSeconds();
        void(log.WriteString(fmt::format("S t {:.0f} frames {} avg {:.2f} p99 {:.2f} max {:.2f} cpu {:.0f}\n",
                                         std::chrono::duration<double>(now - run_start).count(),
                                         sec.size(), sum / static_cast<double>(sec.size()),
                                         Pct(sec, 0.99), mx, (cpu - cpu_prev_s) / el * 100.0)));
        void(log.Flush());
        cpu_prev_s = cpu;
        sec.clear();
        sec_start = now;
        if (++s_since_total >= 10) {
            s_since_total = 0;
            double total = 0.0;
            for (double d : run) {
                total += d;
            }
            const double avg = total / static_cast<double>(run.size());
            // 1% low = fps of the average of the slowest 1% of frames
            std::vector<double> sorted = run;
            std::sort(sorted.begin(), sorted.end(), std::greater<>());
            const auto low = [&](double frac) {
                const size_t n = std::max<size_t>(1, static_cast<size_t>(frac * static_cast<double>(sorted.size())));
                double s = 0.0;
                for (size_t i = 0; i < n; ++i) {
                    s += sorted[i];
                }
                return 1000.0 / (s / static_cast<double>(n));
            };
            const auto dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir);
            Common::FS::IOFile t(dir / "frame_stats_total.txt", Common::FS::FileAccessMode::Write,
                                 Common::FS::FileType::TextFile);
            void(t.WriteString(fmt::format(
                "mode {} suyu_render {}\nframes {}\navg_ms {:.2f} avg_fps {:.1f}\np99_ms {:.2f} p999_ms {:.2f} max_ms {:.2f}\n"
                "low1pct_fps {:.1f} low01pct_fps {:.1f}\nframes_over_25ms {}\nframes_over_50ms {}\n",
                GameSettings::NvnVulkanMode(), GameSettings::SuyuRender() ? 1 : 0, run.size(), avg,
                1000.0 / avg, Pct(run, 0.99), Pct(run, 0.999), sorted.front(), low(0.01), low(0.001),
                std::count_if(run.begin(), run.end(), [](double d) { return d > 25.0; }),
                std::count_if(run.begin(), run.end(), [](double d) { return d > 50.0; }))));
        }
    }
};
} // namespace

void PerfStats::EndSystemFrame() {
    FsMonitor::OnFrame();
    GameSettings::OnFrame();
    ModHost::OnFrame();
    std::scoped_lock lock{object_mutex};

    auto frame_end = Clock::now();
    {
        static FrameStats fs;
        if (previous_frame_end.time_since_epoch().count() != 0) {
            fs.Add(frame_end, std::chrono::duration<double, std::milli>(frame_end - previous_frame_end).count());
        } else {
            fs.Add(frame_end, 0.0);
        }
    }
    const auto frame_time = frame_end - frame_begin;
    if (current_index < perf_history.size()) {
        perf_history[current_index++] =
            std::chrono::duration<double, std::milli>(frame_time).count();
    }
    accumulated_frametime += frame_time;
    system_frames += 1;

    previous_frame_length = frame_end - previous_frame_end;
    previous_frame_end = frame_end;
}

void PerfStats::EndGameFrame() {
    game_frames.fetch_add(1, std::memory_order_relaxed);
}

double PerfStats::GetMeanFrametime() const {
    std::scoped_lock lock{object_mutex};

    if (current_index <= IgnoreFrames) {
        return 0;
    }

    const double sum = std::accumulate(perf_history.begin() + IgnoreFrames,
                                       perf_history.begin() + current_index, 0.0);
    return sum / static_cast<double>(current_index - IgnoreFrames);
}

PerfStatsResults PerfStats::GetAndResetStats(microseconds current_system_time_us) {
    std::scoped_lock lock{object_mutex};

    const auto now = Clock::now();
    // Walltime elapsed since stats were reset
    const auto interval = duration_cast<DoubleSecs>(now - reset_point).count();

    const auto system_us_per_second = (current_system_time_us - reset_point_system_us) / interval;
    const auto current_frames = static_cast<double>(game_frames.load(std::memory_order_relaxed));
    const auto current_fps = current_frames / interval;
    const PerfStatsResults results{
        .system_fps = static_cast<double>(system_frames) / interval,
        .average_game_fps = (current_fps + previous_fps) / 2.0,
        .frametime = duration_cast<DoubleSecs>(accumulated_frametime).count() /
                     static_cast<double>(system_frames),
        .emulation_speed = system_us_per_second.count() / 1'000'000.0,
    };

    // Reset counters
    reset_point = now;
    reset_point_system_us = current_system_time_us;
    accumulated_frametime = Clock::duration::zero();
    system_frames = 0;
    game_frames.store(0, std::memory_order_relaxed);
    previous_fps = current_fps;

    return results;
}

double PerfStats::GetLastFrameTimeScale() const {
    std::scoped_lock lock{object_mutex};

    constexpr double FRAME_LENGTH = 1.0 / 60;
    return duration_cast<DoubleSecs>(previous_frame_length).count() / FRAME_LENGTH;
}

void SpeedLimiter::DoSpeedLimiting(microseconds current_system_time_us) {
    if (Settings::values.use_multi_core.GetValue() ||
        !Settings::values.use_speed_limit.GetValue()) {
        return;
    }

    auto now = Clock::now();

    const double sleep_scale = Settings::SpeedLimit() / 100.0;

    // Max lag caused by slow frames. Shouldn't be more than the length of a frame at the current
    // speed percent or it will clamp too much and prevent this from properly limiting to that
    // percent. High values means it'll take longer after a slow frame to recover and start
    // limiting
    const microseconds max_lag_time_us = duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(25ms / sleep_scale));
    speed_limiting_delta_err += duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(
            (current_system_time_us - previous_system_time_us) / sleep_scale));
    speed_limiting_delta_err -= duration_cast<microseconds>(now - previous_walltime);
    speed_limiting_delta_err =
        std::clamp(speed_limiting_delta_err, -max_lag_time_us, max_lag_time_us);

    if (speed_limiting_delta_err > microseconds::zero()) {
        std::this_thread::sleep_for(speed_limiting_delta_err);
        auto now_after_sleep = Clock::now();
        speed_limiting_delta_err -= duration_cast<microseconds>(now_after_sleep - now);
        now = now_after_sleep;
    }

    previous_system_time_us = current_system_time_us;
    previous_walltime = now;
}

} // namespace Core
