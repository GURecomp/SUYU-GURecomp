// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/fs/path_util.h"
#include "common/host_key.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/arm/recomp/game_settings.h"
#include "core/arm/recomp/game_settings_ini.h"
#include "core/arm/recomp/recomp_hooks.h"
#include "core/core.h"
#include "core/memory.h"

namespace Core::GameSettings {

namespace {

constexpr u64 kMhgu = 0x0100770008DD8000ULL;
constexpr const char* kMhguMain = "Rabbit_RoApplicationMTFMasterReleaseNX.nss";
constexpr u32 kFrameRateGlobal = 0x18A6210; // main offset of the engine object pointer
constexpr u32 kFrameRateField = 0x243C;     // float target frame rate in that object
constexpr float kOriginalFps = 30.0f;
constexpr u32 kProjection = 0x1C358;        // main offset of the camera projection function
constexpr u32 kProjectionB = 0x1C3A8;       // probe only: variant, r0 = camera params, r1 = output matrix
constexpr u32 kLookAtA = 0x1C33C;           // probe only: look-at, r0 = out view matrix, r1 = camera object
constexpr u32 kLookAtB = 0x1C380;           // probe only: same, r0 = camera object, r1 = out (called by the world-to-screen 0x18A48)
constexpr u32 kFilterCtors[] = {0x968318, 0x8B2FBC, 0x8B72E0, 0x8B8148, 0x8C090C, 0x9310AC};
constexpr u32 kProbeReturnPc = 0xFFFFE100u; // sentinel return address of the probe post-hook (outside the modules)
constexpr u32 kAspectField = 0x38;          // float aspect ratio in its r1 argument
constexpr float kOriginalAspect = 16.0f / 9.0f;
constexpr u32 kClipA = 0x30;                // near and far clip planes (which is which: the
constexpr u32 kClipB = 0x34;                // larger one is far)
// Render size setup: main+0xBBCD34 writes the docked default 1920x1080 to [r5] (when the caller
// gave no size), then main+0xBBCF90 calls main+0xBBBDE0 with the engine object, which creates
// the render targets from it.
constexpr u32 kRenderDefault = 0xBBCD34;
constexpr u32 kRenderCreate = 0xBBBDE0;
constexpr u32 kDockedW = 1920;
constexpr u32 kDockedH = 1080;
constexpr u32 kMaxRenderW = 3840; // largest the public resolution patches go
constexpr u32 kMaxRenderH = 2160;

// ---- Upscaler quality modes (retune here) ------------------------------------------------------------------------------
// While the Direct renderer runs an upscaler (nvn_fsr) the Quality mode decides the render scale (a Settings::ResolutionSetup
// step; the user's saved Graphics > Resolution scale is left alone, [Upscaler] render_scale_override = true uses it instead).
//   ratio = output height / internal render height the mode asks for. The nearest available step (1/4X ... 8X) is used.
//   kRatioBase1080: render at the game's 1080p base whatever the output ("Quality"; ratio = output height / 1080), capped at
//                   kQualityMaxRatio so Native AA <= Quality <= Balanced <= Performance holds for outputs above 1620p.
//   kRatioLowest:   the lowest step (1/4X).
constexpr double kRatioBase1080 = 0.0;
constexpr double kRatioLowest = -1.0;
constexpr double kQualityMaxRatio = 1.5;
constexpr double kFsrQualityRatios[5] = {
    1.0,            // Native AA: internal = output (FSR as anti-aliasing)
    kRatioBase1080, // Quality
    1.5,            // Balanced
    2.0,            // Performance
    kRatioLowest,   // Ultra Performance
};
constexpr const char* kFsrQualityNames[5] = {"Native AA", "Quality", "Balanced", "Performance", "Ultra Performance"};
// Settings::ResolutionSetup order (Res1_4X ... Res8X) and the scale each one means
constexpr double kStepFactor[13] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
constexpr const char* kStepName[13] = {"1/4X", "1/2X", "3/4X", "1X", "5/4X", "3/2X", "2X", "3X", "4X", "5X", "6X", "7X", "8X"};
// What each step means in pixels (height) on the console's two render sizes: 1X = 720p handheld / 1080p docked.
//   step:     1/4X 1/2X 3/4X  1X  5/4X 3/2X  2X   3X   4X   5X   6X   7X   8X
//   handheld:  180  360  540  720  900 1080 1440 2160 2880 3600 4320 5040 5760
//   docked:    270  540  810 1080 1350 1620 2160 3240 4320 5400 6480 7560 8640
constexpr u32 kHandheldH = 720;
constexpr u32 kStepHandheldH[13] = {180, 360, 540, 720, 900, 1080, 1440, 2160, 2880, 3600, 4320, 5040, 5760};
constexpr u32 kStepDockedH[13] = {270, 540, 810, 1080, 1350, 1620, 2160, 3240, 4320, 5400, 6480, 7560, 8640};

// The option table, the layout and the text transforms live in game_settings_ini.cpp (testable on their own).

System* g_system{};
float g_aspect{kOriginalAspect};
bool g_wide{false};
float g_draw_distance{1.0f};
std::mutex g_far_lock;
std::unordered_map<u32, float> g_far_written; // camera params -> far value we wrote
bool g_logged_clip{false};
u32 g_field_ptr_addr{}; // main + kFrameRateGlobal
std::atomic<float> g_fps{kOriginalFps};
std::atomic<bool> g_fps_auto{false};
float g_fps_cap{60.0f}; // auto: the monitor's refresh rate
std::atomic<bool> g_active{false};
u64 g_frames{};
bool g_logged_seen{false};
float g_last_written{0.0f};
u32 g_render_w{0}; // 0: keep the game's render size
u32 g_render_h{0};
u32 g_out_h{1080}; // height of the set [Display] resolution (1080 with resolution = default)
std::atomic<u32> g_render_dest{0}; // where main+0xBBCD34 writes the docked size
bool g_logged_render{false};

std::filesystem::path IniPath() {
    // Portable exports keep the user dir in <package>/user; the ini sits beside the exe.
    return Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir).parent_path() /
           "game_settings.ini";
}

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Reads [section] key from the ini; empty when absent. *present: the key exists at all.
/// The FIRST occurrence of a key wins. A later occurrence with a different value is reported once per key
/// (game_settings: [section] key appears N times ...), because it is silently ignored otherwise.
std::string ReadValue(const std::string& want_section, const std::string& want_key,
                      bool* present = nullptr) {
    std::ifstream in(IniPath());
    std::string line;
    std::string section;
    bool found = false;
    // A key that moved out of [Debug] (layout 2) is found in either section: [Debug] nvn_fsr = [Upscaler] nvn_fsr.
    const std::string want_canon = Ini::Canonical(want_section, want_key);
    std::string first;
    int first_line = 0, line_no = 0, count = 0;
    std::string differ; // "line N = value" of later occurrences with another value
    while (std::getline(in, line)) {
        ++line_no;
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (Trim(line.substr(0, eq)) == want_key && (section == want_section || Ini::Canonical(section, want_key) == want_canon)) {
            std::string value = Trim(line.substr(eq + 1));
            if (const auto c = value.find(';'); c != std::string::npos) {
                value = Trim(value.substr(0, c));
            }
            ++count;
            if (!found) {
                found = true;
                first = value;
                first_line = line_no;
            } else if (value != first) {
                differ += fmt::format(" line {} = '{}'", line_no, value);
            }
        }
    }
    if (found) {
        if (present) {
            *present = true;
        }
        if (!differ.empty()) {
            static std::mutex warned_lock;
            static std::set<std::string> warned;
            std::scoped_lock lk{warned_lock};
            if (warned.insert(want_section + "/" + want_key).second) {
                LOG_WARNING(Core_ARM,
                            "game settings: [{}] {} appears {} times with different values; the FIRST one wins "
                            "(line {} = '{}'), ignored:{}",
                            want_section, want_key, count, first_line, first, differ);
            }
        }
        return first;
    }
    return {};
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Writes the whole file through a temporary beside it (a crash mid-write cannot leave half an ini).
bool WriteAll(const std::filesystem::path& path, const std::string& text) {
    const auto tmp = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::mutex g_ini_write_lock; // EnsureLayout / SetValue: read-modify-write of the file

/// Creates the ini (current layout, defaults); converts an older layout once (backup game_settings.ini.bak, never
/// overwritten: .bak.1, .bak.2 ... when it exists); adds keys a newer build knows into their own sections.
/// Caller holds g_ini_write_lock.
void EnsureLayoutLocked() {
    const auto path = IniPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (WriteAll(path, Ini::Rebuild("", nullptr))) {
            LOG_INFO(Core_ARM, "game settings: wrote the defaults to {}", path.string());
        }
        return;
    }
    const std::string text = ReadAll(path);
    if (!Ini::IsCurrentLayout(text)) {
        auto bak = std::filesystem::path(path).concat(".bak");
        for (int n = 1; std::filesystem::exists(bak, ec) && n < 100; ++n) {
            bak = std::filesystem::path(path).concat(".bak." + std::to_string(n));
        }
        std::filesystem::copy_file(path, bak, std::filesystem::copy_options::none, ec);
        if (ec) {
            LOG_WARNING(Core_ARM, "game settings: could not back up {} ({}); the old layout is kept", path.string(),
                        ec.message());
            return;
        }
        std::vector<std::string> notes;
        const std::string rebuilt = Ini::Rebuild(text, &notes);
        if (!WriteAll(path, rebuilt)) {
            LOG_WARNING(Core_ARM, "game settings: could not rewrite {}; the old layout is kept", path.string());
            return;
        }
        LOG_INFO(Core_ARM, "game settings: converted {} to the current layout (one section each); old file kept as {}",
                 path.string(), bak.filename().string());
        for (const auto& n : notes) {
            LOG_INFO(Core_ARM, "game settings: {}", n);
        }
        return;
    }
    std::vector<std::string> added;
    const std::string updated = Ini::AddMissing(text, &added);
    if (!added.empty() && WriteAll(path, updated)) {
        std::string list;
        for (const auto& a : added) {
            list += (list.empty() ? "" : ", ") + a;
        }
        LOG_INFO(Core_ARM, "game settings: added new options to {}: {}", path.string(), list);
    }
}

void WriteMissingOptions() {
    std::scoped_lock lk{g_ini_write_lock};
    EnsureLayoutLocked();
}

/// [Graphics] renderer = Emulated | Direct (case-insensitive; also emu/replacement, 0/1, true/false, yes/no, on/off).
/// Returns true when the key is there; *direct = Direct. Without `renderer` the legacy [Graphics] native_gpu is read
/// (true = Direct). Anything else counts as Emulated, the original behaviour. *shown: the text as found.
bool ReadRenderer(bool* direct, std::string* shown = nullptr) {
    bool present = false;
    std::string v = ReadValue("Graphics", "renderer", &present);
    if (!present) {
        v = ReadValue("Graphics", "native_gpu", &present);
    }
    if (shown) {
        *shown = v;
    }
    std::string l = v;
    for (char& c : l) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    *direct = l == "direct" || l == "replacement" || l == "true" || l == "1" || l == "yes" || l == "on";
    return present;
}

/// [Graphics] renderer decides; the older [Debug] keys are only read without it. Warns once at start
/// when such a key is still in the file and says otherwise (it has no effect then).
void WarnDeadKeys() {
    bool on = false;
    std::string ng;
    if (!ReadRenderer(&on, &ng)) {
        return;
    }
    const auto truthy = [](const std::string& v) { return v == "true" || v == "1" || v == "yes" || v == "on"; };
    const auto falsy = [](const std::string& v) { return v == "false" || v == "0" || v == "no" || v == "off"; };
    struct Dead {
        const char* key;
        bool conflicts;
    };
    bool p1 = false, p2 = false, p3 = false;
    const std::string nn = ReadValue("Debug", "native_nvn", &p1);
    const std::string nv = ReadValue("Debug", "nvn_vulkan", &p2);
    const std::string sr = ReadValue("Debug", "suyu_render", &p3);
    const Dead dead[] = {
        {"native_nvn", p1 && truthy(nn) != on},
        {"nvn_vulkan", p2 && (nv == "present") != on},
        {"suyu_render", p3 && !falsy(sr) == on}, // native_gpu = true means suyu does not render
    };
    for (const Dead& d : dead) {
        if (d.conflicts) {
            LOG_WARNING(Core_ARM, "game settings: [Debug] {} is ignored: [Graphics] renderer = {} decides the renderer "
                                  "(remove the old key)", d.key, ng);
        }
    }
}

/// "21:9", "16:10", "2.39" -> ratio; 0 when it doesn't parse.
float ParseAspect(const std::string& s) {
    if (const auto colon = s.find(':'); colon != std::string::npos) {
        const float w = std::strtof(s.substr(0, colon).c_str(), nullptr);
        const float h = std::strtof(s.substr(colon + 1).c_str(), nullptr);
        return h > 0.0f ? w / h : 0.0f;
    }
    return std::strtof(s.c_str(), nullptr);
}

/// suyu's output aspect for a ratio (stretch to the window when there is no preset).
Settings::AspectRatio OutputAspect(float a) {
    const auto close = [a](float r) { return std::fabs(a - r) < 0.02f; };
    if (close(21.0f / 9.0f) || close(64.0f / 27.0f) || close(43.0f / 18.0f)) {
        return Settings::AspectRatio::R21_9;
    }
    if (close(32.0f / 9.0f)) {
        return Settings::AspectRatio::R32_9;
    }
    if (close(16.0f / 10.0f)) {
        return Settings::AspectRatio::R16_10;
    }
    if (close(4.0f / 3.0f)) {
        return Settings::AspectRatio::R4_3;
    }
    return Settings::AspectRatio::Stretch;
}

/// What the frontend reported through SetDesktopMode (0s when it didn't).
u32 g_desk_w = 0, g_desk_h = 0, g_desk_hz = 0;

/// The primary monitor's mode: width, height, refresh rate (0s when unknown).
void DesktopMode(u32* w, u32* h, u32* hz) {
    *w = g_desk_w;
    *h = g_desk_h;
    *hz = g_desk_hz;
#ifdef _WIN32
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        *w = mode.dmPelsWidth;
        *h = mode.dmPelsHeight;
        *hz = mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
    }
#endif
}

/// "3440x1440" -> w, h; false when it doesn't parse.
bool ParseResolution(const std::string& s, u32* w, u32* h) {
    const auto x = s.find_first_of("xX*");
    if (x == std::string::npos) {
        return false;
    }
    *w = static_cast<u32>(std::strtoul(s.substr(0, x).c_str(), nullptr, 10));
    *h = static_cast<u32>(std::strtoul(s.substr(x + 1).c_str(), nullptr, 10));
    return *w >= 320 && *h >= 240 && *w <= 16384 && *h <= 16384;
}

bool IsTrue(const std::string& s) {
    return s.empty() || s == "true" || s == "1" || s == "yes" || s == "on";
}

/// [Display] resolution -> g_out_h / g_render_w / g_render_h. The Vulkan renderer reads them (render scale, split scale)
/// when it starts, before Start() runs, so they are resolved from the ini on first use as well; Start() repeats it.
void ResolveDisplayResolution() {
    u32 res_w = 0, res_h = 0;
    if (const std::string res = ReadValue("Display", "resolution"); !res.empty() && res != "default") {
        if (res == "auto") {
            u32 hz = 0;
            DesktopMode(&res_w, &res_h, &hz);
        } else if (!ParseResolution(res, &res_w, &res_h)) {
            res_w = res_h = 0;
        }
    }
    g_out_h = res_h != 0 ? res_h : kDockedH;
    // The guest always renders at console size; the internal resolution changes only through the render-scale steps
    // (EffectiveResolutionStep). g_render_w/h stay 0 (= console size), so the render-size hooks are not installed.
    g_render_w = g_render_h = 0;
}

} // namespace

