// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <fmt/xchar.h>
// SDL3 removed these constants; define compat shims
static constexpr Uint8 SDL_PRESSED = 1;
static constexpr Uint8 SDL_RELEASED = 0;

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/arm/recomp/mod_host.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/mouse.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/main.h"
#include "common/param_package.h"
#include "common/settings_input.h"
#include "suyu_cmd/emu_window/emu_window_sdl2.h"
#include "suyu_cmd/multiplayer.h"
#include "suyu_cmd/sdl_software_keyboard.h"
#include "suyu_cmd/suyu_icon.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <array>
#include <filesystem>
#include <iterator>
#include <vector>
#include <system_error>
#include <string>
#include <vector>

// ── F12 debug panel ─────────────────────────────────────────────────────────
// A real interactive window (not a message box): live status text refreshed on
// a timer, a list of the mod folders currently visible to the patch manager,
// and buttons that open the folders this build actually uses. Deliberately
// carries no emulator branding — an exported game shows the game's own name.
namespace {

constexpr int kIdStatus = 1001;
constexpr int kIdMods = 1002;
constexpr int kIdOpenUser = 1003;
constexpr int kIdOpenMods = 1004;
constexpr int kIdOpenKeys = 1005;
constexpr int kIdClose = 1006;
constexpr int kIdDevices = 1007;
constexpr int kIdApplyPad = 1008;
constexpr int kIdKeyboard = 1009;
constexpr int kIdRescanPads = 1010;
constexpr int kIdBindList = 1011;
constexpr int kIdBindOne = 1012;
constexpr int kIdClearOne = 1013;
constexpr int kIdMpHost = 1014;
constexpr int kIdMpJoin = 1015;
constexpr int kIdMpLeave = 1016;
constexpr int kIdMpSave = 1017;
constexpr int kIdMpStartup = 1018;
constexpr UINT_PTR kTimer = 1;

std::filesystem::path DevExeDir() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    return std::filesystem::path(exe_path).parent_path();
}

std::wstring DevKeysDir() {
    return Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir).wstring();
}

void DevOpen(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    ShellExecuteW(nullptr, L"explore", p.wstring().c_str(), nullptr, nullptr, SW_SHOW);
}

struct DevPanelState {
    Core::System* system{};
    InputCommon::InputSubsystem* input{};
    HWND status{};
    HWND mods{};
    HWND devices{};
    HWND binds{};
    std::vector<Common::ParamPackage> device_list;
    // Multiplayer
    HWND mp_nick{};
    HWND mp_addr{};
    HWND mp_port{};
    HWND mp_pass{};
    HWND mp_room{};
    HWND mp_max{};
    HWND mp_startup{};
    HWND mp_status{};
    std::wstring mp_shown;
    ULONGLONG mp_hold_until{}; // a message stays this long before the live status returns
};

std::string DevText(HWND edit) {
    const int n = GetWindowTextLengthW(edit);
    std::wstring w(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(edit, w.data(), n + 1);
    w.resize(static_cast<std::size_t>(n));
    if (w.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(len) - 1);
    return out;
}

std::wstring DevWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    out.resize(static_cast<std::size_t>(len) - 1);
    return out;
}

/// The panel's multiplayer fields as a config (mode from the start-up choice).
Multiplayer::Config DevMpConfig(const DevPanelState& st) {
    Multiplayer::Config c = Multiplayer::Load();
    c.nickname = DevText(st.mp_nick);
    c.address = DevText(st.mp_addr);
    const unsigned long port = std::strtoul(DevText(st.mp_port).c_str(), nullptr, 10);
    c.port = static_cast<u16>(port >= 1 && port <= 65535 ? port : 24872);
    c.password = DevText(st.mp_pass);
    c.room_name = DevText(st.mp_room);
    const unsigned long max = std::strtoul(DevText(st.mp_max).c_str(), nullptr, 10);
    c.max_players = static_cast<u32>(max >= 2 && max <= 16 ? max : 4);
    static constexpr const char* kModes[] = {"off", "host", "join"};
    const auto sel = SendMessageW(st.mp_startup, CB_GETCURSEL, 0, 0);
    c.mode = kModes[sel >= 0 && sel < 3 ? sel : 0];
    if (c.nickname.empty()) {
        c.nickname = "Hunter";
    }
    if (c.room_name.empty()) {
        c.room_name = "MHGU";
    }
    return c;
}

void DevMpRefresh(DevPanelState& st) {
    if (GetTickCount64() < st.mp_hold_until) {
        return;
    }
    const std::wstring text = DevWide(Multiplayer::StatusText());
    if (text != st.mp_shown) {
        st.mp_shown = text;
        SetWindowTextW(st.mp_status, text.c_str());
    }
}

void DevMpMessage(DevPanelState& st, const std::string& message) {
    st.mp_shown = DevWide(message);
    st.mp_hold_until = GetTickCount64() + 4000;
    SetWindowTextW(st.mp_status, st.mp_shown.c_str());
}

