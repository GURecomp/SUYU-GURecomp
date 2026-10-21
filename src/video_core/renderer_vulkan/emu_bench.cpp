// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frame-time bench for the Emu Renderer (renderer = Emulated, NvnVulkanMode() == 0), where no nvn_vk Renderer exists.
// [Debug] nvn_bench = 1 turns it on; the keys and ini values are the ones of the replacement renderer's bench:
//   Numpad * starts a run, Numpad - aborts it (or starts, when idle), Numpad + cycles the scene label while idle,
//   nvn_bench_scene / nvn_bench_warmup (1-30, default 3) / nvn_bench_measure (5-120, default 15).
// One run = warm-up seconds + measure seconds of one state "emu". Each second of the run writes one S line to
// user\emu_perf.txt in the key=value format of nvn_perf.txt, so tools\perf-summary.ps1 reads it unchanged.
// Wall time of a frame = time between two Composite calls. Nothing here touches the render path beyond a QPC read.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/recomp/game_settings.h"
#include "video_core/renderer_vulkan/emu_bench.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Vulkan {
namespace {

using Clock = std::chrono::steady_clock;

struct EmuBench {
    bool init_done{};
    bool enabled{};
    int warm{3};
    int measure{15};
    std::string scene{"village_stand"};

    bool active{};
    int run{};
    int sec{-1}; // -1 = partial second after the key ('p'), then warm-up 'w', then measure 'm'
    bool key_down[3]{};
    std::ofstream out;

    Clock::time_point sec_start{};
    Clock::time_point last_frame{};
    Clock::time_point t0{};
    bool have_last{};
    unsigned long long frames{};
    std::vector<float> wall;

    void Init() {
        init_done = true;
        const auto flag = [](const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; };
        if (!flag(Core::GameSettings::Value("Debug", "nvn_bench"))) return;
        const auto num = [](const char* key, int def, int lo, int hi) {
            const std::string v = Core::GameSettings::Value("Debug", key);
            if (v.empty()) return def;
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            return (end == v.c_str() || *end != 0) ? def : static_cast<int>(std::clamp<long>(n, lo, hi));
        };
        warm = num("nvn_bench_warmup", 3, 1, 30);
        measure = num("nvn_bench_measure", 15, 5, 120);
        std::string sc = Core::GameSettings::Value("Debug", "nvn_bench_scene");
        for (char& c : sc) {
            if (c == ' ' || c == '=' || c == ':') c = '_';
        }
        scene = sc.empty() ? "village_stand" : sc;
        enabled = true;
        t0 = Clock::now();
        LOG_INFO(Render_Vulkan, "EMU BENCH: ON (Emu Renderer). Numpad * or Numpad - starts / aborts a run, Numpad + cycles the scene (now '{}'). "
                                "{} s warm-up + {} s measure, results in emu_perf.txt",
                 scene, warm, measure);
    }

    static double Pct(const std::vector<float>& s, double p) {
        if (s.empty()) return 0.0;
        const size_t i = std::min(s.size() - 1, static_cast<size_t>(p * static_cast<double>(s.size())));
        return s[i];
    }

    void Line(const std::string& s) {
        if (!out.is_open()) {
            out.open(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "emu_perf.txt", std::ios::trunc);
            out << "# emu_perf: renderer=emu (Emu Renderer, renderer = Emulated). S = one-second summary of frame wall time "
                   "(time between Composite calls), same keys as nvn_perf.txt; read it with tools\\perf-summary.ps1 -Perf <this file>.\n";
        }
        out << s << '\n';
        out.flush();
    }

    void Finish(bool aborted) {
        active = false;
        wall.clear();
        Line(fmt::format("B ev={} run={} scene={}", aborted ? "abort" : "done", run, scene));
#ifdef _WIN32
        MessageBeep(MB_OK);
#endif
        if (aborted) {
            LOG_INFO(Render_Vulkan, "EMU BENCH ABORTED (run {}, scene {}); partial results in emu_perf.txt", run, scene);
        } else {
            LOG_INFO(Render_Vulkan, "EMU BENCH DONE (run {}, scene {}, {} s measured); results in emu_perf.txt", run, scene, measure);
        }
    }