std::atomic<u32> g_aspect_pc{0};

static bool NativeGpuSet(bool* on) {
    return ReadRenderer(on); // [Graphics] renderer (Direct = true), else the legacy native_gpu
}

bool FenceThreadSleep() {
    static const bool enabled = [] {
        const std::string v = ReadValue("Debug", "fence_thread_sleep");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return enabled;
}

std::string ProbePcs() {
    return ReadValue("Debug", "probe_pcs");
}

u32 RenderWidth() {
    static const bool resolved = (ResolveDisplayResolution(), true);
    (void)resolved;
    return g_render_w;
}

bool NativeNvn() {
    static const bool enabled = [] {
        if (bool on; NativeGpuSet(&on)) return on;
        const std::string v = ReadValue("Debug", "native_nvn");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return enabled;
}

int NvnVulkanMode() {
    static const int mode = [] {
        // [Graphics] native_gpu decides when present; the [Debug] keys are for development (record, shadow)
        if (bool on; NativeGpuSet(&on)) return on ? 3 : 0;
        const std::string v = ReadValue("Debug", "nvn_vulkan");
        return v == "present" ? 3 : (v == "shadow" ? 2 : (v == "record" ? 1 : 0));
    }();
    return mode;
}

int NvnVkDebug() {
    static const int v = [] {
        const std::string t = ReadValue("Debug", "nvn_vk_debug");
        return t.empty() ? 0 : std::atoi(t.c_str());
    }();
    return v;
}

bool NvnUpscalerProbe() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_upscaler_probe");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

int NvnJitterSpike() {
    static const int mode = [] {
        const std::string v = ReadValue("Debug", "nvn_jitter_spike");
        const int m = v.empty() || v == "off" || v == "false" ? 0 : std::atoi(v.c_str());
        return m < 0 || m > 2 ? 0 : m;
    }();
    return mode;
}

bool NvnFsr() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

bool NvnFsrReactive() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_reactive");
        return !(v == "false" || v == "0" || v == "no" || v == "off"); // default on
    }();
    return on;
}

float NvnFsrSharpness() {
    static const float s = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_sharpness");
        if (v.empty()) {
            return 0.3f;
        }
        char* end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        return end == v.c_str() || !std::isfinite(f) ? 0.3f : std::clamp(f, 0.0f, 1.0f);
    }();
    return s;
}

int NvnFsrSnapDraw() {
    // not cached: the renderer polls it about once a second, so the ini can be edited while the game runs
    const std::string v = ReadValue("Debug", "nvn_fsr_snapdraw");
    if (v.empty()) {
        return 0;
    }
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    return end == v.c_str() || n < 0 || n > 100000 ? 0 : static_cast<int>(n);
}

bool NvnFsrHudLayer() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_hudlayer");
        return !(v == "false" || v == "0" || v == "no" || v == "off"); // default on
    }();
    return on;
}

bool NvnFsrMotionMask() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_motionmask");
        return v == "true" || v == "1" || v == "yes" || v == "on"; // default off (shimmer on distant fine edges)
    }();
    return on;
}

int NvnFsrJitterConv() {
    static const int c = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_jitter_conv");
        if (v.empty()) {
            return 1;
        }
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        return end == v.c_str() || n < 0 || n > 3 ? 1 : static_cast<int>(n);
    }();
    return c;
}

float NvnFsrObjMvNear() {
    static const float d = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_objmv_near");
        char* end = nullptr;
        const float f = v.empty() ? 3000.0f : std::strtof(v.c_str(), &end);
        return v.empty() || end == v.c_str() || !std::isfinite(f) || f <= 0.0f ? 3000.0f : f;
    }();
    return d;
}

bool NvnFsrObjMv() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_objmv");
        return !(v == "false" || v == "0" || v == "no" || v == "off"); // default on
    }();
    return on;
}

int NvnFsrObjVel() {
    static const int m = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_objvel");
        if (v.empty() || v == "true" || v == "on" || v == "yes") {
            return 4; // default: every depth-tested colour+depth scene draw
        }
        if (v == "false" || v == "off" || v == "no") {
            return 0;
        }
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        return end == v.c_str() || n < 0 || n > 4 ? 4 : static_cast<int>(n);
    }();
    return m;
}

u32 NvnFsrObjVelMaxVerts() {
    static const u32 m = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_objvel_maxverts");
        char* end = nullptr;
        const long long n = v.empty() ? 0 : std::strtoll(v.c_str(), &end, 10);
        return v.empty() || end == v.c_str() || n <= 0 ? 4000000u : static_cast<u32>(std::clamp<long long>(n, 10000, 8000000));
    }();
    return m;
}

float NvnFsrObjVelReactive() {
    static const float r = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_objvel_reactive");
        char* end = nullptr;
        const float f = v.empty() ? 0.1f : std::strtof(v.c_str(), &end);
        return v.empty() || end == v.c_str() || !std::isfinite(f) ? 0.1f : std::clamp(f, 0.0f, 1.0f);
    }();
    return r;
}

float NvnFsrObjVelMaxPx() {
    static const float d = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_objvel_maxpx");
        char* end = nullptr;
        const float f = v.empty() ? 64.0f : std::strtof(v.c_str(), &end);
        return v.empty() || end == v.c_str() || !std::isfinite(f) || f <= 0.0f ? 64.0f : std::clamp(f, 1.0f, 1024.0f);
    }();
    return d;
}

float NvnFsrReactiveStrength() {
    static const float r = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_reactive_strength");
        char* end = nullptr;
        const float f = v.empty() ? 0.9f : std::strtof(v.c_str(), &end);
        return v.empty() || end == v.c_str() || !std::isfinite(f) ? 0.9f : std::clamp(f, 0.0f, 1.0f);
    }();
    return r;
}