// Per-button remapping. Auto-map covers the common case; this covers the rest -
// pick the entry, press the input you want, done. Same "press what you want"
// flow the emulator's own input dialog uses, driven off the input backend's
// polling API rather than a second mapping implementation.
void DevRefreshBinds(DevPanelState& st) {
    const int sel = static_cast<int>(SendMessageW(st.binds, LB_GETCURSEL, 0, 0));
    SendMessageW(st.binds, LB_RESETCONTENT, 0, 0);
    const auto& player = Settings::values.players.GetValue()[0];
    const auto add = [&](const char* label, const std::string& param) {
        Common::ParamPackage pkg{param};
        std::string shown = param.empty() ? "(unset)" : pkg.Get("display", param);
        if (shown.size() > 60) {
            shown.resize(60);
        }
        const std::string line = std::string(label) + "  =  " + shown;
        const std::wstring wide(line.begin(), line.end());
        SendMessageW(st.binds, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
    };
    for (std::size_t i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        add(Settings::NativeButton::mapping[i], player.buttons[i]);
    }
    for (std::size_t i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        add(Settings::NativeAnalog::mapping[i], player.analogs[i]);
    }
    if (sel >= 0) {
        SendMessageW(st.binds, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    }
}

void DevBindSelected(DevPanelState& st, bool clear) {
    const int sel = static_cast<int>(SendMessageW(st.binds, LB_GETCURSEL, 0, 0));
    constexpr int kButtonCount = static_cast<int>(Settings::NativeButton::NumButtons);
    constexpr int kAnalogCount = static_cast<int>(Settings::NativeAnalog::NumAnalogs);
    if (sel < 0 || sel >= kButtonCount + kAnalogCount || st.input == nullptr) {
        return;
    }
    const bool is_analog = sel >= kButtonCount;
    auto& player = Settings::values.players.GetValue()[0];

    if (clear) {
        if (is_analog) {
            player.analogs[sel - kButtonCount].clear();
        } else {
            player.buttons[sel].clear();
        }
        DevRefreshBinds(st);
        return;
    }

    st.input->BeginMapping(is_analog ? InputCommon::Polling::InputType::Stick
                                     : InputCommon::Polling::InputType::Button);
    // Poll rather than block: the panel owns the message loop, and a modal
    // "press something" dialog with no way out is worse than a timeout.
    Common::ParamPackage captured;
    const DWORD deadline = GetTickCount() + 5000;
    while (GetTickCount() < deadline) {
        captured = st.input->GetNextInput();
        if (captured.Has("engine")) {
            break;
        }
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
    st.input->StopMapping();

    if (!captured.Has("engine")) {
        MessageBoxW(nullptr, L"No input detected - nothing changed.", L"Controls",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (is_analog) {
        player.analogs[sel - kButtonCount] = captured.Serialize();
    } else {
        player.buttons[sel] = captured.Serialize();
    }
    player.connected = true;
    if (st.system != nullptr) {
        st.system->HIDCore().ReloadInputDevices();
    }
    DevRefreshBinds(st);
}

// Controller setup, done the way a player expects: pick the pad from a list and
// press one button. The full per-button remapper belongs in the emulator's own
// UI - what a shipped game build needs is for a plugged-in pad to just work,
// and a way back to keyboard when it does not.
void DevRefreshDevices(DevPanelState& st) {
    SendMessageW(st.devices, CB_RESETCONTENT, 0, 0);
    st.device_list.clear();
    if (st.input == nullptr) {
        return;
    }
    for (const auto& device : st.input->GetInputDevices()) {
        const std::string name = device.Get("display", device.Get("class", "Unknown"));
        if (name == "Any" || name == "Keyboard/Mouse") {
            continue;
        }
        st.device_list.push_back(device);
        const std::wstring wide(name.begin(), name.end());
        SendMessageW(st.devices, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
    }
    if (st.device_list.empty()) {
        SendMessageW(st.devices, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"(no controller detected - plug one in and rescan)"));
    }
    SendMessageW(st.devices, CB_SETCURSEL, 0, 0);
}

void DevApplyPadMapping(DevPanelState& st) {
    const auto index = static_cast<std::size_t>(SendMessageW(st.devices, CB_GETCURSEL, 0, 0));
    if (st.input == nullptr || index >= st.device_list.size()) {
        MessageBoxW(nullptr, L"No controller selected.", L"Controls", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto& device = st.device_list[index];
    // GetValue() hands back a reference to the live array, so the mappings are
    // written straight into the setting.
    auto& player = Settings::values.players.GetValue()[0];
    for (const auto& [button, param] : st.input->GetButtonMappingForDevice(device)) {
        player.buttons[button] = param.Serialize();
    }
    for (const auto& [analog, param] : st.input->GetAnalogMappingForDevice(device)) {
        player.analogs[analog] = param.Serialize();
    }
    for (const auto& [motion, param] : st.input->GetMotionMappingForDevice(device)) {
        player.motions[motion] = param.Serialize();
    }
    player.connected = true;
    if (st.system != nullptr) {
        st.system->HIDCore().ReloadInputDevices();
    }
    MessageBoxW(nullptr, L"Controller mapped to Player 1.", L"Controls",
                MB_OK | MB_ICONINFORMATION);
}

void DevApplyKeyboardMapping(DevPanelState& st) {
    // Same layout the emulator ships as its keyboard default.
    static constexpr std::array<int, Settings::NativeButton::NumButtons> kButtons = {
        SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_Z, SDL_SCANCODE_X,
        SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_M, SDL_SCANCODE_N,
        SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_B,
    };
    static constexpr std::array<std::array<int, 4>, Settings::NativeAnalog::NumAnalogs> kAnalogs{{
        {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT},
        {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L},
    }};
    // GetValue() hands back a reference to the live array, so the mappings are
    // written straight into the setting.
    auto& player = Settings::values.players.GetValue()[0];
    for (std::size_t i = 0; i < kButtons.size() && i < player.buttons.size(); ++i) {
        player.buttons[i] = InputCommon::GenerateKeyboardParam(kButtons[i]);
    }
    for (std::size_t i = 0; i < kAnalogs.size() && i < player.analogs.size(); ++i) {
        player.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
            kAnalogs[i][0], kAnalogs[i][1], kAnalogs[i][2], kAnalogs[i][3], 0, 0.5f);
    }
    player.connected = true;
    if (st.system != nullptr) {
        st.system->HIDCore().ReloadInputDevices();
    }
    MessageBoxW(nullptr, L"Keyboard controls restored for Player 1.", L"Controls",
                MB_OK | MB_ICONINFORMATION);
}

std::wstring DevStatusText(Core::System& system) {
    std::string game_name;
    [[maybe_unused]] auto _ = system.GetGameName(game_name);
    const auto perf = system.GetAndResetPerfStats();
    wchar_t buf[2048];
    const auto exe_dir = DevExeDir();
    swprintf(buf, std::size(buf),
             L"Title:        %hs\r\n"
             L"Title ID:     %016llX\r\n"
             L"FPS:          %.1f   Speed: %.0f%%   Frame: %.2f ms\r\n"
             L"CPU backend:  %hs\r\n"
             L"\r\n"
             L"User data:    %s\r\n"
             L"Mods:         %s\r\n"
             L"Keys:         %s\r\n",
             game_name.empty() ? "(not loaded)" : game_name.c_str(),
             static_cast<unsigned long long>(system.GetApplicationProcessProgramID()),
             perf.average_game_fps, perf.emulation_speed * 100.0, perf.frametime * 1000.0,
             g_native_export_mode ? "ArmRecomp (static recompiled modules)" : "dynarmic JIT",
             (exe_dir / L"user").wstring().c_str(), (exe_dir / L"mods").wstring().c_str(),
             DevKeysDir().c_str());
    return buf;
}

void DevRefreshMods(HWND list) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const auto mods_dir = DevExeDir() / L"mods";
    bool any = false;
    std::error_code ec;
    if (std::filesystem::is_directory(mods_dir, ec)) {
        for (const auto& tid : std::filesystem::directory_iterator(mods_dir, ec)) {
            if (!tid.is_directory()) {
                continue;
            }
            for (const auto& mod : std::filesystem::directory_iterator(tid.path(), ec)) {
                const std::wstring entry =
                    tid.path().filename().wstring() + L"  /  " + mod.path().filename().wstring();
                SendMessageW(list, LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(entry.c_str()));
                any = true;
            }
        }
    }
    if (!any) {
        SendMessageW(list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"(none - drop <title id>/<mod name>/ into mods/)"));
    }
}

LRESULT CALLBACK DevPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<DevPanelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_TIMER:
        if (st != nullptr && st->system != nullptr) {
            SetWindowTextW(st->status, DevStatusText(*st->system).c_str());
            DevMpRefresh(*st);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kIdOpenUser:
            DevOpen(DevExeDir() / L"user");
            return 0;
        case kIdOpenMods:
            DevOpen(DevExeDir() / L"mods");
            return 0;
        case kIdOpenKeys:
            DevOpen(DevKeysDir());
            return 0;
        case kIdClose:
            DestroyWindow(hwnd);
            return 0;
        case kIdMpHost:
        case kIdMpJoin:
            if (st != nullptr && st->system != nullptr) {
                const auto config = DevMpConfig(*st);
                std::string error;
                const bool ok = LOWORD(wp) == kIdMpHost
                                    ? Multiplayer::Host(*st->system, config, &error)
                                    : Multiplayer::Join(config, &error);
                if (!ok) {
                    DevMpMessage(*st, "Failed: " + error);
                }
            }
            return 0;
        case kIdMpLeave:
            Multiplayer::Leave();
            return 0;
        case kIdMpSave:
            if (st != nullptr) {
                const auto config = DevMpConfig(*st);
                Multiplayer::Save(config);
                DevMpMessage(*st, "Saved to game_settings.ini (on start: " + config.mode + ").");
            }
            return 0;
        case kIdRescanPads:
            if (st != nullptr) {
                DevRefreshDevices(*st);
            }
            return 0;
        case kIdApplyPad:
            if (st != nullptr) {
                DevApplyPadMapping(*st);
                DevRefreshBinds(*st);
            }
            return 0;
        case kIdKeyboard:
            if (st != nullptr) {
                DevApplyKeyboardMapping(*st);
                DevRefreshBinds(*st);
            }
            return 0;
        case kIdBindOne:
            if (st != nullptr) {
                DevBindSelected(*st, false);
            }
            return 0;
        case kIdClearOne:
            if (st != nullptr) {
                DevBindSelected(*st, true);
            }
            return 0;
        case kIdBindList:
            if (HIWORD(wp) == LBN_DBLCLK && st != nullptr) {
                DevBindSelected(*st, false);
            }
            return 0;
        case kIdMods:
            if (HIWORD(wp) == LBN_DBLCLK && st != nullptr) {
                DevRefreshMods(st->mods);
            }
            return 0;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimer);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowDevMenu(Core::System& system, InputCommon::InputSubsystem* input) {
    static bool registered = false;
    static const wchar_t* kClass = L"SuyuGameDebugPanel";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DevPanelProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        wc.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        RegisterClassExW(&wc);
        registered = true;
    }

    std::string game_name;
    [[maybe_unused]] auto _ = system.GetGameName(game_name);
    const std::wstring title =
        (game_name.empty() ? std::wstring(L"Game") : std::wstring(game_name.begin(), game_name.end())) +
        L" - Debug Panel (F12)";

    const HWND hwnd = CreateWindowExW(0, kClass, title.c_str(),
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                      CW_USEDEFAULT, 720, 940, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        return;
    }

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    DevPanelState state{};
    state.system = &system;
    state.input = input;
    state.status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                       ES_AUTOVSCROLL | WS_VSCROLL,
                                   10, 10, 690, 190, hwnd,
                                   reinterpret_cast<HMENU>(kIdStatus), inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Mods discovered under mods/ (double-click to rescan):",
                    WS_CHILD | WS_VISIBLE, 12, 208, 500, 18, hwnd, nullptr, inst, nullptr);
    state.mods = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 10, 228, 690,
                                 110, hwnd, reinterpret_cast<HMENU>(kIdMods), inst, nullptr);
    CreateWindowExW(0, L"STATIC", L"Controls - Player 1:", WS_CHILD | WS_VISIBLE, 12, 348, 140, 18,
                    hwnd, nullptr, inst, nullptr);
    state.devices = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, 150, 344,
                                    280, 200, hwnd, reinterpret_cast<HMENU>(kIdDevices), inst,
                                    nullptr);
    CreateWindowExW(0, L"BUTTON", L"Rescan", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 440, 344, 80,
                    26, hwnd, reinterpret_cast<HMENU>(kIdRescanPads), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Use controller", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 344,
                    172, 26, hwnd, reinterpret_cast<HMENU>(kIdApplyPad), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Use keyboard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 376,
                    172, 26, hwnd, reinterpret_cast<HMENU>(kIdKeyboard), inst, nullptr);
    CreateWindowExW(0, L"STATIC",
                    L"Pick an entry and press Rebind (or double-click), then press the input you want:",
                    WS_CHILD | WS_VISIBLE, 12, 410, 560, 18, hwnd, nullptr, inst, nullptr);
    state.binds = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 10, 430, 510,
                                  190, hwnd, reinterpret_cast<HMENU>(kIdBindList), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Rebind", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 430, 172,
                    28, hwnd, reinterpret_cast<HMENU>(kIdBindOne), inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 528, 464, 172,
                    28, hwnd, reinterpret_cast<HMENU>(kIdClearOne), inst, nullptr);
    // Multiplayer: host or join a room (e.g. over Radmin VPN); Save writes game_settings.ini.
    {
        const auto label = [&](const wchar_t* text, int x, int y, int w) {
            CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y + 4, w, 18, hwnd,
                            nullptr, inst, nullptr);
        };
        const auto edit = [&](const std::string& value, int x, int y, int w, DWORD extra) {
            return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", DevWide(value).c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra, x,
                                   y, w, 24, hwnd, nullptr, inst, nullptr);
        };
        const auto mp_button = [&](const wchar_t* text, int x, int y, int w, int id) {
            CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            x, y, w, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            inst, nullptr);
        };
        const Multiplayer::Config mp = Multiplayer::Load();
        CreateWindowExW(0, L"STATIC",
                        L"Multiplayer - local play through a room (for example over Radmin VPN):",
                        WS_CHILD | WS_VISIBLE, 12, 632, 600, 18, hwnd, nullptr, inst, nullptr);
        label(L"Nickname", 12, 656, 70);
        state.mp_nick = edit(mp.nickname, 85, 656, 150, 0);
        label(L"Host address", 245, 656, 80);
        state.mp_addr = edit(mp.address, 328, 656, 160, 0);
        label(L"Port", 498, 656, 30);
        state.mp_port = edit(std::to_string(mp.port), 530, 656, 70, ES_NUMBER);
        label(L"Password", 12, 686, 70);
        state.mp_pass = edit(mp.password, 85, 686, 150, ES_PASSWORD);
        label(L"Room name", 245, 686, 80);
        state.mp_room = edit(mp.room_name, 328, 686, 160, 0);
        label(L"Players", 498, 686, 45);
        state.mp_max = edit(std::to_string(mp.max_players), 548, 686, 52, ES_NUMBER);
        mp_button(L"Host", 10, 718, 100, kIdMpHost);
        mp_button(L"Join", 116, 718, 100, kIdMpJoin);
        mp_button(L"Leave", 222, 718, 100, kIdMpLeave);
        label(L"On start:", 340, 718, 55);
        state.mp_startup = CreateWindowExW(
            0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 398,
            720, 100, 120, hwnd, reinterpret_cast<HMENU>(kIdMpStartup), inst, nullptr);
        for (const wchar_t* m : {L"Off", L"Host", L"Join"}) {
            SendMessageW(state.mp_startup, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m));
        }
        SendMessageW(state.mp_startup, CB_SETCURSEL,
                     mp.mode == "host" ? 1 : (mp.mode == "join" ? 2 : 0), 0);
        mp_button(L"Save settings", 540, 718, 160, kIdMpSave);
        state.mp_status = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 10,
            752, 690, 76, hwnd, nullptr, inst, nullptr);
    }

    const auto button = [&](const wchar_t* text, int x, int id) {
        CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, 840, 160, 28,
                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    button(L"Open user data folder", 10, kIdOpenUser);
    button(L"Open mods folder", 180, kIdOpenMods);
    button(L"Open keys folder", 350, kIdOpenKeys);
    button(L"Resume", 540, kIdClose);

    const HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM f) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font));

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    SetWindowTextW(state.status, DevStatusText(system).c_str());
    DevRefreshMods(state.mods);
    DevRefreshDevices(state);
    DevRefreshBinds(state);
    DevMpRefresh(state);
    SetTimer(hwnd, kTimer, 500, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    // Modal to the game: emulation stays paused-ish while the panel is up, and
    // the panel gets its own pump so the live status keeps refreshing.
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

} // namespace
#endif

