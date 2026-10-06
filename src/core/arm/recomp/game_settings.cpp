// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/arm/recomp/game_settings.h"
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

constexpr const char* kTemplate =
    "; game_settings.ini: options for this exported game.\n"
    "; Edit a value, save, and restart the game. Nothing needs to be exported again.\n"
    "; Lines starting with ; are comments. Defaults are the original game's behaviour.\n";

struct Option {
    const char* section;
    const char* key;
    const char* value; // default
    const char* comment;
};

// Written into a new ini, and appended to an older one that lacks them.
constexpr Option kOptions[] = {
    {"Display", "fps", "30",
     "; Target frame rate. 30 = original game. 60 is the usual choice; 90, 120 and up only make\n"
     "; sense on a monitor that refreshes that fast. The game times itself from this value.\n"
     "; auto: follow the frame rate the PC actually reaches, up to the monitor's refresh rate.\n"},
    {"Display", "resolution", "default",
     "; Screen resolution the game is shaped for. default = original game (16:9).\n"
     "; auto = this monitor's resolution, or e.g. 3440x1440, 2560x1080, 1280x800. The 3D view\n"
     "; takes exactly that shape and the picture fills the window (run fullscreen at that size);\n"
     "; the HUD keeps its 16:9 layout and is stretched with it (the HudFix mod corrects it).\n"},
    {"Display", "aspect", "16:9",
     "; Only used with resolution = default: shape of the 3D view as a ratio, 16:9 = original.\n"
     "; 21:9, 32:9, 16:10, 4:3 or a number such as 2.39.\n"},
    {"Display", "fullscreen", "false",
     "; true = start fullscreen. F11 or Alt+Enter switches while playing.\n"},
    {"Display", "show_fps", "false",
     "; true = show the frame rate in the window title. The game menu (F10) always shows it.\n"},
    {"Display", "console_mode", "docked",
     "; docked = the game renders at 1920x1080, as on a TV (original PC default).\n"
     "; handheld = the game renders at 1280x720, as on the Switch's own screen: much lighter on\n"
     "; weak PCs and integrated graphics. The picture is scaled up to the window either way;\n"
     "; native_render doesn't raise the handheld render size.\n"},
    {"Graphics", "native_render", "true",
     "; With a resolution set: the game renders at that resolution itself (up to 3840x2160)\n"
     "; instead of suyu scaling a 1920x1080 picture. Keep suyu's resolution scale at 1x then.\n"
     "; false = keep the 1920x1080 render (use suyu's resolution scale instead).\n"},
    {"Debug", "diagnostics", "false",
     "; Run reports for development (profiler, graphics call census, timing watch, file\n"
     "; monitor; written to the user folder). true = write them (a little slower).\n"},
    {"Debug", "rtss_overlay", "false",
     "; RivaTuner Statistics Server's Vulkan overlay crashes the game at start, so it is kept\n"
     "; out of this game (RivaTuner itself keeps working elsewhere). true = let it load.\n"},
    {"Graphics", "draw_distance", "1.0",
     "; Far clip plane multiplier: 1.0 = original game, up to 8.0. Moves the distance beyond which\n"
     "; nothing is drawn. Objects that fade out by their own distance checks aren't affected yet.\n"},
    {"Multiplayer", "mode", "off",
     "; Local play over a suyu room (e.g. through Radmin VPN). off | host | join: what to do at\n"
     "; start. Also in the game menu (F10), which saves its fields here.\n"},
    {"Multiplayer", "nickname", "Hunter",
     "; Your name in the room: 4-20 characters (letters, digits, space, . _ -).\n"},
    {"Multiplayer", "address", "",
     "; join: the host's address (their Radmin VPN IP, for example).\n"},
    {"Multiplayer", "port", "24872", "; Room port (host and join must match).\n"},
    {"Multiplayer", "password", "", "; Room password; empty = none.\n"},
    {"Multiplayer", "room_name", "MHGU", "; host: the room's name.\n"},
    {"Multiplayer", "max_players", "4", "; host: room size (2-16).\n"},
    {"Multiplayer", "auto_reconnect", "true",
     "; true = when the connection to a joined room drops, try again every 5 s for a minute.\n"},
    {"Multiplayer", "recent", "",
     "; Hosts joined before (address:port, newest first), offered in the game menu.\n"},
    {"Menu", "key", "F10",
     "; Opens the game menu window (controls, multiplayer, folders): a key name such as F10,\n"
     "; F9 or Home. none = no key. The F12 debug panel (Windows) stays as it is.\n"},
    {"Menu", "pad_combo", "back+start",
     "; Controller buttons held together for 1 s to open the menu (SDL names: back, start,\n"
     "; guide, leftstick, rightstick, leftshoulder, ...). back+start = Minus + Plus on a\n"
     "; Switch layout, View + Menu on Xbox. none = off.\n"},
    {"Mods", "loader", "Forge/forge.dll",
     "; Mod loader DLL, relative to mods/<title id>/ (install Forge PC there for code mods).\n"
     "; none = load no mod code (file replacements in mods/ still apply).\n"},
};

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
std::string ReadValue(const std::string& want_section, const std::string& want_key,
                      bool* present = nullptr) {
    std::ifstream in(IniPath());
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || section != want_section) {
            continue;
        }
        if (Trim(line.substr(0, eq)) == want_key) {
            std::string value = Trim(line.substr(eq + 1));
            if (const auto c = value.find(';'); c != std::string::npos) {
                value = Trim(value.substr(0, c));
            }
            if (present) {
                *present = true;
            }
            return value;
        }
    }
    return {};
}