bool NvnFsrAnimSurf() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_animsurf");
        return !(v == "false" || v == "0" || v == "no" || v == "off"); // default on
    }();
    return on;
}

float NvnFsrAnimSurfThr() {
    static const float t = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_animsurf_thr");
        char* end = nullptr;
        const float f = v.empty() ? 0.03f : std::strtof(v.c_str(), &end);
        return v.empty() || end == v.c_str() || !std::isfinite(f) || f <= 0.0f ? 0.03f : std::clamp(f, 0.002f, 1.0f);
    }();
    return t;
}

static bool ReadFlag(const char* key, bool def) {
    const std::string v = ReadValue("Debug", key);
    if (v.empty()) {
        return def;
    }
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

bool NvnFsrSignals() {
    static const bool on = ReadFlag("nvn_fsr_signals", false);
    return on;
}

namespace {
struct FsrDebugState {
    bool on;
    const char* why;
};
const FsrDebugState& FsrDebug() {
    static const FsrDebugState s = [] {
        // nvn_fsr_debug: true / on / yes or a non-zero number (the older "= 1" spelling); nvn_bench: the same flag test as
        // the renderer's bench (1 / true / on / yes)
        const std::string v = ReadValue("Debug", "nvn_fsr_debug");
        const bool dbg = v == "true" || v == "on" || v == "yes" || (!v.empty() && std::atoi(v.c_str()) != 0);
        const bool bench = ReadFlag("nvn_bench", false);
        return FsrDebugState{dbg || bench, dbg && bench ? "nvn_fsr_debug and nvn_bench = 1 are set"
                                           : dbg        ? "nvn_fsr_debug is set"
                                           : bench      ? "nvn_bench = 1 is set (the benchmark switches the FSR states)"
                                                        : "nvn_fsr_debug is off (default) and nvn_bench is off"};
    }();
    return s;
}
} // namespace

bool NvnFsrDebugHooks() {
    return FsrDebug().on;
}

const char* NvnFsrDebugHooksReason() {
    return FsrDebug().why;
}

bool NvnFsrAnimSurfBypass() {
    static const bool on = ReadFlag("nvn_fsr_animsurf_bypass", true);
    return on;
}

bool NvnFsrAnimSurfEdgeGate() {
    static const bool on = ReadFlag("nvn_fsr_animsurf_edge_gate", true);
    return on;
}

bool NvnFsrMenu3d() {
    static const bool on = ReadFlag("nvn_fsr_menu3d", true);
    return on;
}

bool NvnFsrMenuSkip() {
    static const bool on = ReadFlag("nvn_fsr_menuskip", false);
    return on;
}

bool NvnFsrObjVelStatic() {
    static const bool on = ReadFlag("nvn_fsr_objvel_static", true);
    return on;
}

u32 NvnFsrStaticFrames() {
    static const u32 n = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_static_frames");
        char* end = nullptr;
        const long long x = v.empty() ? 0 : std::strtoll(v.c_str(), &end, 10);
        return v.empty() || end == v.c_str() || x <= 0 ? 240u : static_cast<u32>(std::min<long long>(x, 100000));
    }();
    return n;
}

int NvnFsrBackend() {
    static const int b = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_backend");
        return v == "4" ? 4 : 3; // anything else (absent, 3, junk) = the FSR 3.1 path
    }();
    return b;
}

bool NvnFsrIndicator() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_fsr_indicator");
        return v == "true" || v == "1" || v == "yes" || v == "on"; // default off
    }();
    return on;
}

static float ReadHudEdge(const char* key, float def) {
    const std::string v = ReadValue("Debug", key);
    char* end = nullptr;
    const float f = v.empty() ? def : std::strtof(v.c_str(), &end);
    return v.empty() || end == v.c_str() || !std::isfinite(f) ? def : std::clamp(f, 0.0f, 1.0f);
}

float NvnFsrHudLo() {
    return ReadHudEdge("nvn_fsr_hud_lo", 0.08f);
}

float NvnFsrHudHi() {
    return ReadHudEdge("nvn_fsr_hud_hi", 0.25f);
}

bool NvnMvPass() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "nvn_mv_pass");
        return v == "true" || v == "1" || v == "yes" || v == "on" || NvnFsr();
    }();
    return on;
}

// Jitter on the main camera projection: 0 off, 1/2 = nvn_jitter_spike modes, 3 = nvn_fsr (FSR Halton sequence,
// applied only while the renderer reports FSR active). nvn_fsr wins over nvn_jitter_spike.
static int JitMode() {
    return NvnFsr() ? 3 : NvnJitterSpike();
}

int NvnMvLag() {
    static const int lag = [] {
        const std::string v = ReadValue("Debug", "nvn_mv_lag");
        const int n = v.empty() ? 1 : std::atoi(v.c_str()); // default 1: the camera record the game's jittered draws use is the one of the previous guest present
        return std::clamp(n, 0, 3);
    }();
    return lag;
}

bool DescriptorBuffer() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "descriptor_buffer");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

bool MultiRangeBuffers() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "multi_range_buffers");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

bool SuyuRender() {
    static const bool on = [] {
        if (bool ng; NativeGpuSet(&ng)) return !ng;
        const std::string v = ReadValue("Debug", "suyu_render");
        return NvnVulkanMode() != 3 || !(v == "false" || v == "0" || v == "no" || v == "off");
    }();
    return on;
}

bool PresentNoDrain() {
    static const bool on = [] {
        std::string v = ReadValue("Debug", "present_nodrain");
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); // True / TRUE count too
        return !(v == "false" || v == "0" || v == "no" || v == "off"); // absent = on; false = the old draining present
    }();
    return on;
}

int PresRing() {
    static const int n = [] {
        const std::string v = ReadValue("Debug", "pres_ring");
        const int x = v.empty() ? 3 : std::atoi(v.c_str());
        return x < 2 ? 3 : (x > 16 ? 16 : x);
    }();
    return n;
}

bool FenceHold() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "fence_hold");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

bool PresentFifo() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "present_fifo");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

bool OrderProbe() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "order_probe");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