EmuWindow_SDL2::EmuWindow_SDL2(InputCommon::InputSubsystem* input_subsystem_, Core::System& system_)
    : input_subsystem{input_subsystem_}, system{system_} {
    input_subsystem->Initialize();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        LOG_CRITICAL(Frontend, "Failed to initialize SDL3: {}, Exiting...", SDL_GetError());
        exit(1);
    }
}

EmuWindow_SDL2::~EmuWindow_SDL2() {
    system.HIDCore().UnloadInputDevices();
    input_subsystem->Shutdown();
    SDL_Quit();
}

InputCommon::MouseButton EmuWindow_SDL2::SDLButtonToMouseButton(u32 button) const {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return InputCommon::MouseButton::Left;
    case SDL_BUTTON_RIGHT:
        return InputCommon::MouseButton::Right;
    case SDL_BUTTON_MIDDLE:
        return InputCommon::MouseButton::Wheel;
    case SDL_BUTTON_X1:
        return InputCommon::MouseButton::Backward;
    case SDL_BUTTON_X2:
        return InputCommon::MouseButton::Forward;
    default:
        return InputCommon::MouseButton::Undefined;
    }
}

std::pair<float, float> EmuWindow_SDL2::MouseToTouchPos(s32 touch_x, s32 touch_y) const {
    int w, h;
    SDL_GetWindowSize(render_window, &w, &h);
    const float fx = static_cast<float>(touch_x) / w;
    const float fy = static_cast<float>(touch_y) / h;

    return {std::clamp<float>(fx, 0.0f, 1.0f), std::clamp<float>(fy, 0.0f, 1.0f)};
}

