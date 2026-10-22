// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <map>
#include <set>

#include "core/arm/recomp/game_settings_ini.h"

namespace Core::GameSettings::Ini {

namespace {

#ifdef _WIN32
constexpr const char* kModLoader = "Forge/forge.dll";
#else
constexpr const char* kModLoader = "Forge/forge.so";
#endif

// layout 4: [Upscaler] nvn_fsr_quality / render_scale_override, Advanced-mode rows, [Graphics] draw_distance retired
// (layout 3: [Graphics] renderer = Emulated | Direct replaced native_gpu = false | true). An older file is converted once.
constexpr const char* kLayoutMarker = "; layout 4";

constexpr const char* kHeader =
    "; game_settings.ini: options for this exported game.\n"
    "; Edit a value, save, and restart the game. Nothing needs to be exported again.\n"
    "; Lines starting with ; are comments. A line like \"; key = value\" shows a default: remove the ; to change it.\n"
    "; layout 4: one section each. An older file is converted once; the old one is kept as game_settings.ini.bak.\n";

// Sections in file order; [Dev] always last, after any section this table does not know.
const char* const kOrder[] = {"Display", "Graphics", "Upscaler", "Present", "Diagnostics", "Multiplayer", "Menu", "Mods"};
constexpr const char* kDev = "Dev";

constexpr const char* D = "Debug"; // the section older files had these in

std::vector<Row> MakeRows() {
    using K = Kind;
    return {
        // ---- Display
        {"Display", "fps", "30",
         "; Target frame rate: 30 = original game, 60/120/240 for fast monitors; auto = follow what the PC reaches\n"
         "; (up to the monitor's refresh rate). The game times itself from it. Default 30.\n", K::Menu, nullptr},
        {"Display", "resolution", "default",
         "; Screen shape: default = original 16:9, auto = this monitor, or WxH such as 3440x1440 (run fullscreen at it).\n"
         "; The HUD keeps its 16:9 layout (the HudFix mod corrects it). Default default.\n", K::Menu, nullptr},
        {"Display", "aspect", "16:9",
         "; Only with resolution = default: shape of the 3D view (21:9, 32:9, 16:10, 4:3 or e.g. 2.39). Default 16:9.\n",
         K::Menu, nullptr},
        {"Display", "fullscreen", "false",
         "; true = start fullscreen (F11 or Alt+Enter switches). Default false.\n", K::Menu, nullptr},
        {"Display", "vsync", "on",
         "; on = locked to the refresh rate; mailbox = newest frame each refresh (fps above refresh);\n"
         "; off/false = no lock (can tear); relaxed = on, late frames at once. Needs a restart. Default on.\n", K::Menu, nullptr},
        {"Display", "show_fps", "false",
         "; true = frame rate in the window title (the F10 menu always shows it). Default false.\n", K::Menu, nullptr},
        {"Display", "confirm_exit", "true",
         "; true = ask \"Are you sure you want to exit?\" when the window is closed (X or Alt+F4). Default true.\n",
         K::Doc, nullptr},
        {"Display", "console_mode", "docked",
         "; docked = 1920x1080 render (TV); handheld = 1280x720 (lighter on weak PCs). Default docked.\n", K::Menu, nullptr},
        {"Display", "rtss_overlay", "false",
         "; RivaTuner's Vulkan overlay crashes this game at start, so it is kept out. true = let it load. Default false.\n",
         K::Menu, D},
        // ---- Graphics
        {"Graphics", "renderer", "Emulated",
         "; Emulated = suyu's own renderer (original behaviour); Direct = the replacement renderer: the game's NVN calls\n"
         "; replayed on Vulkan, own thread; FSR needs Direct. Needs a restart. Default Emulated.\n", K::Menu, nullptr},
        {"Graphics", "native_render", "true",
         "; With a resolution set: the game renders at it itself (render scale, up to 3840x2160) instead of a\n"
         "; 1920x1080 picture scaled up; keep suyu's resolution scale at 1x then. Default true.\n", K::Menu, nullptr},
        // retired from the menu and the file; a value an older file has is kept as a real line and still read
        {"Graphics", "draw_distance", nullptr,
         "; draw_distance (retired, not in the menu): far clip multiplier 1.0-8.0, still read when this line is present.\n",
         K::Dead, nullptr},
        // ---- Upscaler (replacement renderer only)
        // The menu's "Upscaler" row (Off | FSR 3.1 | FSR 4) reads and writes nvn_fsr + nvn_fsr_backend: they stay the stored truth.
        {"Upscaler", "nvn_fsr", "false",
         "; FSR upscaling on the replacement renderer (renderer = Direct). The game menu's Upscaler row sets this and\n"
         "; nvn_fsr_backend (Off = false; FSR 3.1 = true + 3; FSR 4 = true + 4). Numpad keys: see GPU-MD. Default false.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_backend", "3",
         "; 3 = FSR 3.1.4 (Vulkan); 4 = FSR 4 through the driver's DX12 library (falls back to 3.1). Default 3.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_quality", "Native AA",
         "; With an upscaler on (renderer = Direct): Native AA | Quality | Balanced | Performance | Ultra Performance. The mode\n"
         "; decides the render scale (ratios in game_settings.cpp; FSR upscales to the output, see nvn_fsr_split_scale)\n"
         "; unless render_scale_override is on. Needs a\n"
         "; restart. Default Native AA (the picture is drawn at the output size, FSR as anti-aliasing).\n", K::Menu, nullptr},
        {"Upscaler", "nvn_fsr_sharpness", "0.3", "; RCAS sharpening 0-1. Default 0.3.\n", K::Menu, D},
        {"Upscaler", "render_scale_override", "false",
         "; true = the saved Render scale (Graphics > Resolution scale) is used even with an upscaler on; the Quality mode no\n"
         "; longer decides it. Default false.\n", K::Advanced, nullptr},
        {"Upscaler", "nvn_fsr_split_scale", "false",
         "; true = Quality / Balanced / Performance / Ultra Performance draw the scene below the output and FSR upscales\n"
         "; it to the full output (HUD layer and present images stay at the output). Needs the game rendering at the\n"
         "; output itself (native_render with a resolution, or the default 1080p). false = the old single render scale\n"
         "; for everything (suyu's built-in render scale steps). Needs a restart. Default false.\n", K::Advanced, nullptr},
        {"Upscaler", "nvn_fsr_reactive", "true", "; Reactive mask (less ghosting on effects). Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_reactive_strength", "0.9", "; Reactive mask strength 0-1. Default 0.9.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_jitter_conv", "1", "; Jitter sign convention 0-3 (Numpad3). Default 1.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_hudlayer", "true", "; HUD drawn on its own layer over the upscaled picture. Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_indicator", "false", "; State squares in the top-left corner (development aid). Default false.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_hud_lo", "0.08", "; Diff-composite HUD mask lower edge 0-1. Default 0.08.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_hud_hi", "0.25", "; Diff-composite HUD mask upper edge 0-1. Default 0.25.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_motionmask", "false", "; Motion mask (shimmer on distant fine edges). Default false.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_objmv", "true", "; Block matching for object motion (scene frames). Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_objmv_near", "3000", "; Block-matching depth limit. Default 3000.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_objvel", "4",
         "; Geometry object vectors: 0 off, 1 mask draws, 2 + skinned, 3 skinned only, 4 all depth-tested. Default 4.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_objvel_reactive", "0.1", "; Reactive cap on texels with object vectors 0-1. Default 0.1.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_objvel_maxpx", "64", "; Largest trusted object motion in pixels. Default 64.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_objvel_maxverts", "4000000", "; Captured vertices per frame (10000-8000000). Default 4000000.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_objvel_static", "true", "; Static draws left out of the capture. Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_animsurf", "true", "; Animated surfaces (banners, flipbooks). Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_animsurf_thr", "0.03", "; Animated-surface threshold. Default 0.03.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_animsurf_edge_gate", "true", "; Flat-depth edge gate for animated surfaces. Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_animsurf_bypass", "true", "; Catmull-Rom bypass for animated surfaces. Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_menu3d", "true", "; Menu 3D (save screen, previews): snapshot + object vectors. Default true.\n", K::Doc, D},
        {"Upscaler", "nvn_fsr_menuskip", "false", "; Old rule: no FSR / jitter after 3 non-scene frames. Default false.\n", K::Doc, D},
        {"Upscaler", "nvn_mv_pass", "false", "; Camera motion vectors (always on with nvn_fsr). Default false.\n", K::Doc, D},
        {"Upscaler", "nvn_mv_lag", "1", "; Camera record lag 0-3 frames. Default 1.\n", K::Doc, D},
        // ---- Present (replacement renderer)
        {"Present", "present_nodrain", "true",
         "; true = the present does not wait for the GPU (CPU and GPU overlap, +1 frame latency); false = the old\n"
         "; draining present. Read at start. Default true.\n", K::Doc, D},
        {"Present", "pres_ring", "3", "; Present images 2-16 (raised to 4 with present_nodrain). Default 3.\n", K::Doc, D},
        {"Present", "present_fifo", "false", "; Finished copies queue in order, each composite takes the next (ring at least 6). Default false.\n", K::Doc, D},
        // ---- Diagnostics / probes
        {"Diagnostics", "diagnostics", "false",
         "; Run reports for every graphics mode (profiler, call census, timing watch, file monitor; a little slower).\n"
         "; Default false.\n", K::Advanced, D},
        {"Diagnostics", "gpu_diagnostics", "false",
         "; Replacement renderer reports: nvn_prof.txt / nvn_perf.txt timing (a little slower). Default false.\n", K::Advanced, D},
        {"Diagnostics", "log_redact_paths", "true",
         "; true = logs hide personal data (user folder, paths, user/computer name, IP addresses as <ip-N>) so they\n"
         "; can be shared. false = full paths and addresses (own debugging only). Next start. Default true.\n", K::Advanced, D},
        {"Diagnostics", "nvn_call_sampling", "false",
         "; Per-call NVN recording counts + first argument samples (nvn_rec.txt); costs CPU per NVN call. Default false.\n",
         K::Advanced, D},
        {"Diagnostics", "nvn_perf_gpu", "1", "; With gpu_diagnostics: 0 = no GPU timestamps (CPU part only). Default 1.\n", K::Advanced, D},
        {"Diagnostics", "nvn_fsr_texel_stats", nullptr,
         "; nvn_fsr_texel_stats: 1/0 = once-a-second texel readbacks for the log percentages (default: on with\n"
         "; gpu_diagnostics, off without).\n", K::Advanced, D},
        {"Diagnostics", "nvn_bench", "0",
         "; 1 = benchmark mode (needs gpu_diagnostics): Numpad * or - starts a run, Numpad + cycles the scene. Default 0.\n",
         K::Advanced, D},
        {"Diagnostics", "nvn_bench_scene", "village_stand", "; Benchmark scene label. Default village_stand.\n", K::Advanced, D},
        {"Diagnostics", "nvn_bench_warmup", "3", "; Benchmark warm-up seconds per state (1-30). Default 3.\n", K::Advanced, D},
        {"Diagnostics", "nvn_bench_measure", "15", "; Benchmark measured seconds per state (5-120). Default 15.\n", K::Advanced, D},
        {"Diagnostics", "nvn_upscaler_probe", "false", "; Render-target / uniform probes (file I/O every frame). Default false.\n",
         K::Advanced, D},
        {"Diagnostics", "vulkan_validation", "false", "; Vulkan validation layer (slow). Default false.\n", K::Advanced, D},
        {"Diagnostics", "probes", "false", "; Wiggle-hitch probes (stall ring, SVC/HLE/hook timing, frame lines). Default false.\n",
         K::Advanced, D},
        {"Diagnostics", "probe_pcs", "", "; Guest addresses to observe (comma separated). Default empty.\n", K::Doc, D},
        // ---- Multiplayer (unchanged)
        {"Multiplayer", "mode", "off",
         "; Local play over a suyu room (e.g. through Radmin VPN): off | host | join at start. The F10 menu saves here.\n",
         K::Menu, nullptr},
        {"Multiplayer", "nickname", "Hunter",
         "; Your name in suyu rooms and Ryujinx lobbies: 4-20 characters (letters, digits, space, . _ -).\n", K::Menu, nullptr},
        {"Multiplayer", "address", "", "; join: the host's address (their Radmin VPN IP, for example).\n", K::Menu, nullptr},
        {"Multiplayer", "port", "24872", "; Room port (host and join must match).\n", K::Menu, nullptr},
        {"Multiplayer", "password", "", "; Room password; empty = none.\n", K::Menu, nullptr},
        {"Multiplayer", "room_name", "MHGU", "; host: the room's name.\n", K::Menu, nullptr},
        {"Multiplayer", "max_players", "4", "; host: room size (2-16).\n", K::Menu, nullptr},
        {"Multiplayer", "auto_reconnect", "true",
         "; true = when a joined room drops, try again every 5 s for a minute.\n", K::Menu, nullptr},
        {"Multiplayer", "recent", "", "; Hosts joined before (address:port, newest first), offered in the game menu.\n",
         K::Menu, nullptr},
        {"Multiplayer", "backend", "room",
         "; room = a suyu room (the settings above); ryuldn = a RyuLdn master server (play with Ryujinx players). Read at start.\n",
         K::Menu, nullptr},
        {"Multiplayer", "online_server_host", "",
         "; Online server (NEX): IPv4 address or host name every Nintendo online name resolves to; empty = off (then\n"
         "; network_replacement_host applies). Ports are fixed by the game. Independent of the LAN / RyuLDN settings.\n"
         "; Read when the game connects (set it before booting).\n",
         K::Menu, nullptr},
        {"Multiplayer", "ryuldn_host", "ldn.ryujinx.app",
         "; ryuldn: the main server (host name or IP); ldn.ryujinx.app = Ryujinx's public server (empty = the same).\n",
         K::Menu, nullptr},
        {"Multiplayer", "ryuldn_port", "30456", "; ryuldn: the server's port (Ryujinx uses 30456).\n", K::Menu, nullptr},
        {"Multiplayer", "ryuldn_servers", "",
         "; ryuldn: more servers to list lobbies from, comma separated host or host:port.\n", K::Menu, nullptr},
        {"Multiplayer", "ryuldn_passphrase", "",
         "; ryuldn: empty = public lobbies; Ryujinx-xxxxxxxx (8 hex digits) = only players with the same passphrase.\n",
         K::Menu, nullptr},
        // ---- Menu
        {"Menu", "key", "F10", "; Key that opens the game menu (F10, F9, Home, ...); none = no key.\n", K::Menu, nullptr},
        {"Menu", "pad_combo", "back+start",
         "; Controller buttons held 1 s to open the menu (SDL names, e.g. back+start = Minus + Plus); none = off.\n",
         K::Menu, nullptr},
        {"Menu", "show_advanced", "false",
         "; Advanced mode. true = the game menu gets an Advanced tab with every emulator setting (most need a restart) and the\n"
         "; Settings tab shows the Diagnostics group and the advanced rows (Override render scale ...).\n", K::Menu, nullptr},
        // ---- Mods
        {"Mods", "loader", kModLoader,
         "; Mod loader library relative to mods/<title id>/ (Forge PC); none = no mod code (file mods still apply).\n",
         K::Menu, nullptr},
        // ---- Dev (bottom): development switches
        {kDev, "fence_hold", "false", "; Hold the guest's fence until the replay's GPU work for the submit is done (test). Default false.\n", K::Doc, D},
        {kDev, "order_probe", "false", "; Order probe (then F8): one line per present for 60 presents. Default false.\n", K::Doc, D},
        {kDev, "nvn_jitter_spike", "0", "; Old jitter test modes 0-2 (nvn_fsr wins). Default 0.\n", K::Doc, D},
        {kDev, "nvn_fsr_debug", "0", "; 1 = extra FSR hooks (Numpad keys, ini re-reads, draw lists), logging and timing (messages in nvn_fsr.txt). Default 0.\n", K::Doc, D},
        {kDev, "nvn_fsr_debug_checking", "0",
         "; WARNING: 1 = the AMD library's FFX debug checking; it makes FSR 4 reject the dispatch (ffxDispatch rc 6, falls back to FSR 3.1) on this build. Dev only, leave 0. Default 0.\n",
         K::Doc, D},
        {kDev, "nvn_fsr_capture_ts", "0",
         "; 1 = GPU timestamps around every object-velocity capture (needs gpu_diagnostics). Default 0.\n", K::Doc, D},
        {kDev, "nvn_fsr_bm_compact", "0", "; 1 = block matching as classify + compact search list. Default 0.\n", K::Doc, D},
        {kDev, "nvn_fsr_static_frames", "240",
         "; Frames a draw must match the camera before it counts as static (object velocity). Default 240.\n", K::Doc, D},
        {kDev, "nvn_fsr_snapdraw", "0", "; Forced snapshot draw (0 = auto; re-read while running). Default 0.\n", K::Doc, D},
        {kDev, "nvn_fsr_signals", "false", "; Per-slot pose-signal logging (expensive). Default false.\n", K::Doc, D},
        {kDev, "nvn_vk_debug", "0", "; Replacement renderer debug level. Default 0.\n", K::Doc, D},
        {kDev, "nvn_prescan", "1", "; Pipeline prescan of each submit. Default 1.\n", K::Doc, D},
        {kDev, "nvn_prescan_lazy", "0", "; Prescan from the first draw that misses a pipeline. Default 0.\n", K::Doc, D},
        {kDev, "nvn_prewarm", "1", "; Shader cache prewarm at start. Default 1.\n", K::Doc, D},
        {kDev, "nvn_unsafe_reads", "0", "; Skip rasterizer downloads on the render thread. Default 0.\n", K::Doc, D},
        {kDev, "nvn_spot_every", "1", "; Re-check cached texture contents at most every N frames (1 = every frame). Default 1.\n", K::Doc, D},
        {kDev, "nvn_workers", nullptr, "; nvn_workers: worker threads 0-256 (default: CPU threads - 1, at least 2).\n", K::Doc, D},
        {kDev, "descriptor_buffer", "false", "; Vulkan device: allow VK_EXT_descriptor_buffer. Default false.\n", K::Doc, D},
        {kDev, "multi_range_buffers", "false", "; suyu buffer cache: multi-range buffers. Default false.\n", K::Doc, D},
        {kDev, "fence_thread_sleep", "false", "; suyu GPU fencing thread polls with sleeps. Default false.\n", K::Doc, D},
        {kDev, "motion_blur", "off", "; off | probe | force (motion-blur filter hook). Default off.\n", K::Doc, D},
        // ---- read from older files only; superseded by [Graphics] renderer
        {kDev, "native_nvn", nullptr, "", K::Dead, D},
        {kDev, "nvn_vulkan", nullptr, "", K::Dead, D},
        {kDev, "suyu_render", nullptr, "", K::Dead, D},
    };
}

std::string Trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        start = end + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

bool IsHeader(const std::string& t) {
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}
bool IsComment(const std::string& t) {
    return !t.empty() && (t[0] == ';' || t[0] == '#');
}

/// "key = value" -> key, raw value (inline comment kept); false for anything else.
bool SplitKey(const std::string& t, std::string& key, std::string& value) {
    if (t.empty() || IsComment(t) || IsHeader(t)) return false;
    const auto eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = Trim(std::string_view(t).substr(0, eq));
    value = Trim(std::string_view(t).substr(eq + 1));
    return true;
}

std::string StripInline(const std::string& v) {
    if (const auto c = v.find(';'); c != std::string::npos) return Trim(std::string_view(v).substr(0, c));
    return v;
}

/// "; key = value": a default shown as a comment (SetKey turns it into the real line).
bool IsDocValueLine(const std::string& t, std::string_view key) {
    if (t.empty() || t[0] != ';') return false;
    size_t i = 1;
    while (i < t.size() && t[i] == ' ') ++i;
    if (t.compare(i, key.size(), key) != 0) return false;
    size_t j = i + key.size();
    while (j < t.size() && t[j] == ' ') ++j;
    return j < t.size() && t[j] == '=';
}

/// A "; key = ..." or "; key: ..." documentation line for this key.
bool IsDocLineFor(const std::string& t, std::string_view key) {
    if (t.empty() || t[0] != ';') return false;
    size_t i = 1;
    while (i < t.size() && t[i] == ' ') ++i;
    if (t.compare(i, key.size(), key) != 0) return false;
    const size_t j = i + key.size();
    return j < t.size() && (t[j] == ' ' || t[j] == '=' || t[j] == ':');
}

int OrderOf(const std::string& section) {
    for (int i = 0; i < static_cast<int>(std::size(kOrder)); ++i) {
        if (section == kOrder[i]) return i;
    }
    if (section == kDev) return 1000;
    return 500; // unknown sections: after Mods, before Dev
}

void AppendRow(std::vector<std::string>& out, const Row& r, const std::string* value) {
    for (const auto& c : SplitLines(r.comment)) out.push_back(c);
    if (value) {
        out.push_back(std::string(r.key) + " = " + *value);
    } else if (r.value == nullptr) {
        // comment only (no literal default)
    } else if (r.kind == Kind::Menu) {
        out.push_back(std::string(r.key) + " = " + r.value);
    } else {
        out.push_back(std::string("; ") + r.key + " = " + r.value);
    }
}

struct Parsed {
    struct Entry {
        std::string section; // canonical
        std::string key;
        std::string value; // raw (first occurrence)
        std::vector<std::string> ignored; // later occurrences with another value
    };
    std::vector<Entry> entries;               // file order of first occurrences
    std::map<std::pair<std::string, std::string>, size_t> index;
    std::vector<std::string> section_order;   // canonical sections in order of first appearance
    std::map<std::string, std::vector<std::string>> junk; // non-key, non-comment lines per raw section
};

Parsed Parse(const std::string& text) {
    Parsed p;
    std::string section;
    for (const auto& raw : SplitLines(text)) {
        const std::string t = Trim(raw);
        if (t.empty() || IsComment(t)) continue;
        if (IsHeader(t)) {
            section = Trim(std::string_view(t).substr(1, t.size() - 2));
            continue;
        }
        std::string key, value;
        if (!SplitKey(t, key, value)) {
            p.junk[section].push_back(t);
            if (std::find(p.section_order.begin(), p.section_order.end(), section) == p.section_order.end())
                p.section_order.push_back(section);
            continue;
        }
        const std::string canon = Canonical(section, key);
        if (std::find(p.section_order.begin(), p.section_order.end(), canon) == p.section_order.end())
            p.section_order.push_back(canon);
        const auto k = std::make_pair(canon, key);
        if (const auto it = p.index.find(k); it != p.index.end()) {
            auto& e = p.entries[it->second];
            if (StripInline(value) != StripInline(e.value)) e.ignored.push_back(value);
            continue;
        }
        p.index[k] = p.entries.size();
        p.entries.push_back({canon, key, value, {}});
    }
    return p;
}

} // namespace

const std::vector<Row>& Rows() {
    static const std::vector<Row> rows = MakeRows();
    return rows;
}

const Row* FindRow(std::string_view key) {
    for (const Row& r : Rows()) {
        if (key == r.key) return &r;
    }
    return nullptr;
}

std::string Canonical(std::string_view section, std::string_view key) {
    if (const Row* r = FindRow(key); r && (section == r->section || (r->legacy && section == r->legacy))) {
        return r->section;
    }
    return std::string(section);
}

bool IsCurrentLayout(const std::string& text) {
    for (const auto& l : SplitLines(text)) {
        const std::string t = Trim(l);
        if (IsHeader(t)) return false; // the marker is in the header comment, before the first section
        if (t.rfind(kLayoutMarker, 0) == 0) return true;
    }
    return false;
}

std::string Rebuild(const std::string& text, std::vector<std::string>* notes) {
    const Parsed p = Parse(text);
    const auto value_of = [&](const std::string& sec, const std::string& key) -> const Parsed::Entry* {
        const auto it = p.index.find({sec, key});
        return it == p.index.end() ? nullptr : &p.entries[it->second];
    };
    const Parsed::Entry* legacy_ng = value_of("Graphics", "native_gpu"); // layout 2: true | false
    const bool has_native_gpu = legacy_ng != nullptr || value_of("Graphics", "renderer") != nullptr; // decides the renderer
    bool has_dead = false; // a [Dev] legacy renderer key (superseded by [Graphics] renderer); retired options do not count
    for (const Row& r : Rows()) {
        if (r.kind == Kind::Dead && std::string_view(r.section) == kDev && value_of(r.section, r.key)) has_dead = true;
    }
    std::set<std::pair<std::string, std::string>> written;
    std::vector<std::string> out = SplitLines(kHeader);

    const auto write_extra = [&](const std::string& sec) {
        bool first = true;
        for (const auto& e : p.entries) {
            if (e.section != sec || written.contains({e.section, e.key})) continue;
            if (first) out.push_back("; not an option this build knows, kept as found:");
            first = false;
            out.push_back(e.key + " = " + e.value);
            written.insert({e.section, e.key});
            for (const auto& v : e.ignored) out.push_back("; ignored duplicate (the first one wins): " + e.key + " = " + v);
            if (notes) notes->push_back("kept unknown [" + sec + "] " + e.key);
        }
        if (const auto j = p.junk.find(sec); j != p.junk.end()) {
            for (const auto& l : j->second) out.push_back(l);
        }
    };
    const auto write_rows = [&](const std::string& sec) {
        for (const Row& r : Rows()) {
            if (sec != r.section) continue;
            const Parsed::Entry* e = value_of(r.section, r.key);
            if (r.kind == Kind::Dead) {
                if (!e) continue;
                if (std::string_view(r.section) != kDev) {
                    // a retired option (e.g. draw_distance): the user's value is kept as found and is still read
                    out.push_back(std::string("; retired option, no longer in the game menu; still read while this line is here:"));
                    out.push_back(std::string(r.key) + " = " + e->value);
                    written.insert({r.section, r.key});
                    for (const auto& v : e->ignored) out.push_back("; ignored duplicate (the first one wins): " + std::string(r.key) + " = " + v);
                    if (notes) notes->push_back(std::string("kept retired ") + r.key + " = " + e->value);
                    continue;
                }
                if (has_native_gpu) {
                    if (notes) notes->push_back(std::string("dropped ") + r.key + " = " + e->value +
                                                " (ignored while [Graphics] renderer is set; still in the backup)");
                    written.insert({r.section, r.key});
                    continue;
                }
                out.push_back(std::string("; legacy, read only while [Graphics] renderer is absent:"));
                out.push_back(std::string(r.key) + " = " + e->value);
                written.insert({r.section, r.key});
                continue;
            }
            std::string migrated;
            const bool is_renderer = std::string_view(r.key) == "renderer" && std::string_view(r.section) == "Graphics";
            if (is_renderer && legacy_ng) {
                // layout 2 -> 3: native_gpu = true | false becomes renderer = Direct | Emulated (an existing renderer wins)
                std::string l = StripInline(legacy_ng->value);
                for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const bool direct = l == "true" || l == "1" || l == "yes" || l == "on" || l == "direct";
                if (!e) {
                    migrated = direct ? "Direct" : "Emulated";
                    if (notes) notes->push_back("[Graphics] native_gpu = " + legacy_ng->value + " is now renderer = " + migrated);
                } else if (notes) {
                    notes->push_back("dropped [Graphics] native_gpu = " + legacy_ng->value + " ([Graphics] renderer is set)");
                }
                written.insert({"Graphics", "native_gpu"});
            }
            if (!e && migrated.empty() && r.kind == Kind::Menu && has_dead && is_renderer) {
                // An older file that still decides the renderer through the legacy keys: adding renderer = Emulated
                // would override them. Documented only.
                for (const auto& c : SplitLines(r.comment)) out.push_back(c);
                out.push_back(std::string("; ") + r.key + " = " + r.value);
                continue;
            }
            AppendRow(out, r, e ? &e->value : (migrated.empty() ? nullptr : &migrated));
            if (!e && !migrated.empty()) written.insert({r.section, r.key});
            if (e) {
                written.insert({r.section, r.key});
                for (const auto& v : e->ignored) {
                    out.push_back("; ignored duplicate (the first one wins): " + std::string(r.key) + " = " + v);
                    if (notes) notes->push_back(std::string("ignored duplicate ") + r.key + " = " + v + " (kept as a comment)");
                }
            }
        }
        write_extra(sec);
    };

    // Keys before any section header (never read): kept at the top as found.
    if (std::find(p.section_order.begin(), p.section_order.end(), std::string()) != p.section_order.end()) {
        write_extra("");
    }
    for (const char* sec : kOrder) {
        out.push_back("");
        out.push_back(std::string("[") + sec + "]");
        write_rows(sec);
    }
    std::vector<std::string> unknown;
    for (const auto& s : p.section_order) {
        if (!s.empty() && OrderOf(s) == 500) unknown.push_back(s);
    }
    for (const auto& s : unknown) {
        out.push_back("");
        out.push_back("[" + s + "]");
        write_extra(s);
        if (notes) notes->push_back("kept section [" + s + "] as found");
    }
    out.push_back("");
    out.push_back(std::string("[") + kDev + "]");
    out.push_back("; Development switches: leave them alone unless asked to change one.");
    write_rows(kDev);
    return Join(out);
}

namespace {

/// Line index after which a key of `section` (canonical) is inserted: the last non-blank line of the section's first
/// block; -1 = the section does not exist.
int SectionEnd(const std::vector<std::string>& lines, const std::string& section, int* header_at = nullptr) {
    std::string cur;
    bool in = false, seen = false;
    int last = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string t = Trim(lines[i]);
        if (IsHeader(t)) {
            if (in) return last; // end of the first block
            cur = Trim(std::string_view(t).substr(1, t.size() - 2));
            // headers are compared as written: the current layout writes the canonical names
            in = cur == section && !seen;
            if (in) {
                seen = true;
                last = i;
                if (header_at) *header_at = i;
            }
            continue;
        }
        if (in && !t.empty()) last = i;
    }
    return seen ? last : -1;
}

/// Where a new [section] goes: before the first header of a section that comes later in the order.
int NewSectionAt(const std::vector<std::string>& lines, const std::string& section) {
    const int want = OrderOf(section);
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string t = Trim(lines[i]);
        if (!IsHeader(t)) continue;
        if (OrderOf(Trim(std::string_view(t).substr(1, t.size() - 2))) > want) return i;
    }
    return static_cast<int>(lines.size());
}