bool VulkanValidation() {
    static const bool on = [] {
        const std::string v = ReadValue("Debug", "vulkan_validation");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return on;
}

bool DiagnosticsEnabled() {
    static const bool enabled = [] {
        const std::string v = ReadValue("Debug", "diagnostics");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return enabled;
}

bool GpuDiagnosticsEnabled() {
    static const bool enabled = [] {
        const std::string v = ReadValue("Debug", "gpu_diagnostics");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return enabled;
}

bool NvnCallSampling() {
    static const bool enabled = ReadFlag("nvn_call_sampling", false); // empty value counts as unset (off)
    return enabled;
}

bool LogRedactPaths() {
    const std::string v = ReadValue("Debug", "log_redact_paths");
    return !(v == "false" || v == "0" || v == "no" || v == "off");
}

bool ProbesEnabled() {
    static const bool enabled = [] {
        const std::string v = ReadValue("Debug", "probes");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return enabled;
}

namespace {
std::atomic<u64> g_settings_generation{0}; // SetValue count (SettingsGeneration)
} // namespace

std::string Value(const std::string& section, const std::string& key) {
    return ReadValue(section, key);
}

void SetValue(const std::string& want_section, const std::string& want_key,
              const std::string& value) {
    {
        std::scoped_lock lk{g_ini_write_lock};
        EnsureLayoutLocked();
        // Replaces the effective line (a moved key is found in its old [Debug] place too) or the "; key = default"
        // line; else adds it inside the key's own section (no duplicate sections).
        const std::string text = ReadAll(IniPath());
        if (!WriteAll(IniPath(), Ini::SetKey(text, want_section, want_key, value))) {
            LOG_WARNING(Core_ARM, "game settings: could not write [{}] {} = {}", want_section, want_key, value);
        }
    }
    g_settings_generation.fetch_add(1, std::memory_order_release); // after the file is written: readers re-read then
}

u64 SettingsGeneration() {
    return g_settings_generation.load(std::memory_order_acquire);
}

std::vector<OptionInfo> Options() {
    std::vector<OptionInfo> out;
    for (const Ini::Row& o : Ini::Rows()) {
        if (o.kind != Ini::Kind::Menu && o.kind != Ini::Kind::Advanced) {
            continue; // the game menu lists the user options (Advanced ones only in Advanced mode); the rest is documented in the file
        }
        std::string comment;
        const std::string_view text{o.comment};
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            std::string_view line = text.substr(start, end - start);
            if (line.starts_with(";")) {
                line.remove_prefix(line.starts_with("; ") ? 2 : 1);
            }
            if (!comment.empty()) {
                comment += ' ';
            }
            comment += line;
            start = end + 1;
        }
        out.push_back({o.section, o.key, o.value ? o.value : "", std::move(comment), o.kind == Ini::Kind::Advanced});
    }
    return out;
}

UpscalerChoice UpscalerChoiceNow() {
    const std::string on = ReadValue("Upscaler", "nvn_fsr");
    if (!(on == "true" || on == "1" || on == "yes" || on == "on")) {
        return UpscalerChoice::Off;
    }
    return ReadValue("Upscaler", "nvn_fsr_backend") == "4" ? UpscalerChoice::Fsr4 : UpscalerChoice::Fsr31;
}

void SetUpscalerChoice(UpscalerChoice choice) {
    SetValue("Upscaler", "nvn_fsr", choice == UpscalerChoice::Off ? "false" : "true");
    if (choice != UpscalerChoice::Off) {
        SetValue("Upscaler", "nvn_fsr_backend", choice == UpscalerChoice::Fsr4 ? "4" : "3");
    }
}

const char* FsrQualityName(FsrQuality q) {
    const int i = static_cast<int>(q);
    return i >= 0 && i < kFsrQualityCount ? kFsrQualityNames[i] : kFsrQualityNames[0];
}

FsrQuality FsrQualityNow() {
    std::string v = ReadValue("Upscaler", "nvn_fsr_quality");
    std::string k; // lower case, letters only: "Native AA" / native_aa / nativeaa
    for (const char c : v) {
        if (std::isalpha(static_cast<unsigned char>(c))) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (k == "quality") return FsrQuality::Quality;
    if (k == "balanced") return FsrQuality::Balanced;
    if (k == "performance") return FsrQuality::Performance;
    if (k == "ultraperformance" || k == "ultra") return FsrQuality::UltraPerformance;
    return FsrQuality::NativeAA; // absent, "native", "nativeaa", junk
}

bool DirectRendererNow() {
    bool direct = false;
    ReadRenderer(&direct);
    return direct;
}

bool RenderScaleOverrideNow() {
    const std::string v = ReadValue("Upscaler", "render_scale_override");
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

const char* ResolutionStepName(int step) {
    return step >= 0 && step < 13 ? kStepName[step] : "?";
}

bool FsrSplitScaleNow() {
    const std::string v = ReadValue("Upscaler", "nvn_fsr_split_scale");
    // default OFF: the owner wants suyu's built-in render scale steps; the custom 1/256 split is opt-in
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

namespace {
/// Render scale a Quality mode asks for (relative to the game's own render height base_h); Ultra Performance = the
/// lowest step. Shared by the nearest-step path and the split path.
double GameBaseHeight(); // the console's 1X render height: 1080 docked, 720 handheld
double ModeTargetScale(FsrQuality q, double out_h, double target_h) {
    if (q == FsrQuality::UltraPerformance) return kStepFactor[0];
    double ratio = kFsrQualityRatios[static_cast<int>(q)];
    if (ratio == kRatioBase1080) ratio = std::min(std::max(out_h / GameBaseHeight(), 1.0), kQualityMaxRatio);
    // the factor is relative to the targets the engine really creates (target_h), not to the 1X base
    return (out_h / ratio) / target_h;
}

/// Nearest Settings::ResolutionSetup step (log distance) to a scale.
int NearestStep(double target_scale) {
    int step = 3;
    double best = 1e30;
    for (int i = 0; i < 13; ++i) {
        const double d = std::fabs(std::log(kStepFactor[i] / target_scale));
        if (d < best - 1e-9) {
            best = d;
            step = i;
        }
    }
    return step;
}

std::atomic<u32> g_win_h{0}; // last window height reported by the frontend (SetWindowSize)
const char* g_out_src = "console base";
// Output height: [Display] resolution = WxH if set; else the current window when native_render is on; else the console base.
double OutputHeight() {
    u32 w = 0, h = 0;
    if (const std::string res = ReadValue("Display", "resolution");
        !res.empty() && res != "default" && res != "auto" && ParseResolution(res, &w, &h)) {
        g_out_src = "[Display] resolution";
        return static_cast<double>(h);
    }
    if (IsTrue(ReadValue("Graphics", "native_render")) && g_win_h.load() != 0) {
        g_out_src = "window size";
        return static_cast<double>(g_win_h.load());
    }
    g_out_src = "console base";
    return GameBaseHeight();
}
// the console's own render size = render scale 1X: 1080p docked, 720p handheld, whatever native_render does
double GameBaseHeight() {
    return Core::GameSettings::StartHandheld() ? static_cast<double>(kHandheldH) : static_cast<double>(kDockedH);
}
// 1X of the render scale is always the console size; native_render and the output size never change it
double TargetHeight() {
    return GameBaseHeight();
}
} // namespace

bool FsrSplitScale(FsrSplit* out) {
    FsrSplit s{};
    const bool on = [&] {
        if (UpscalerChoiceNow() == UpscalerChoice::Off || !DirectRendererNow()) {
            s.why = "no upscaler on the Direct renderer";
            return false;
        }
        if (RenderScaleOverrideNow()) {
            s.why = "render_scale_override is on";
            return false;
        }
        if (!FsrSplitScaleNow()) {
            s.why = "nvn_fsr_split_scale = false";
            return false;
        }
        const FsrQuality q = FsrQualityNow();
        if (q == FsrQuality::NativeAA) {
            s.why = "Native AA (render = output)";
            return false;
        }
        const double out_h = OutputHeight(), base_h = TargetHeight();
        // The output (present images, HUD layer) must be exactly the game's own render size (1X): images at 1X need no
        // rescaling in the translated shaders, so the one scale baked into them (the render scale) stays right for
        // every draw. That is the case with native_render and a resolution set, or the default 1080p.
        const int out_step = NearestStep(out_h / base_h);
        if (out_step != 3) {
            s.why = fmt::format("output {}p is not the game's render height {}p (Native AA would use {}; set native_render = true)",
                                static_cast<int>(out_h), static_cast<int>(base_h), kStepName[out_step]);
            return false;
        }
        const double target = ModeTargetScale(q, out_h, base_h);
        // suyu's rescaling form: up_scale >> down_shift, here in 1/256 steps, reduced
        u32 up = static_cast<u32>(std::clamp(std::lround(target * 256.0), 16l, 256l));
        u32 shift = 8;
        while (shift > 0 && (up % 2) == 0) {
            up /= 2;
            --shift;
        }
        const double scale = static_cast<double>(up) / static_cast<double>(1u << shift);
        if (scale >= 1.0) {
            s.why = fmt::format("{}: the mode's scale x{:.3f} is not below the output", FsrQualityName(q), target);
            return false;
        }
        s.render_scale = scale;
        s.up_scale = up;
        s.down_shift = shift;
        s.why = fmt::format("{} (output {}p = game render height): scene x{:.4f} ({} >> {}), output 1X", FsrQualityName(q),
                            static_cast<int>(out_h), scale, up, shift);
        return true;
    }();
    if (out) *out = std::move(s);
    return on;
}

int EffectiveResolutionStep(int saved_step, bool* by_mode, std::string* why) {
    if (by_mode) *by_mode = false;
    if (why) why->clear();
    if (saved_step < 0 || saved_step > 12) saved_step = 3;
    if (UpscalerChoiceNow() == UpscalerChoice::Off || !DirectRendererNow() || RenderScaleOverrideNow()) {
        return saved_step;
    }
    const FsrQuality q = FsrQualityNow();
    const double out_h = OutputHeight();
    const double target_h = TargetHeight();
    const int step = q == FsrQuality::UltraPerformance ? 0 : NearestStep(ModeTargetScale(q, out_h, target_h));
    if (by_mode) *by_mode = true;
    if (why) {
        const bool hh = StartHandheld();
        const double base_h = GameBaseHeight();
        const u32 px = target_h == GameBaseHeight() ? (hh ? kStepHandheldH[step] : kStepDockedH[step])
                                                    : static_cast<u32>(std::lround(kStepFactor[step] * target_h));
        *why = fmt::format("{} (1X = {}p); output {}p from {} = {:.2f}X of 1X; {} -> step {} ({}p of the {}p console targets){}",
                           hh ? "handheld" : "docked", static_cast<int>(base_h), static_cast<int>(out_h), g_out_src,
                           out_h / base_h, FsrQualityName(q), kStepName[step], px, static_cast<int>(target_h),
                           step == saved_step ? "" : fmt::format("; saved step was {}", kStepName[saved_step]));
        // nvn_fsr_split_scale: the Direct renderer uses the split instead of this single step
        if (FsrSplit sp{}; FsrSplitScale(&sp)) *why += "; split scale: " + sp.why;
    }
    return step;
}

namespace {
std::atomic<int> g_show_fps{-1}; // -1: not read yet
} // namespace

bool ShowFps() {
    if (g_show_fps.load() < 0) {
        const std::string v = ReadValue("Display", "show_fps");
        g_show_fps = (v == "true" || v == "1" || v == "yes" || v == "on") ? 1 : 0;
    }
    return g_show_fps.load() == 1;
}

bool ApplyLive(const std::string& section, const std::string& key, const std::string& value) {
    if (section == "Display" && key == "show_fps") {
        g_show_fps = (value == "true" || value == "1" || value == "yes" || value == "on") ? 1 : 0;
        return true;
    }
    if (section == "Display" && key == "fps") {
        // OnFrame writes the engine's frame rate field every 8 frames from g_fps; the display
        // rate (DisplayRateScale) is read per vsync. Both follow at once.
        if (value == "auto") {
            g_fps = std::min(g_fps.load(), g_fps_cap);
            g_fps_auto = true;
        } else {
            const float v = std::strtof(value.c_str(), nullptr);
            if (v < 10.0f || v > 360.0f) {
                return false;
            }
            g_fps_auto = false;
            g_fps = v;
        }
        LOG_INFO(Core_ARM, "game settings: fps = {} applied while running", value);
        return true;
    }
    return false;
}

bool StartFullscreen() {
    const std::string v = ReadValue("Display", "fullscreen");
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

bool StartHandheld() {
    const std::string v = ReadValue("Display", "console_mode");
    return v == "handheld" || v == "undocked" || v == "portable";
}

void SetWindowSize(u32 width, u32 height) {
    (void)width;
    g_win_h = height;
}

void SetDesktopMode(u32 width, u32 height, u32 refresh_hz) {
    g_desk_w = width;
    g_desk_h = height;
    g_desk_hz = refresh_hz > 1 ? refresh_hz : 0;
}
std::atomic<u32> g_render_default_pc{0};
std::atomic<u32> g_render_create_pc{0};
std::atomic<u32> g_motion_blur_pc{0};
std::atomic<u32> g_vp_probe_pc{0};
std::atomic<u32> g_main_base{0};
std::atomic<bool> g_probe_on{false};
namespace {
// uMotionBlurFilter render function (STATIC-REVIEW-motionblur-velocity.md): r0 = filter, r1 = draw context.
constexpr u32 kMotionBlurRender = 0x8BCD84;
constexpr u32 kMbStretch = 0x64, kMbCamera = 0x65, kMbMedian = 0x66, kMbTemp = 0x3C, kMbVelocity = 0x44;
bool g_mb_force = false;
} // namespace

void Start(System& system, const std::map<u64, std::string>& modules) {
    static std::atomic<bool> started{false};
    if (system.GetApplicationProcessProgramID() != kMhgu || started.exchange(true)) {
        return; // other titles, or a second core registering the same process
    }
    g_system = &system;
    WriteMissingOptions();
    WarnDeadKeys();

    u32 desk_w = 0, desk_h = 0, desk_hz = 0;
    DesktopMode(&desk_w, &desk_h, &desk_hz);
    g_fps_cap = std::clamp(desk_hz != 0 ? static_cast<float>(desk_hz) : 60.0f, 30.0f, 360.0f);

    if (const std::string fps = ReadValue("Display", "fps"); fps == "auto") {
        g_fps_auto = true;
        g_fps = std::min(60.0f, g_fps_cap);
    } else if (!fps.empty()) {
        const float v = std::strtof(fps.c_str(), nullptr);
        if (v >= 10.0f && v <= 360.0f) {
            g_fps = v;
        } else {
            LOG_WARNING(Core_ARM, "game settings: fps = {} is out of range (10-360); using 30",
                        fps);
        }
    }

    // resolution (exact shape) wins over aspect.
    u32 res_w = 0, res_h = 0;
    if (const std::string res = ReadValue("Display", "resolution");
        !res.empty() && res != "default") {
        if (res == "auto") {
            res_w = desk_w;
            res_h = desk_h;
        } else if (!ParseResolution(res, &res_w, &res_h)) {
            LOG_WARNING(Core_ARM, "game settings: resolution = {} isn't WIDTHxHEIGHT or auto", res);
            res_w = res_h = 0;
        }
    }
    bool stretch = false;
    g_out_h = res_h != 0 ? res_h : kDockedH;
    if (res_w != 0 && res_h != 0) {
        g_aspect = static_cast<float>(res_w) / static_cast<float>(res_h);
        stretch = true; // the view already has the window's exact shape: fill it
        // native_render does not resize the guest render (that would stack with the render-scale step);
        // g_render_w/h stay 0 = console size.
    } else if (const std::string aspect = ReadValue("Display", "aspect"); !aspect.empty()) {
        const float v = ParseAspect(aspect);
        if (v >= 1.0f && v <= 4.0f) {
            g_aspect = v;
        } else {
            LOG_WARNING(Core_ARM, "game settings: aspect = {} isn't a ratio from 1 to 4; using 16:9",
                        aspect);
        }
    }

    u32 main_base = 0;
    for (const auto& [base, name] : modules) {
        if (name == kMhguMain) {
            main_base = static_cast<u32>(base);
            g_field_ptr_addr = main_base + kFrameRateGlobal;
        }
    }
    if (const std::string dd = ReadValue("Graphics", "draw_distance"); !dd.empty()) {
        const float v = std::strtof(dd.c_str(), nullptr);
        if (v >= 1.0f && v <= 8.0f) {
            g_draw_distance = v;
        } else {
            LOG_WARNING(Core_ARM, "game settings: draw_distance = {} is out of range (1-8); using 1",
                        dd);
        }
    }
    const bool wide = std::fabs(g_aspect - kOriginalAspect) > 0.005f;
    g_wide = wide;
    // The output shape always follows the ini (a value left in suyu's config by an earlier
    // wide run must not squeeze a 16:9 picture).
    Settings::values.aspect_ratio.SetValue(
        stretch && wide ? Settings::AspectRatio::Stretch
                        : (wide ? OutputAspect(g_aspect) : Settings::AspectRatio::R16_9));
    g_main_base.store(main_base);
    if (main_base != 0 && JitMode() != 0) {
        // jitter spike / nvn_fsr: only the render projection is written; the post-hook is shared with the probe
        RecompHooks::Mark(main_base + kProjection);
        RecompHooks::Mark(main_base + kProjectionB); // counted only (CPU world-to-screen), never written
        g_probe_on.store(true);
        if (NvnFsr()) {
            LOG_INFO(Core_ARM, "game settings: nvn_fsr on: FSR Halton jitter on the main camera projection while FSR is active");
        } else {
            LOG_INFO(Core_ARM, "game settings: nvn_jitter_spike = {} (F4 toggles, F3 amplitude)", NvnJitterSpike());
        }
    }
    if (main_base != 0 && NvnMvPass()) {
        // motion vector pass: main camera view (look-at 0x1C33C) and unjittered projection (0x1C358), read only
        RecompHooks::Mark(main_base + kProjection);
        RecompHooks::Mark(main_base + kLookAtA);
        g_probe_on.store(true);
        LOG_INFO(Core_ARM, "game settings: nvn_mv_pass on (camera capture at main+0x1C33C / 0x1C358), lag {}", NvnMvLag());
    }
    if (main_base != 0 && NvnUpscalerProbe()) {
        RecompHooks::Mark(main_base + kProjection);
        RecompHooks::Mark(main_base + kProjectionB);
        RecompHooks::Mark(main_base + kLookAtA);
        RecompHooks::Mark(main_base + kLookAtB);
        for (const u32 off : kFilterCtors) {
            RecompHooks::Mark(main_base + off);
        }
        g_probe_on.store(true);
        LOG_INFO(Core_ARM, "game settings: upscaler probe hooks on (camera projection, filter constructors)");
    }
    if (main_base != 0 && (ProbesEnabled() || g_render_w != 0)) {
        RecompHooks::Mark(main_base + 0xBC1204);
        g_vp_probe_pc.store(main_base + 0xBC1204);
    }
    if (main_base != 0) {
        if (const std::string mb = ReadValue("Debug", "motion_blur"); mb == "probe" || mb == "force") {
            g_mb_force = mb == "force";
            RecompHooks::Mark(main_base + kMotionBlurRender);
            g_motion_blur_pc.store(main_base + kMotionBlurRender);
            LOG_INFO(Core_ARM, "game settings: motion_blur = {} (hook at {:#x})", mb, main_base + kMotionBlurRender);
        } else if (!mb.empty() && mb != "off") {
            LOG_WARNING(Core_ARM, "game settings: motion_blur = {} is not off, probe or force; using off", mb);
        }
        if (wide || g_draw_distance != 1.0f) {
            RecompHooks::Mark(main_base + kProjection);
            g_aspect_pc.store(main_base + kProjection);
        }
        if (g_render_w != 0) {
            RecompHooks::Mark(main_base + kRenderDefault);
            g_render_default_pc.store(main_base + kRenderDefault);
            RecompHooks::Mark(main_base + kRenderCreate);
            g_render_create_pc.store(main_base + kRenderCreate);
        }
    }
    LOG_INFO(Core_ARM,
             "game settings: fps {}{}, aspect {:.4f}{}, render {}x{}, draw distance x{} "
             "(monitor {}x{} {} Hz)",
             g_fps_auto ? "auto from " : "", g_fps.load(), g_aspect, stretch ? " (fill window)" : "",
             g_render_w != 0 ? g_render_w : kDockedW, g_render_w != 0 ? g_render_h : kDockedH,
             g_draw_distance, desk_w, desk_h, desk_hz);
    g_active.store(g_field_ptr_addr != 0);
}

void OnHookedEntry(u32 pc, u32* regs) {
    auto& mem = g_system->ApplicationMemory();
    if (pc == g_vp_probe_pc.load(std::memory_order_relaxed)) {
        // [Debug] probes: draw-state object r10 of the viewport/scissor setter (main+0xBC12D0/0xBC13BC)
        const u32 o = regs[10];
        if (!mem.IsValidVirtualAddressRange(o, 0x60)) {
            return;
        }
        // Only for our own renderer: suyu's draws already fill the window texture, widening the rect there breaks it.
        if (g_render_w != 0 && !SuyuRender() && mem.Read32(o + 0x58) == ((g_render_h << 16) | g_render_w) &&
            mem.Read32(o + 0x20) == kDockedW && mem.Read32(o + 0x24) == kDockedH) {
            // The pass draws into the custom-size window texture, but its viewport rect still holds the docked size,
            // so the picture only fills the top-left corner. Widen the rect (and its half-size twin) to the target.
            mem.Write32(o + 0x20, g_render_w);
            mem.Write32(o + 0x24, g_render_h);
            mem.Write32(o + 0x30, g_render_w / 2);
            mem.Write32(o + 0x34, g_render_h / 2);
        }
        static std::mutex m;
        static std::set<std::string> seen;
        std::string line;
        for (u32 off = 0x18; off <= 0x58; off += 4) {
            line += fmt::format(" +{:x}={}", off, static_cast<s32>(mem.Read32(o + off)));
        }
        std::scoped_lock lk{m};
        if (ProbesEnabled() && seen.size() < 60 && seen.insert(line).second) {
            LOG_INFO(Core_ARM, "viewport setter state {:#x}:{}", o, line);
        }
        return;
    }
    if (pc == g_motion_blur_pc.load(std::memory_order_relaxed)) {
        // r0 = uMotionBlurFilter. force: switch on camera and stretch blur so the velocity pass runs.
        const u32 self = regs[0];
        if (!mem.IsValidVirtualAddressRange(self, 0x90)) {
            return;
        }
        static std::atomic<u32> calls{0};
        static std::atomic<u64> last_log{0};
        const u32 n = ++calls;
        const u8 was_stretch = mem.Read8(self + kMbStretch), was_camera = mem.Read8(self + kMbCamera);
        if (g_mb_force) {
            mem.Write8(self + kMbStretch, 1);
            mem.Write8(self + kMbCamera, 1);
        }
        const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::steady_clock::now().time_since_epoch()).count());
        if (last_log.exchange(now) != now) {
            LOG_INFO(Core_ARM, "motion blur filter: call {} this {:#x} stretch {} camera {} median {} temp {:#x} velocity {:#x}{}",
                     n, self, was_stretch, was_camera, mem.Read8(self + kMbMedian),
                     mem.Read32(self + kMbTemp), mem.Read32(self + kMbVelocity), g_mb_force ? " (forced on after this read)" : "");
        }
        return;
    }
    if (pc == g_render_default_pc.load(std::memory_order_relaxed)) {
        g_render_dest.store(regs[5]); // the block is about to store 1920x1080 there
        return;
    }
    if (pc == g_render_create_pc.load(std::memory_order_relaxed)) {
        const u32 dest = g_render_dest.load();
        if (dest == 0 || !mem.IsValidVirtualAddress(dest + 4)) {
            return;
        }
        const u32 w = mem.Read32(dest);
        const u32 h = mem.Read32(dest + 4);
        if (w == kDockedW && h == kDockedH) { // only the docked default is replaced
            mem.Write32(dest, g_render_w);
            mem.Write32(dest + 4, g_render_h);
            if (!g_logged_render) {
                g_logged_render = true;
                LOG_INFO(Core_ARM, "game settings: render size at {:#x}: {}x{} -> {}x{}", dest, w,
                         h, g_render_w, g_render_h);
            }
        }
        return;
    }

    // Camera projection: aspect and far plane.
    const u32 field = regs[1] + kAspectField;
    if (!mem.IsValidVirtualAddress(field)) {
        return;
    }
    const float current = std::bit_cast<float>(mem.Read32(field));
    // Only the main 16:9 views: render-to-texture cameras keep their own shape.
    const bool main_view = std::fabs(current - kOriginalAspect) < 0.01f ||
                           (g_wide && std::fabs(current - g_aspect) < 0.01f);
    if (g_wide && std::fabs(current - kOriginalAspect) < 0.01f) {
        mem.Write32(field, std::bit_cast<u32>(g_aspect));
    }
    if (g_draw_distance == 1.0f || !main_view) {
        return;
    }
    // Far plane: the larger of the two clip values. The game may keep its own value there or
    // refresh it every frame; only a value we didn't write ourselves is scaled (never twice).
    const u32 base = regs[1];
    const float a = std::bit_cast<float>(mem.Read32(base + kClipA));
    const float b = std::bit_cast<float>(mem.Read32(base + kClipB));
    if (!std::isfinite(a) || !std::isfinite(b) || a <= 0.0f || b <= 0.0f) {
        return;
    }
    const u32 far_off = a > b ? kClipA : kClipB;
    const float far_now = a > b ? a : b;
    std::scoped_lock lk{g_far_lock};
    if (!g_logged_clip) {
        g_logged_clip = true;
        LOG_INFO(Core_ARM, "game settings: camera clip planes +0x30 = {}, +0x34 = {}", a, b);
    }
    const auto it = g_far_written.find(base);
    if (it != g_far_written.end() && it->second == far_now) {
        return;
    }
    const float scaled = far_now * g_draw_distance;
    mem.Write32(base + far_off, std::bit_cast<u32>(scaled));
    g_far_written[base] = scaled;
}

// ---- [Debug] nvn_upscaler_probe: camera matrix log and filter constructor hit counters -------------------------
namespace {
struct CamPending {
    u32 pc, obj, out, lr, vtable;
    float nearv, farv, aspect, fov;
    bool view; // true: look-at (view matrix) function, false: projection function
};
// Camera object fields (same object for the projection and look-at functions): eye +0x40, up +0x50, target +0x60
// (3 floats each), clip +0x30/+0x34, aspect +0x38, fov +0x3C.
struct CamState {
    u32 ptr{};
    float eye[3]{}, tgt[3]{}, up[3]{};
    float fov{}, aspect{};
    u64 stamp{}; // bumped on every update of the main camera
};
thread_local std::vector<CamPending> t_cam_stack;
std::mutex g_cam_lock;
std::vector<std::string> g_cam_drain; // every call since the last drain (cap 64)
u64 g_cam_calls[2]{};                 // [0] projection, [1] view
u64 g_cam_last_log_s[2]{};
CamState g_main_cam;                  // last camera seen with the main aspect ratio
CamState g_prev_cam;                  // main camera at the previous present that saw an update
u64 g_prev_cam_stamp{};
u64 g_prev_cam_present{};
u32 g_jump_lines{};
u32 g_switch_lines{};
std::atomic<u64> g_filter_hits[std::size(kFilterCtors)];
std::atomic<u64> g_filter_first_present[std::size(kFilterCtors)];
std::atomic<u32> g_filter_first_lr[std::size(kFilterCtors)];
std::atomic<u64> g_probe_frames{0};
// F5 camera trace
bool g_f5_down{};
u32 g_trace_left{};
u32 g_trace_runs{};
u64 g_trace_bytes{};
constexpr u64 kTraceCap = 2u * 1024 * 1024;

std::ofstream& CamLog() {
    static std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) /
                                 "nvn_upscaler_probe_cam.txt",
                             std::ios::trunc);
    return out;
}

std::ofstream& TraceLog() {
    static std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) /
                                 "nvn_upscaler_probe_camtrace.txt",
                             std::ios::trunc);
    return out;
}

u64 NowSeconds() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

float RdF(Core::Memory::Memory& mem, u32 addr) {
    return std::bit_cast<float>(mem.Read32(addr));
}

CamState ReadCam(Core::Memory::Memory& mem, u32 obj) {
    CamState c;
    c.ptr = obj;
    if (!mem.IsValidVirtualAddressRange(obj, 0x70)) {
        return c;
    }
    for (u32 i = 0; i < 3; ++i) {
        c.eye[i] = RdF(mem, obj + 0x40 + i * 4);
        c.up[i] = RdF(mem, obj + 0x50 + i * 4);
        c.tgt[i] = RdF(mem, obj + 0x60 + i * 4);
    }
    c.aspect = RdF(mem, obj + kAspectField);
    c.fov = RdF(mem, obj + 0x3C);
    return c;
}

std::string CamText(const CamState& c) {
    return fmt::format("eye {:.5g} {:.5g} {:.5g} target {:.5g} {:.5g} {:.5g} up {:.5g} {:.5g} {:.5g} fov {:.5g} aspect {:.5g}",
                       c.eye[0], c.eye[1], c.eye[2], c.tgt[0], c.tgt[1], c.tgt[2], c.up[0], c.up[1], c.up[2],
                       c.fov, c.aspect);
}

float Dist3(const float* a, const float* b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
} // namespace

// ---- [Debug] nvn_jitter_spike: sub-pixel jitter on the main camera's render projection -----------------------
namespace {
std::mutex g_jit_lock;
bool g_jit_on{};                 // F4
u32 g_jit_amp_idx{1};            // F3: {0.5, 2, 8} px
constexpr float kJitAmps[3] = {0.5f, 2.0f, 8.0f};
u64 g_jit_present{};
float g_jit_x{}, g_jit_y{};      // current jitter in render pixels (set once per present)
bool g_jit_f3_down{}, g_jit_f4_down{};
u32 g_jit_lines{};
u64 g_jit_skip_aspect{}, g_jit_skip_w2s{}, g_jit_applied{};
u64 g_jit_last_log_s{};
u64 g_jit_last_sum_s{};
bool g_jit_init{};
// nvn_fsr (JitMode 3): set by the renderer (render thread), read here under g_jit_lock / by the hook
std::atomic<bool> g_fsr_active{false};
std::atomic<u32> g_fsr_rw{0}, g_fsr_rh{0}, g_fsr_ow{0}, g_fsr_oh{0};
u32 g_fsr_phase_idx{}, g_fsr_phase_cnt{};

std::ofstream& JitLog() {
    static std::ofstream out(Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "nvn_jitter_spike.txt",
                             std::ios::trunc);
    return out;
}

float Halton(u32 i, u32 b) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= static_cast<float>(b);
        r += f * static_cast<float>(i % b);
        i /= b;
    }
    return r;
}

void JitPickPhase() { // g_jit_lock held
    const float a = kJitAmps[g_jit_amp_idx];
    if (JitMode() == 3) {
        // FSR: AMD's sequence (ffxFsr3UpscalerGetJitterPhaseCount / GetJitterOffset): phase count
        // int(8 * (output_w / render_w)^2), offset = Halton(i + 1, 2|3) - 0.5 in render target pixels. Stored in pixels
        // of the jitter grid (MvRenderSize, which is the [Display] resolution, not the real render target), so
        // JitterApply's ndc = 2 * px / grid = 2 * offset / render target.
        const u32 rw = g_fsr_rw.load(std::memory_order_relaxed), rh = g_fsr_rh.load(std::memory_order_relaxed);
        const u32 ow = g_fsr_ow.load(std::memory_order_relaxed);
        const u32 gw = g_render_w != 0 ? g_render_w : kDockedW, gh = g_render_w != 0 ? g_render_h : kDockedH;
        u32 count = 8;
        if (rw != 0 && ow != 0) {
            const float r = static_cast<float>(ow) / static_cast<float>(rw);
            count = static_cast<u32>(std::max(1, static_cast<int>(8.0f * r * r)));
        }
        const u32 i = static_cast<u32>(g_jit_present % count);
        const float ox = Halton(i + 1, 2) - 0.5f, oy = Halton(i + 1, 3) - 0.5f;
        g_fsr_phase_idx = i;
        g_fsr_phase_cnt = count;
        g_jit_x = rw != 0 ? ox * static_cast<float>(gw) / static_cast<float>(rw) : ox;
        g_jit_y = rh != 0 ? oy * static_cast<float>(gh) / static_cast<float>(rh) : oy;
    } else if (NvnJitterSpike() == 1) {
        g_jit_x = a;
        g_jit_y = 0.0f;
    } else {
        const u32 i = static_cast<u32>(g_jit_present % 8) + 1;
        g_jit_x = (Halton(i, 2) - 0.5f) * 2.0f * a;
        g_jit_y = (Halton(i, 3) - 0.5f) * 2.0f * a;
    }
}

void JitDescribe(const char* why) { // g_jit_lock held
    JitLog() << fmt::format("# {} at present {}: jitter {} mode {} amplitude {} px", why, g_jit_present,
                            g_jit_on ? "ON" : "OFF", JitMode(), kJitAmps[g_jit_amp_idx]);
    if (JitMode() == 3) {
        JitLog() << " ; nvn_fsr: FSR Halton(2,3) sequence, +-0.5 render target px, on while FSR is active (Insert)";
    } else if (NvnJitterSpike() == 1) {
        JitLog() << fmt::format(" ; applying +{} px in x (m[8] = -ndc_x). If the picture moves RIGHT the sign is "
                                "as assumed; if it moves LEFT it is inverted",
                                kJitAmps[g_jit_amp_idx]);
    }
    JitLog() << std::endl;
}

void JitterTick() {
    std::scoped_lock lk{g_jit_lock};
    const bool fsr = JitMode() == 3;
    if (!g_jit_init) {
        g_jit_init = true;
        g_jit_on = fsr ? g_fsr_active.load() : NvnJitterSpike() != 0;
        JitPickPhase();
        JitLog() << "# render size for NDC: " << (g_render_w != 0 ? g_render_w : kDockedW) << "x"
                 << (g_render_w != 0 ? g_render_h : kDockedH) << std::endl;
        JitDescribe("start");
    }
    ++g_jit_present;
    if (fsr) {
        // nvn_fsr: on/off follows the renderer's FSR state (Insert); F3/F4 belong to the spike mode only
        const bool on = g_fsr_active.load();
        if (on != g_jit_on) {
            g_jit_on = on;
            JitDescribe(on ? "FSR active" : "FSR inactive");
        }
    }
    const bool f4 = !fsr && Common::HostKeyDown(VK_F4);
    const bool f3 = !fsr && Common::HostKeyDown(VK_F3);
    if (f4 && !g_jit_f4_down) {
        g_jit_on = !g_jit_on;
        JitDescribe("F4 toggle");
    }
    if (f3 && !g_jit_f3_down) {
        g_jit_amp_idx = (g_jit_amp_idx + 1) % 3;
        JitDescribe("F3 amplitude");
    }
    g_jit_f4_down = f4;
    g_jit_f3_down = f3;
    JitPickPhase();
    // nvn_fsr: the once-a-second nvn_jitter_spike.txt summary is a debug hook (nvn_fsr_debug / nvn_bench); the spike mode keeps it
    if (fsr && !NvnFsrDebugHooks()) {
        return;
    }
    const u64 now = NowSeconds();
    if (now != g_jit_last_sum_s) {
        g_jit_last_sum_s = now;
        JitLog() << fmt::format("# present {}: applied {} skipped(not main camera) {} skipped(CPU world-to-screen entry 0x1C3A8) {}",
                                g_jit_present, g_jit_applied, g_jit_skip_aspect, g_jit_skip_w2s)
                 << std::endl;
    }
}

// Post-hook of main+0x1C358 (render projection), after the game wrote the matrix at 'out'.
// Returns true when a jitter was written; *jx / *jy = the jitter in render pixels (see JitPickPhase).
bool JitterApply(Core::Memory::Memory& mem, const CamPending& p, bool main_aspect, float* jx, float* jy) {
    std::scoped_lock lk{g_jit_lock};
    if (!g_jit_on) {
        return false;
    }
    if (!main_aspect) {
        ++g_jit_skip_aspect;
        return false;
    }
    if (!mem.IsValidVirtualAddressRange(p.out, 64)) {
        return false;
    }
    *jx = g_jit_x;
    *jy = g_jit_y;
    const float w = static_cast<float>(g_render_w != 0 ? g_render_w : kDockedW);
    const float h = static_cast<float>(g_render_w != 0 ? g_render_h : kDockedH);
    const float nx = 2.0f * g_jit_x / w;
    const float ny = 2.0f * g_jit_y / h;
    const u32 b8 = mem.Read32(p.out + 0x20), b9 = mem.Read32(p.out + 0x24);
    // w = -z: ndc = ... - m[8] (x), - m[9] (y); so m = -ndc shifts the picture by +ndc
    const float a8 = std::bit_cast<float>(b8) - nx;
    const float a9 = std::bit_cast<float>(b9) - ny;
    mem.Write32(p.out + 0x20, std::bit_cast<u32>(a8));
    mem.Write32(p.out + 0x24, std::bit_cast<u32>(a9));
    ++g_jit_applied;
    if (JitMode() == 3 && !NvnFsrDebugHooks()) {
        return true; // nvn_fsr without the debug hooks: no per-call jitter log
    }
    const u64 now = NowSeconds();
    if (g_jit_lines < 120 || now != g_jit_last_log_s) {
        ++g_jit_lines;
        g_jit_last_log_s = now;
        JitLog() << fmt::format(
            "present {} cam {:#x} jitter px ({:.4f}, {:.4f}) ndc ({:.6f}, {:.6f}) m[8] {:.7g} -> {:.7g} m[9] {:.7g} -> {:.7g}",
            g_jit_present, p.obj, g_jit_x, g_jit_y, nx, ny, std::bit_cast<float>(b8), a8, std::bit_cast<float>(b9), a9)
                 << std::endl;
    }
    return true;
}
} // namespace