void EmuWindow_SDL2::OnMouseButton(u32 button, u8 state, s32 x, s32 y) {
    const auto mouse_button = SDLButtonToMouseButton(button);
    if (state == SDL_PRESSED) {
        const auto [touch_x, touch_y] = MouseToTouchPos(x, y);
        input_subsystem->GetMouse()->PressButton(x, y, mouse_button);
        input_subsystem->GetMouse()->PressMouseButton(mouse_button);
        input_subsystem->GetMouse()->PressTouchButton(touch_x, touch_y, mouse_button);
    } else {
        input_subsystem->GetMouse()->ReleaseButton(mouse_button);
    }
}

void EmuWindow_SDL2::OnMouseMotion(s32 x, s32 y) {
    const auto [touch_x, touch_y] = MouseToTouchPos(x, y);
    input_subsystem->GetMouse()->Move(x, y, 0, 0);
    input_subsystem->GetMouse()->MouseMove(touch_x, touch_y);
    input_subsystem->GetMouse()->TouchMove(touch_x, touch_y);
}

void EmuWindow_SDL2::OnFingerDown(float x, float y, std::size_t id) {
    input_subsystem->GetTouchScreen()->TouchPressed(x, y, id);
}

void EmuWindow_SDL2::OnFingerMotion(float x, float y, std::size_t id) {
    input_subsystem->GetTouchScreen()->TouchMoved(x, y, id);
}