void InsertInto(std::vector<std::string>& lines, const std::string& section, const std::vector<std::string>& add) {
    int end = SectionEnd(lines, section);
    if (end < 0) {
        int at = NewSectionAt(lines, section);
        std::vector<std::string> block;
        if (at > 0 && !Trim(lines[at - 1]).empty()) block.push_back("");
        block.push_back("[" + section + "]");
        block.insert(block.end(), add.begin(), add.end());
        if (at < static_cast<int>(lines.size())) block.push_back("");
        lines.insert(lines.begin() + at, block.begin(), block.end());
        return;
    }
    lines.insert(lines.begin() + end + 1, add.begin(), add.end());
}

/// The key's effective line, or a documentation line for it, in the key's canonical section.
struct Found {
    int real = -1, doc = -1;
};
Found FindKey(const std::vector<std::string>& lines, const std::string& canon, const std::string& key) {
    Found f;
    std::string section;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string t = Trim(lines[i]);
        if (IsHeader(t)) {
            section = Trim(std::string_view(t).substr(1, t.size() - 2));
            continue;
        }
        if (Canonical(section, key) != canon) continue;
        std::string k, v;
        if (SplitKey(t, k, v) && k == key) {
            f.real = i;
            return f;
        }
        if (f.doc < 0 && IsDocLineFor(t, key)) f.doc = i;
    }
    return f;
}

} // namespace