void FsrSetActive(bool on) {
    g_fsr_active.store(on);
}

bool FsrActive() {
    return g_fsr_active.load();
}

void FsrSetSizes(u32 render_w, u32 render_h, u32 output_w, u32 output_h) {
    g_fsr_rw.store(render_w, std::memory_order_relaxed);
    g_fsr_rh.store(render_h, std::memory_order_relaxed);
    g_fsr_ow.store(output_w, std::memory_order_relaxed);
    g_fsr_oh.store(output_h, std::memory_order_relaxed);
}

void FsrJitterPhase(u32* index, u32* count) {
    std::scoped_lock lk{g_jit_lock};
    *index = g_fsr_phase_idx;
    *count = g_fsr_phase_cnt;
}

// ---- [Debug] nvn_mv_pass: main camera state per guest present, for the motion vector pass ---------------------
// Capture (guest threads, in the probe post-hooks; read only):
//  - look-at main+0x1C33C (r0 = out, r1 = camera): the view matrix of a main-aspect camera, kept per camera pointer.
//  - projection main+0x1C358 (r0 = out, r1 = camera), main aspect only (same test as the jitter): the matrix as the
//    game wrote it, read BEFORE JitterApply writes m[8]/m[9], i.e. unjittered. Paired with the last view of the same
//    camera; the pair is the "pending" record of the current present interval (last one wins; calls are counted).
// Present (guest thread, nvn_vk Renderer::Present -> MvOnGuestPresent): the pending record gets the index of that
// present (1-based count of guest presents) and goes into a ring of 32. So a record tagged N holds the camera computed
// between guest presents N-1 and N. Which present's GPU frame actually uses it is NOT known from the code: the
// renderer looks up "latest record with tag <= frame - nvn_mv_lag" (lag default 1), and the F2 dump measures the
// lags 0..3 against the picture.
namespace {
constexpr float kMvResetDistance = 100.0f; // world units of eye movement in one record step = a cut (history reset)
std::mutex g_mv_lock;
std::atomic<u64> g_mv_present{0};
MvCam g_mv_pending;                 // camera of the current interval (valid = false until a projection is seen)
MvCam g_mv_ring[32];                // by present % 32
MvCam g_mv_last_pushed;             // for the reset test
u32 g_mv_view_ptr{};                // last main-aspect look-at: camera pointer and matrix
float g_mv_view[16]{};
u64 g_mv_view_calls{};
u64 g_mv_proj_calls{}, g_mv_proj_noview{};
u32 g_mv_present_thread{};

u32 ThreadTag() {
    return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

// eye = -t * R^T (row-vector view: v_view = v_world * R + t, rows 0-2 of the matrix = R, row 3 = t)
void EyeFromView(const float* m, float* eye) {
    for (u32 j = 0; j < 3; ++j) {
        eye[j] = -(m[12] * m[j * 4 + 0] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
    }
}

void MvCapture(const CamPending& p, const float* m, bool main_aspect, bool jit_applied, float jx, float jy) {
    if (!main_aspect) {
        return;
    }
    std::scoped_lock lk{g_mv_lock};
    if (p.view) {
        g_mv_view_ptr = p.obj;
        std::copy(m, m + 16, g_mv_view);
        ++g_mv_view_calls;
        return;
    }
    ++g_mv_proj_calls;
    if (g_mv_view_ptr != p.obj) {
        ++g_mv_proj_noview; // no view of this camera seen yet: keep the previous pending record
        return;
    }
    const u32 calls = g_mv_pending.valid ? g_mv_pending.calls + 1 : 1;
    MvCam& c = g_mv_pending;
    c = MvCam{};
    c.ptr = p.obj;
    std::copy(g_mv_view, g_mv_view + 16, c.view);
    std::copy(m, m + 16, c.proj);
    c.jitter_px[0] = jit_applied ? jx : 0.0f;
    c.jitter_px[1] = jit_applied ? jy : 0.0f;
    EyeFromView(c.view, c.eye);
    c.calls = calls;
    c.thread = ThreadTag();
    c.valid = true;
}
} // namespace

u64 MvOnGuestPresent() {
    std::scoped_lock lk{g_mv_lock};
    const u64 idx = g_mv_present.load(std::memory_order_relaxed) + 1;
    g_mv_present.store(idx, std::memory_order_relaxed);
    g_mv_present_thread = ThreadTag();
    if (g_mv_pending.valid) {
        MvCam c = g_mv_pending;
        c.present = idx;
        const MvCam& o = g_mv_last_pushed;
        c.reset = !o.valid || o.ptr != c.ptr || Dist3(o.eye, c.eye) > kMvResetDistance;
        g_mv_ring[idx % std::size(g_mv_ring)] = c;
        g_mv_last_pushed = c;
        g_mv_pending.valid = false;
    }
    return idx;
}

bool MvCamFor(u64 target, MvCam* out) {
    std::scoped_lock lk{g_mv_lock};
    const u64 now = g_mv_present.load(std::memory_order_relaxed);
    if (target > now && g_mv_pending.valid) {
        *out = g_mv_pending; // interval still open: the camera of the coming present
        out->present = now + 1;
        const MvCam& o = g_mv_last_pushed;
        out->reset = !o.valid || o.ptr != out->ptr || Dist3(o.eye, out->eye) > kMvResetDistance;
        return true;
    }
    const u64 n = std::size(g_mv_ring);
    for (u64 i = std::min(target, now); i > 0 && i + n > now; --i) {
        const MvCam& c = g_mv_ring[i % n];
        if (c.valid && c.present == i) {
            *out = c;
            return true;
        }
    }
    return false;
}

std::string MvCaptureStats() {
    std::scoped_lock lk{g_mv_lock};
    return fmt::format("guest presents {} view calls {} projection calls {} (without a view of the same camera {}) "
                       "present thread {:08x} last camera thread {:08x}",
                       g_mv_present.load(), g_mv_view_calls, g_mv_proj_calls, g_mv_proj_noview, g_mv_present_thread,
                       g_mv_last_pushed.thread);
}

void MvRenderSize(u32* w, u32* h) {
    RenderWidth();
    *w = g_render_w != 0 ? g_render_w : kDockedW;
    *h = g_render_w != 0 ? g_render_h : kDockedH;
}

void OnProbeDispatch(u32 pc, u32* regs) {
    const u32 base = g_main_base.load(std::memory_order_relaxed);
    if (base == 0 || g_system == nullptr) {
        return;
    }
    auto& mem = g_system->ApplicationMemory();
    if (pc == kProbeReturnPc) {
        if (t_cam_stack.empty()) {
            return; // not ours
        }
        const CamPending p = t_cam_stack.back();
        t_cam_stack.pop_back();
        regs[15] = p.lr; // resume the caller; the dispatcher looks the block up from r15 afterwards
        float m[16]{};
        bool ok = mem.IsValidVirtualAddressRange(p.out, 64);
        for (u32 i = 0; ok && i < 16; ++i) {
            m[i] = std::bit_cast<float>(mem.Read32(p.out + i * 4));
        }
        const bool main_aspect =
            std::fabs(p.aspect - kOriginalAspect) < 0.01f || std::fabs(p.aspect - g_aspect) < 0.01f;
        bool jit_applied = false;
        float jx = 0.0f, jy = 0.0f;
        if (JitMode() != 0 && !p.view) {
            if (p.pc == base + kProjection) {
                // render projection: last write of the matrix, so the jitter is applied after the game's own result
                jit_applied = JitterApply(mem, p, main_aspect, &jx, &jy);
            } else {
                std::scoped_lock jl{g_jit_lock}; // 0x1C3A8: only called by the CPU world-to-screen 0x18A48; left alone
                ++g_jit_skip_w2s;
            }
        }
        if (NvnMvPass() && ok && (p.pc == base + kLookAtA || p.pc == base + kProjection)) {
            MvCapture(p, m, main_aspect, jit_applied, jx, jy); // m was read before the jitter write
        }
        if (!NvnUpscalerProbe()) {
            return;
        }
        const CamState cs = ReadCam(mem, p.obj);
        std::scoped_lock lk{g_cam_lock};
        const u32 k = p.view ? 1 : 0;
        const u64 n = ++g_cam_calls[k];
        const u64 now = NowSeconds();
        const bool main_view = std::fabs(p.aspect - kOriginalAspect) < 0.01f ||
                               std::fabs(p.aspect - g_aspect) < 0.01f;
        if (main_view) {
            const u64 stamp = g_main_cam.stamp + 1;
            g_main_cam = cs;
            g_main_cam.stamp = stamp;
        }
        std::string line;
        if (p.view) {
            line = fmt::format(
                "view call {} present {} entry main+{:#x} cam {:#x} vtable {:#x} out {:#x} {}{}", n,
                g_probe_frames.load(), p.pc - base, p.obj, p.vtable, p.out, CamText(cs),
                main_view ? " main_view" : "");
        } else {
            line = fmt::format(
                "call {} present {} entry main+{:#x} cam {:#x} vtable {:#x} out {:#x} clip+0x30 {} clip+0x34 {} (larger = far) aspect {} fov {}{}",
                n, g_probe_frames.load(), p.pc - base, p.obj, p.vtable, p.out, p.nearv, p.farv, p.aspect, p.fov,
                main_view ? " main_view" : "");
        }
        if (ok) {
            for (u32 r = 0; r < 4; ++r) {
                line += fmt::format("\n  [{}] {:.7g} {:.7g} {:.7g} {:.7g} | {:08x} {:08x} {:08x} {:08x}", r, m[r * 4],
                                    m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3], std::bit_cast<u32>(m[r * 4]),
                                    std::bit_cast<u32>(m[r * 4 + 1]), std::bit_cast<u32>(m[r * 4 + 2]),
                                    std::bit_cast<u32>(m[r * 4 + 3]));
            }
        } else {
            line += " (output unreadable)";
        }
        // the view function runs for every world-to-screen call: keep only the first 24 per drain
        if (g_cam_drain.size() < 64 && (!p.view || g_cam_drain.size() < 24)) {
            g_cam_drain.push_back(line);
        }
        if (n <= 40 || now != g_cam_last_log_s[k]) {
            if (n > 40) {
                g_cam_last_log_s[k] = now;
            }
            CamLog() << line << std::endl;
        }
        return;
    }
    // look-at (view matrix): 0x1C33C r0 = out, r1 = camera; 0x1C380 r0 = camera, r1 = out (returns at once if out is 0)
    // projection: 0x1C358 r0 = out, r1 = camera; 0x1C3A8 r0 = camera, r1 = out
    const bool is_view = pc == base + kLookAtA || pc == base + kLookAtB;
    if (is_view || pc == base + kProjection || pc == base + kProjectionB) {
        const bool for_mv = NvnMvPass() && (pc == base + kLookAtA || pc == base + kProjection);
        if (!NvnUpscalerProbe() && !for_mv && (is_view || JitMode() == 0)) {
            return; // jitter spike alone only needs the two projection entries; the mv pass 0x1C33C and 0x1C358
        }
        if (t_cam_stack.size() >= 8) {
            return;
        }
        const bool a = pc == base + kLookAtA || pc == base + kProjection; // r0 = out, r1 = camera
        CamPending p{};
        p.pc = pc;
        p.view = is_view;
        p.obj = a ? regs[1] : regs[0];
        p.out = a ? regs[0] : regs[1];
        p.lr = regs[14];
        if (p.out == 0 || !mem.IsValidVirtualAddressRange(p.obj, 0x40)) {
            return;
        }
        p.vtable = mem.Read32(p.obj);
        p.nearv = std::bit_cast<float>(mem.Read32(p.obj + kClipA)); // which of the two is far: see the log
        p.farv = std::bit_cast<float>(mem.Read32(p.obj + kClipB));
        p.aspect = std::bit_cast<float>(mem.Read32(p.obj + kAspectField));
        p.fov = std::bit_cast<float>(mem.Read32(p.obj + 0x3C));
        t_cam_stack.push_back(p);
        regs[14] = kProbeReturnPc; // post-hook: the callee returns to the dispatcher with the sentinel
        return;
    }
    for (std::size_t i = 0; i < std::size(kFilterCtors); ++i) {
        if (pc == base + kFilterCtors[i]) {
            if (g_filter_hits[i].fetch_add(1, std::memory_order_relaxed) == 0) {
                g_filter_first_present[i].store(g_probe_frames.load(), std::memory_order_relaxed);
                g_filter_first_lr[i].store(regs[14] - base, std::memory_order_relaxed);
            }
            return;
        }
    }
}

std::string CamProbeDrain() {
    std::scoped_lock lk{g_cam_lock};
    std::string out;
    for (const auto& l : g_cam_drain) {
        out += l;
        out += '\n';
    }
    g_cam_drain.clear();
    // per-frame state of the main camera (also written when no call happened this frame)
    out += fmt::format("main camera state at present {} (updates so far {}): cam {:#x} {}\n", g_probe_frames.load(),
                       g_main_cam.stamp, g_main_cam.ptr, CamText(g_main_cam));
    return out;
}

static void ProbeTick() {
    const u64 present = ++g_probe_frames;
    const u64 now = NowSeconds();
    std::scoped_lock lk{g_cam_lock};
    {
        const bool down = Common::HostKeyDown(VK_F5);
        if (down && !g_f5_down && g_trace_left == 0 && g_trace_bytes < kTraceCap) {
            g_trace_left = 1800;
            ++g_trace_runs;
            TraceLog() << fmt::format("# trace {} starts at present {}: frame cam eye(xyz) target(xyz) fov aspect; "
                                      "'stale' = no camera call since the previous line\n",
                                      g_trace_runs, present);
        }
        g_f5_down = down;
    }
    const bool fresh = g_main_cam.stamp != g_prev_cam_stamp;
    if (fresh && g_main_cam.ptr != 0) {
        if (g_prev_cam.ptr != 0) {
            const CamState& o = g_prev_cam;
            const CamState& c = g_main_cam;
            if (o.ptr != c.ptr) {
                if (g_switch_lines < 50) {
                    ++g_switch_lines;
                    CamLog() << fmt::format("CAM_PTR_CHANGE present {} (prev update present {}): {:#x} -> {:#x}; new {}\n",
                                            present, g_prev_cam_present, o.ptr, c.ptr, CamText(c));
                }
            } else {
                const float de = Dist3(o.eye, c.eye);
                const float dt = Dist3(o.tgt, c.tgt);
                const float dfov = std::fabs(c.fov - o.fov);
                const float fov_thr = (c.fov > 0.0f && c.fov < 6.3f) ? 5.0f * 3.14159265f / 180.0f : 5.0f; // radians or degrees
                if ((de > 5.0f || dt > 5.0f || dfov > fov_thr) && g_jump_lines < 200) {
                    ++g_jump_lines;
                    CamLog() << fmt::format(
                        "CAM_JUMP {} present {} (prev update present {}) cam {:#x} d_eye {:.5g} d_target {:.5g} d_fov {:.5g} "
                        "old [{}] new [{}]\n",
                        g_jump_lines, present, g_prev_cam_present, c.ptr, de, dt, dfov, CamText(o), CamText(c));
                }
            }
        }
        g_prev_cam = g_main_cam;
        g_prev_cam_present = present;
    }
    g_prev_cam_stamp = g_main_cam.stamp;
    if (g_trace_left != 0) {
        --g_trace_left;
        const CamState& c = g_main_cam;
        const std::string line = fmt::format(
            "{} {:#x} {:.5g} {:.5g} {:.5g} {:.5g} {:.5g} {:.5g} {:.5g} {:.5g}{}\n", present, c.ptr, c.eye[0], c.eye[1],
            c.eye[2], c.tgt[0], c.tgt[1], c.tgt[2], c.fov, c.aspect, fresh ? "" : " stale");
        if (g_trace_bytes + line.size() > kTraceCap) {
            g_trace_left = 0;
            TraceLog() << "# size cap reached\n" << std::flush;
            g_trace_bytes = kTraceCap;
        } else {
            TraceLog() << line;
            g_trace_bytes += line.size();
            if (g_trace_left == 0) {
                TraceLog() << "# trace ends\n" << std::flush;
            }
        }
        if (present % 60 == 0) {
            TraceLog().flush();
        }
    }
    static u64 last = 0;
    if (now - last < 3) {
        return;
    }
    last = now;
    std::string line = fmt::format("filter ctor hits at present {} (every 3 s for the whole run; cam jumps {}):", present, g_jump_lines);
    for (std::size_t i = 0; i < std::size(kFilterCtors); ++i) {
        line += fmt::format(" main+{:#x}={}", kFilterCtors[i], g_filter_hits[i].load());
        if (g_filter_hits[i].load() != 0) {
            line += fmt::format("(first present {} from main+{:#x})", g_filter_first_present[i].load(),
                                g_filter_first_lr[i].load());
        }
    }
    CamLog() << line << std::endl;
}

float DisplayRateScale() {
    const float top = g_fps_auto ? g_fps_cap : g_fps.load(std::memory_order_relaxed);
    return top > 60.0f ? top / 60.0f : 1.0f;
}

void OnFrame() {
    if (g_probe_on.load(std::memory_order_relaxed)) {
        if (NvnUpscalerProbe()) {
            ProbeTick();
        }
        if (JitMode() != 0) {
            JitterTick();
        }
    }
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_fps_auto) {
        // Follow the rate the PC reaches: once a second, drop to what was measured when the
        // target was missed, or step up while it is met, up to the monitor's refresh rate.
        static auto window_start = std::chrono::steady_clock::now();
        static u32 window_frames = 0;
        static u32 changes = 0;
        ++window_frames;
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - window_start).count();
        if (secs >= 1.0) {
            const float measured = static_cast<float>(window_frames / secs);
            const float target = g_fps.load();
            float next = target;
            if (measured < target * 0.93f) {
                next = std::max(30.0f, std::floor(measured));
            } else if (measured >= target * 0.98f && target < g_fps_cap) {
                next = std::min(g_fps_cap, target + 5.0f);
            }
            if (next != target) {
                g_fps = next;
                if (++changes <= 20 || changes % 60 == 0) {
                    LOG_INFO(Core_ARM, "game settings: auto fps {:.0f} measured -> target {:.0f}",
                             measured, next);
                }
            }
            window_start = now;
            window_frames = 0;
        }
    }
    if ((++g_frames % 8) != 0) {
        return;
    }
    auto& mem = g_system->ApplicationMemory();
    const u32 object = mem.Read32(g_field_ptr_addr);
    if (object == 0 || !mem.IsValidVirtualAddress(object + kFrameRateField)) {
        return;
    }
    const u32 field = object + kFrameRateField;
    const float current = std::bit_cast<float>(mem.Read32(field));
    if (!g_logged_seen) {
        g_logged_seen = true;
        LOG_INFO(Core_ARM, "game settings: engine frame rate field at {:#x} = {}", field,
                 current);
    }
    const float want = g_fps.load(std::memory_order_relaxed);
    if (!g_fps_auto && want == kOriginalFps && g_last_written == 0.0f) {
        return; // original game: never touch it
    }
    // Only a plausible frame rate is overwritten: anything else means the object isn't the one
    // we expect (yet).
    if (!std::isfinite(current) || current < 10.0f || current > 360.0f || current == want) {
        return;
    }
    mem.Write32(field, std::bit_cast<u32>(want));
    if (g_last_written == 0.0f) {
        LOG_INFO(Core_ARM, "game settings: engine frame rate {} -> {}", current, want);
    }
    g_last_written = want;
}

} // namespace Core::GameSettings