void EmuWindow_SDL2::OnFingerUp() {
    input_subsystem->GetTouchScreen()->ReleaseAllTouch();
}

void EmuWindow_SDL2::OnKeyEvent(int key, u8 state) {
#ifdef _WIN32
    if (state == SDL_PRESSED && key == SDL_SCANCODE_F12) {
        ShowDevMenu(system, input_subsystem);
        return;
    }
#endif
    if (state == SDL_PRESSED) {
        input_subsystem->GetKeyboard()->PressKey(static_cast<std::size_t>(key));
    } else if (state == SDL_RELEASED) {
        input_subsystem->GetKeyboard()->ReleaseKey(static_cast<std::size_t>(key));
    }
}

bool EmuWindow_SDL2::IsOpen() const {
    return is_open;
}

bool EmuWindow_SDL2::IsShown() const {
    return is_shown;
}

void EmuWindow_SDL2::OnResize() {
    int width, height;
    SDL_GetWindowSizeInPixels(render_window, &width, &height);
    // A minimized window reports 0x0. Feeding that through as a layout makes
    // the renderer build a zero-extent swapchain, which the driver never
    // presents from - the window comes back blank and the main loop stops
    // answering. Keep the last good layout instead; the next real resize (or
    // the restore) delivers correct dimensions.
    if (width <= 0 || height <= 0) {
        return;
    }
    UpdateCurrentFramebufferLayout(width, height);
}