std::string AddMissing(const std::string& text, std::vector<std::string>* added) {
    std::vector<std::string> lines = SplitLines(text);
    bool has_dead = false;
    for (const Row& r : Rows()) {
        if (r.kind == Kind::Dead && std::string_view(r.section) == kDev && FindKey(lines, r.section, r.key).real >= 0) has_dead = true;
    }
    bool changed = false;
    for (const Row& r : Rows()) {
        if (r.kind == Kind::Dead) continue;
        const Found f = FindKey(lines, r.section, r.key);
        if (f.real >= 0 || f.doc >= 0) continue;
        std::vector<std::string> add;
        if (std::string_view(r.key) == "renderer" && FindKey(lines, "Graphics", "native_gpu").real >= 0) {
            continue; // a hand-added legacy native_gpu line still decides; a default renderer line would override it
        }
        if (r.kind == Kind::Menu && !(has_dead && std::string_view(r.key) == "renderer")) {
            AppendRow(add, r, nullptr);
        } else {
            for (const auto& c : SplitLines(r.comment)) add.push_back(c);
            if (r.value) add.push_back(std::string("; ") + r.key + " = " + r.value);
        }
        InsertInto(lines, r.section, add);
        changed = true;
        if (added) added->push_back(std::string("[") + r.section + "] " + r.key);
    }
    return changed ? Join(lines) : text;
}