    void Poll(Clock::time_point now) {
#ifdef _WIN32
        const auto pressed = [](int key, bool& was) {
            const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
            const bool hit = down && !was;
            was = down;
            return hit;
        };
        if (pressed(VK_ADD, key_down[1]) && !active) {
            static const char* const kScenes[] = {"village_stand", "village_move", "quest"};
            size_t i = 0;
            while (i < 3 && scene != kScenes[i]) ++i;
            scene = kScenes[(i + 1) % 3];
            LOG_INFO(Render_Vulkan, "EMU BENCH scene = {} (Numpad * or Numpad - starts the run)", scene);
        }
        const bool start_mul = pressed(VK_MULTIPLY, key_down[0]);
        const bool start_sub = pressed(VK_SUBTRACT, key_down[2]);
        if (start_mul || start_sub) {
            if (active) {
                Finish(true);
            } else {
                ++run;
                active = true;
                sec = -1;
                wall.clear();
                sec_start = now;
                have_last = false;
                Line(fmt::format("B ev=run run={} scene={} frame={} warm={} measure={} states=1 renderer=emu", run, scene, frames, warm, measure));
                MessageBeep(MB_OK);
                LOG_INFO(Render_Vulkan, "EMU BENCH run {} started, scene {}, ~{}s, keep the scene and do not press keys", run, scene, warm + measure);
            }
        }
#else
        (void)now;
#endif
    }

    void Second(Clock::time_point now) {
        const double span_ms = std::chrono::duration<double, std::milli>(now - sec_start).count();
        sec_start = now;
        if (wall.empty()) return;
        std::sort(wall.begin(), wall.end());
        (void)span_ms;
        const char* ph = sec < 0 ? "p" : (sec < warm ? "w" : "m");
        // fps and the percentiles come from the same gap samples: fps = 1000 / mean. (It used to be n / wall-clock span,
        // which hid the short gaps from the percentiles; Composite apparently also runs for repeats of the same
        // frame a few hundred microseconds after the last one, those short gaps pull the mean below the median.)
        double sum = 0.0;
        for (const float g : wall) sum += g;
        const double mean = sum / static_cast<double>(wall.size());
        const double med = Pct(wall, 0.5);
        size_t shorts = 0; // gaps under half the median: extra Composite calls, not frames
        for (const float g : wall) shorts += g < 0.5 * med ? 1 : 0;
        // fsr/objvel/blockmatch/anim: the summary script reads them; the Emu path has none of the FSR states (name "emu" = fsr off)
        Line(fmt::format("S t={:.1f} frame={} n={} fps={:.1f} wall_p50={:.2f} wall_p95={:.2f} wall_p99={:.2f} wall_max={:.2f} wall_mean={:.2f} wall_min={:.2f} short={} "
                         "gap=0 rcpu=0 wait=0 stall=0 "
                         "fsr=off objvel=0 blockmatch=0 anim=0 renderer=emu bench={}:0:{}:emu bscene={} bad=0 drift=0",
                         std::chrono::duration<double>(now - t0).count(), frames, wall.size(), mean > 0.0 ? 1000.0 / mean : 0.0, med, Pct(wall, 0.95),
                         Pct(wall, 0.99), wall.back(), mean, wall.front(), shorts, run, ph, scene));
        wall.clear();
        ++sec;
        if (sec >= warm + measure) Finish(false);
    }

    void Frame() {
        if (!init_done) Init();
        if (!enabled) return;
        const auto now = Clock::now();
        ++frames;
        Poll(now);
        if (!active) {
            last_frame = now;
            return;
        }
        if (have_last) wall.push_back(static_cast<float>(std::chrono::duration<double, std::milli>(now - last_frame).count()));
        have_last = true;
        last_frame = now;
        if (now - sec_start >= std::chrono::seconds(1)) Second(now);
    }
};

} // Anonymous namespace

void EmuBenchFrame() {
    if (Core::GameSettings::NvnVulkanMode() != 0) return;
    static EmuBench bench;
    bench.Frame();
}

} // namespace Vulkan