void EmuWindow_SDL2::ShowCursor(bool show_cursor) {
    if (show_cursor) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

void EmuWindow_SDL2::Fullscreen() {
    switch (Settings::values.fullscreen_mode.GetValue()) {
    case Settings::FullscreenMode::Exclusive:
        // Set window size to render size before entering fullscreen -- SDL3 does not resize window
        // to display dimensions automatically in this mode.
        {
            const SDL_DisplayMode* display_mode =
                SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
            if (display_mode) {
                SDL_SetWindowSize(render_window, display_mode->w, display_mode->h);
            } else {
                LOG_ERROR(Frontend, "SDL_GetDesktopDisplayMode failed: {}", SDL_GetError());
            }
        }

        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Fullscreening failed: {}", SDL_GetError());
        LOG_INFO(Frontend, "Attempting to use borderless fullscreen...");
        [[fallthrough]];
    case Settings::FullscreenMode::Borderless:
        if (SDL_SetWindowFullscreen(render_window, true)) {
            return;
        }

        LOG_ERROR(Frontend, "Borderless fullscreening failed: {}", SDL_GetError());
        [[fallthrough]];
    default:
        // Fallback algorithm: Maximise window.
        // Works on all systems (unless something is seriously wrong), so no fallback for this one.
        LOG_INFO(Frontend, "Falling back on a maximised window...");
        SDL_MaximizeWindow(render_window);
        break;
    }
}

void EmuWindow_SDL2::ToggleFullscreen() {
    if ((SDL_GetWindowFlags(render_window) & SDL_WINDOW_FULLSCREEN) != 0) {
        SDL_SetWindowFullscreen(render_window, false);
    } else {
        Fullscreen();
    }
}

namespace {
/// Hands window input to the mod loader (mod_host_api.h). True if it consumed the event.
bool ForwardToMods(const SDL_Event& event) {
    ModHostInput in{};
    in.size = sizeof(in);
    in.version = MODHOST_API_VERSION;
    const auto mods = [](SDL_Keymod m) {
        return static_cast<uint16_t>(((m & SDL_KMOD_SHIFT) ? 1 : 0) | ((m & SDL_KMOD_CTRL) ? 2 : 0) |
                                     ((m & SDL_KMOD_ALT) ? 4 : 0) | ((m & SDL_KMOD_GUI) ? 8 : 0));
    };
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        in.type = MODHOST_INPUT_KEY;
        in.scancode = static_cast<int32_t>(event.key.scancode);
        in.down = event.key.down;
        in.repeat = event.key.repeat;
        in.mods = mods(event.key.mod);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.which == SDL_TOUCH_MOUSEID) {
            return false;
        }
        in.type = MODHOST_INPUT_MOUSE_MOVE;
        in.x = event.motion.x;
        in.y = event.motion.y;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.which == SDL_TOUCH_MOUSEID) {
            return false;
        }
        in.type = MODHOST_INPUT_MOUSE_BUTTON;
        in.button = event.button.button;
        in.down = event.button.down;
        in.x = event.button.x;
        in.y = event.button.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        in.type = MODHOST_INPUT_MOUSE_WHEEL;
        in.wheel_x = event.wheel.x;
        in.wheel_y = event.wheel.y;
        break;
    case SDL_EVENT_TEXT_INPUT:
        in.type = MODHOST_INPUT_TEXT;
        std::snprintf(in.text, sizeof(in.text), "%s", event.text.text ? event.text.text : "");
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        in.type = MODHOST_INPUT_FOCUS;
        in.down = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        Core::ModHost::OnInput(in);
        return false; // the window still needs these
    default:
        return false;
    }
    return Core::ModHost::OnInput(in);
}
} // namespace