std::string SetKey(const std::string& text, const std::string& section, const std::string& key,
                   const std::string& value) {
    std::vector<std::string> lines = SplitLines(text);
    const std::string canon = Canonical(section, key);
    const Found f = FindKey(lines, canon, key);
    const std::string line = key + " = " + value;
    if (f.real >= 0) {
        lines[f.real] = line;
    } else if (f.doc >= 0 && IsDocValueLine(Trim(lines[f.doc]), key)) {
        lines[f.doc] = line; // "; key = default" becomes the real line
    } else {
        InsertInto(lines, canon, {line});
    }
    return Join(lines);
}

std::string Lookup(const std::string& text, const std::string& section, const std::string& key, bool* present) {
    const std::string canon = Canonical(section, key);
    std::string cur;
    for (const auto& raw : SplitLines(text)) {
        const std::string t = Trim(raw);
        if (t.empty() || IsComment(t)) continue;
        if (IsHeader(t)) {
            cur = Trim(std::string_view(t).substr(1, t.size() - 2));
            continue;
        }
        std::string k, v;
        if (!SplitKey(t, k, v) || k != key || Canonical(cur, key) != canon) continue;
        if (present) *present = true;
        return StripInline(v);
    }
    if (present) *present = false;
    return {};
}

} // namespace Core::GameSettings::Ini