/// Creates the ini, or appends the options an older one doesn't have yet.
void WriteMissingOptions() {
    const auto path = IniPath();
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    std::string add;
    std::string section;
    for (const Option& o : kOptions) {
        bool present = false;
        if (existed) {
            ReadValue(o.section, o.key, &present);
        }
        if (present) {
            continue;
        }
        if (section != o.section) {
            section = o.section;
            add += std::string("\n[") + o.section + "]\n";
        }
        add += std::string(o.comment) + o.key + " = " + o.value + "\n";
    }
    if (!existed) {
        std::ofstream(path) << kTemplate << add;
        LOG_INFO(Core_ARM, "game settings: wrote the defaults to {}", path.string());
    } else if (!add.empty()) {
        std::ofstream(path, std::ios::app) << "\n; Added by a newer build:" << add;
        LOG_INFO(Core_ARM, "game settings: added new options to {}", path.string());
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

} // namespace

std::atomic<u32> g_aspect_pc{0};

bool DiagnosticsEnabled() {
    static const bool enabled = [] {
        const std::string v = ReadValue("Debug", "diagnostics");
        return v == "true" || v == "1" || v == "yes" || v == "on";
    }();
    return enabled;
}

std::string Value(const std::string& section, const std::string& key) {
    return ReadValue(section, key);
}

void SetValue(const std::string& want_section, const std::string& want_key,
              const std::string& value) {
    WriteMissingOptions();
    std::vector<std::string> lines;
    {
        std::ifstream in(IniPath());
        for (std::string line; std::getline(in, line);) {
            lines.push_back(line);
        }
    }
    std::string section;
    std::ptrdiff_t section_end = -1; // insertion point: after the last line of the section
    bool done = false;
    for (std::size_t i = 0; i < lines.size() && !done; ++i) {
        const std::string t = Trim(lines[i]);
        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            section = Trim(t.substr(1, t.size() - 2));
            continue;
        }
        if (section != want_section) {
            continue;
        }
        section_end = static_cast<std::ptrdiff_t>(i) + 1;
        const auto eq = t.find('=');
        if (t.empty() || t[0] == ';' || t[0] == '#' || eq == std::string::npos) {
            continue;
        }
        if (Trim(t.substr(0, eq)) == want_key) {
            lines[i] = want_key + " = " + value;
            done = true;
        }
    }
    if (!done) {
        if (section_end < 0) {
            lines.push_back("");
            lines.push_back("[" + want_section + "]");
            section_end = static_cast<std::ptrdiff_t>(lines.size());
        }
        lines.insert(lines.begin() + section_end, want_key + " = " + value);
    }
    std::ofstream out(IniPath(), std::ios::trunc);
    for (const auto& line : lines) {
        out << line << '\n';
    }
}

std::vector<OptionInfo> Options() {
    std::vector<OptionInfo> out;
    for (const Option& o : kOptions) {
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
        out.push_back({o.section, o.key, o.value, std::move(comment)});
    }
    return out;
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

void SetDesktopMode(u32 width, u32 height, u32 refresh_hz) {
    g_desk_w = width;
    g_desk_h = height;
    g_desk_hz = refresh_hz > 1 ? refresh_hz : 0;
}
std::atomic<u32> g_render_default_pc{0};
std::atomic<u32> g_render_create_pc{0};

void Start(System& system, const std::map<u64, std::string>& modules) {
    static std::atomic<bool> started{false};
    if (system.GetApplicationProcessProgramID() != kMhgu || started.exchange(true)) {
        return; // other titles, or a second core registering the same process
    }
    g_system = &system;
    WriteMissingOptions();

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
    if (res_w != 0 && res_h != 0) {
        g_aspect = static_cast<float>(res_w) / static_cast<float>(res_h);
        stretch = true; // the view already has the window's exact shape: fill it
        if (IsTrue(ReadValue("Graphics", "native_render"))) {
            // Fit within what the engine is known to handle, keeping the shape.
            const float scale = std::min({1.0f, static_cast<float>(kMaxRenderW) / res_w,
                                          static_cast<float>(kMaxRenderH) / res_h});
            g_render_w = static_cast<u32>(res_w * scale) & ~1u;
            g_render_h = static_cast<u32>(res_h * scale) & ~1u;
        }
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
    if (main_base != 0) {
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

float DisplayRateScale() {
    const float top = g_fps_auto ? g_fps_cap : g_fps.load(std::memory_order_relaxed);
    return top > 60.0f ? top / 60.0f : 1.0f;
}

void OnFrame() {
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