void EmuWindow_SDL2::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    // A mod's menu asked for text input (typing into a field).
    static bool text_input_on = false;
    if (const bool want = Core::ModHost::TextInputWanted() || SdlKeyboard::Active();
        want != text_input_on) {
        text_input_on = want;
        if (want) {
            SDL_StartTextInput(render_window);
        } else {
            SDL_StopTextInput(render_window);
        }
    }

    if (!SDL_WaitEvent(&event)) {
        const char* error = SDL_GetError();
        if (!error || strcmp(error, "") == 0) {
            // https://github.com/libsdl-org/SDL/issues/5780
            // Sometimes SDL will return without actually having hit an error condition;
            // just ignore it in this case.
            return;
        }

        LOG_CRITICAL(Frontend, "SDL_WaitEvent failed: {}", error);
        exit(1);
    }

    // Input a mod's menu consumed doesn't reach the game (0 matches no case).
    // Then the game's software keyboard: while it wants text, typing goes to it.
    const bool consumed_by_mods = ForwardToMods(event) || SdlKeyboard::HandleEvent(event);
    switch (consumed_by_mods ? 0u : event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        // Restoring only ever raised RESTORED, never EXPOSED, so is_shown was
        // left false from the minimize and the renderer stayed parked - the
        // window came back black and eventually stopped responding.
        is_shown = true;
        OnResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        is_shown = false;
        OnResize();
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = true;
        OnResize();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        is_open = false;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.scancode == SDL_SCANCODE_F11 ||
            (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT) != 0)) {
            if (!event.key.repeat) {
                ToggleFullscreen();
            }
            break;
        }
        OnKeyEvent(static_cast<int>(event.key.scancode), SDL_PRESSED);
        break;
    case SDL_EVENT_KEY_UP:
        OnKeyEvent(static_cast<int>(event.key.scancode), SDL_RELEASED);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        // ignore if it came from touch
        if (event.motion.which != SDL_TOUCH_MOUSEID)
            OnMouseMotion(static_cast<s32>(event.motion.x), static_cast<s32>(event.motion.y));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        // ignore if it came from touch
        if (event.button.which != SDL_TOUCH_MOUSEID) {
            OnMouseButton(event.button.button,
                          event.button.down ? SDL_PRESSED : SDL_RELEASED,
                          static_cast<s32>(event.button.x), static_cast<s32>(event.button.y));
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
        OnFingerDown(event.tfinger.x, event.tfinger.y,
                     static_cast<std::size_t>(event.tfinger.fingerID));
        break;
    case SDL_EVENT_FINGER_MOTION:
        OnFingerMotion(event.tfinger.x, event.tfinger.y,
                       static_cast<std::size_t>(event.tfinger.fingerID));
        break;
    case SDL_EVENT_FINGER_UP:
        OnFingerUp();
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    default:
        break;
    }

    // While the game waits for typed text, the title shows it (the game draws no keyboard).
    static bool title_typing = false;
    if (const std::string typing = SdlKeyboard::TitleText(); !typing.empty()) {
        SDL_SetWindowTitle(render_window, typing.c_str());
        title_typing = true;
    } else if (title_typing) {
        title_typing = false;
        last_time = 0; // restore the normal title now
    }

    const u64 current_time = SDL_GetTicks();
    if (!title_typing && current_time > last_time + 2000) {
        const auto results = system.GetAndResetPerfStats();
        std::string game_name;
        [[maybe_unused]] auto _ = system.GetGameName(game_name);
        if (g_native_export_mode) {
            // Standalone game export: plain game title, no emulator branding.
            if (!game_name.empty()) {
                SDL_SetWindowTitle(render_window, game_name.c_str());
            }
        } else {
            const auto title =
                fmt::format("{} | {} | FPS: {:.0f} ({:.0f}%)", game_name.empty() ? "suyu" : game_name,
                            Common::g_build_fullname, results.average_game_fps,
                            results.emulation_speed * 100.0);
            SDL_SetWindowTitle(render_window, title.c_str());
        }
        last_time = current_time;
    }
}

#ifdef _WIN32
namespace {
HWND g_shader_box{};
} // namespace
#endif

void EmuWindow_SDL2::ShowShaderProgress(size_t built, size_t total) {
    SDL_PumpEvents();
    if (total == 0) {
        return; // still reading the cache file
    }
    std::string game_name;
    [[maybe_unused]] auto _ = system.GetGameName(game_name);
    const std::string text = fmt::format("Compiling shaders {} / {}", built, total);
    SDL_SetWindowTitle(render_window,
                       (game_name.empty() ? text : fmt::format("{} - {}", game_name, text)).c_str());
#ifdef _WIN32
    const auto owner = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(render_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!g_shader_box && owner) {
        RECT r{};
        GetWindowRect(owner, &r);
        constexpr int kW = 360, kH = 64;
        // An owned popup stays above the game window without being topmost system-wide.
        g_shader_box = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"",
                                       WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                       (r.left + r.right - kW) / 2, (r.top + r.bottom - kH) / 2, kW,
                                       kH, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (g_shader_box) {
            SendMessageW(g_shader_box, WM_SETFONT,
                         reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
    }
    if (g_shader_box) {
        const std::wstring wide = fmt::format(L"Compiling shaders {} / {}  (first start only)", built,
                                              total);
        SetWindowTextW(g_shader_box, wide.c_str());
    }
#endif
}

void EmuWindow_SDL2::HideShaderProgress() {
#ifdef _WIN32
    if (g_shader_box) {
        DestroyWindow(g_shader_box);
        g_shader_box = nullptr;
    }
#endif
    std::string game_name;
    [[maybe_unused]] auto _ = system.GetGameName(game_name);
    SDL_SetWindowTitle(render_window, game_name.empty() ? "suyu" : game_name.c_str());
    SDL_PumpEvents();
}

// Credits to Samantas5855 and others for this function.
void EmuWindow_SDL2::SetWindowIcon() {
#ifdef _WIN32
    // Native game exports embed the ROM's own icon into the exe's PE resources
    // (RT_GROUP_ICON id 1, see suyu/game_export.cpp) — use that instead of the
    // suyu logo so the window reads as the game, not the emulator.
    if (g_native_export_mode) {
        const HICON hicon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256,
                       LR_DEFAULTCOLOR));
        if (hicon != nullptr) {
            ICONINFO info{};
            if (GetIconInfo(hicon, &info)) {
                BITMAP bmp{};
                GetObjectW(info.hbmColor, sizeof(bmp), &bmp);
                const int w = bmp.bmWidth;
                const int h = bmp.bmHeight;
                std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h; // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                const HDC hdc = GetDC(nullptr);
                if (GetDIBits(hdc, info.hbmColor, 0, h, pixels.data(), &bi, DIB_RGB_COLORS)) {
                    // BGRA -> RGBA
                    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                        std::swap(pixels[i], pixels[i + 2]);
                    }
                    SDL_Surface* const icon_surface = SDL_CreateSurfaceFrom(
                        w, h, SDL_PIXELFORMAT_RGBA32, pixels.data(), w * 4);
                    if (icon_surface != nullptr) {
                        SDL_SetWindowIcon(render_window, icon_surface);
                        SDL_DestroySurface(icon_surface);
                        ReleaseDC(nullptr, hdc);
                        DeleteObject(info.hbmColor);
                        DeleteObject(info.hbmMask);
                        DestroyIcon(hicon);
                        return;
                    }
                }
                ReleaseDC(nullptr, hdc);
                DeleteObject(info.hbmColor);
                DeleteObject(info.hbmMask);
            }
            DestroyIcon(hicon);
        }
        LOG_WARNING(Frontend, "Native export: failed to load game icon from exe resources, "
                               "falling back to suyu icon.");
    }
#endif
    SDL_IOStream* const suyu_icon_stream = SDL_IOFromConstMem((void*)suyu_icon, suyu_icon_size);
    if (suyu_icon_stream == nullptr) {
        LOG_WARNING(Frontend, "Failed to create suyu icon stream.");
        return;
    }
    SDL_Surface* const window_icon = SDL_LoadBMP_IO(suyu_icon_stream, true);
    if (window_icon == nullptr) {
        LOG_WARNING(Frontend, "Failed to read BMP from stream.");
        return;
    }
    // The icon is attached to the window pointer
    SDL_SetWindowIcon(render_window, window_icon);
    SDL_DestroySurface(window_icon);
}

void EmuWindow_SDL2::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    SDL_SetWindowMinimumSize(render_window, minimal_size.first, minimal_size.second);
}
