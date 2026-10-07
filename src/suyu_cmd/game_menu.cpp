// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "common/fs/path_util.h"
#include "common/logging/log.h"
#include "common/settings_input.h"
#include "core/arm/recomp/game_settings.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "hid_core/frontend/emulated_controller.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/mouse.h"
#include "input_common/main.h"
#include "suyu_cmd/controls.h"
#include "suyu_cmd/game_menu.h"
#include "suyu_cmd/multiplayer.h"

namespace GameMenu {

namespace {

namespace GS = Core::GameSettings;
using Clock = std::chrono::steady_clock;
using HidController = Core::HID::EmulatedController;

constexpr auto kDrawInterval = std::chrono::milliseconds(16);
constexpr auto kComboHold = std::chrono::milliseconds(1000);
constexpr auto kCaptureDelay = std::chrono::milliseconds(300); // let the "press" button go up
constexpr auto kCaptureTimeout = std::chrono::seconds(6);
constexpr auto kNoteTime = std::chrono::seconds(6);

const ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kYellow{0.95f, 0.80f, 0.35f, 1.0f};
const ImVec4 kRed{0.95f, 0.45f, 0.40f, 1.0f};
const ImVec4 kGrey{0.65f, 0.65f, 0.68f, 1.0f};

Core::System* g_system{};
InputCommon::InputSubsystem* g_input{};
SDL_Window* g_game_window{};
SDL_Window* g_window{};
SDL_Renderer* g_renderer{};
ImGuiContext* g_imgui{};
bool g_focused{};
bool g_close_requested{};
Clock::time_point g_last_draw{};

SDL_Scancode g_key{SDL_SCANCODE_F10};
std::string g_key_name{"F10"};
std::vector<SDL_GamepadButton> g_combo;
std::string g_combo_name;
std::vector<SDL_Gamepad*> g_pads; // opened for the combo check (SDL counts references)
bool g_combo_held{};
bool g_combo_fired{};
Clock::time_point g_combo_since{};

struct Capture {
    bool active{};
    bool polling{};
    bool analog{};
    std::size_t index{};
    std::string label;
    Clock::time_point started{};
};
Capture g_capture;

std::string g_note;
Clock::time_point g_note_until{};

int g_device_pick{};
std::string g_fps_text{"-"};

// Multiplayer fields
char g_nick[32]{};
char g_invite[160]{};
char g_room[64]{};
char g_pass[64]{};
int g_port{24872};
int g_max_players{4};
int g_startup{}; // 0 off, 1 host, 2 join
bool g_reconnect{true};
std::string g_mp_message;
// Addresses stay hidden until clicked (streams, screenshots); hidden again each time the menu
// opens.
std::set<std::size_t> g_shown_addresses; // own addresses (host section) shown by index
bool g_show_join_address{};              // join field + recent hosts
bool g_mp_message_bad{};

template <std::size_t N>
void CopyTo(char (&dst)[N], const std::string& src) {
    std::snprintf(dst, N, "%s", src.c_str());
}

template <std::size_t N>
void CopyTo(std::array<char, N>& dst, const std::string& src) {
    std::snprintf(dst.data(), N, "%s", src.c_str());
}

struct ButtonRow {
    int index;
    const char* label;
};
constexpr ButtonRow kButtonRows[] = {
    {Settings::NativeButton::A, "A"},
    {Settings::NativeButton::B, "B"},
    {Settings::NativeButton::X, "X"},
    {Settings::NativeButton::Y, "Y"},
    {Settings::NativeButton::L, "L"},
    {Settings::NativeButton::R, "R"},
    {Settings::NativeButton::ZL, "ZL"},
    {Settings::NativeButton::ZR, "ZR"},
    {Settings::NativeButton::Plus, "Plus (+)"},
    {Settings::NativeButton::Minus, "Minus (-)"},
    {Settings::NativeButton::DUp, "D-pad up"},
    {Settings::NativeButton::DDown, "D-pad down"},
    {Settings::NativeButton::DLeft, "D-pad left"},
    {Settings::NativeButton::DRight, "D-pad right"},
    {Settings::NativeButton::LStick, "Left stick press"},
    {Settings::NativeButton::RStick, "Right stick press"},
    {Settings::NativeButton::Home, "Home"},
    {Settings::NativeButton::Screenshot, "Capture"},
};

void UpdateGate() {
    HidController::SetInputBlocked(HidController::BlockerMenu, g_window != nullptr && g_focused);
}

void ReadMenuSettings() {
    std::string key = GS::Value("Menu", "key");
    if (key.empty()) {
        key = "F10";
    }
    if (key == "none" || key == "off") {
        g_key = SDL_SCANCODE_UNKNOWN;
        g_key_name.clear();
    } else {
        g_key = SDL_GetScancodeFromName(key.c_str());
        g_key_name = g_key == SDL_SCANCODE_UNKNOWN ? std::string{} : key;
        if (g_key == SDL_SCANCODE_UNKNOWN) {
            LOG_WARNING(Frontend, "game menu: unknown key name '{}' in [Menu] key", key);
        }
    }

    std::string combo = GS::Value("Menu", "pad_combo");
    if (combo.empty()) {
        combo = "back+start";
    }
    g_combo.clear();
    g_combo_name.clear();
    if (combo != "none" && combo != "off") {
        std::size_t start = 0;
        while (start <= combo.size()) {
            const auto plus = combo.find('+', start);
            const std::string name = combo.substr(
                start, plus == std::string::npos ? std::string::npos : plus - start);
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
            if (b == SDL_GAMEPAD_BUTTON_INVALID) {
                LOG_WARNING(Frontend, "game menu: unknown button '{}' in [Menu] pad_combo", name);
                g_combo.clear();
                break;
            }
            g_combo.push_back(b);
            if (plus == std::string::npos) {
                break;
            }
            start = plus + 1;
        }
        if (!g_combo.empty()) {
            g_combo_name = combo;
        }
    }
}

void LoadMultiplayerFields() {
    const Multiplayer::Config c = Multiplayer::Load();
    CopyTo(g_nick, c.nickname);
    CopyTo(g_room, c.room_name);
    CopyTo(g_pass, c.password);
    g_port = c.port;
    g_max_players = static_cast<int>(c.max_players);
    g_startup = c.mode == "host" ? 1 : (c.mode == "join" ? 2 : 0);
    if (g_invite[0] == '\0' && !c.address.empty()) {
        CopyTo(g_invite, fmt::format("{}:{}", c.address, c.port));
    }
    g_reconnect = Multiplayer::AutoReconnect();
}

/// The menu's fields as a config (address etc. from the last saved one).
Multiplayer::Config FieldsToConfig() {
    Multiplayer::Config c = Multiplayer::Load();
    c.nickname = g_nick;
    c.room_name = g_room[0] != '\0' ? std::string(g_room) : std::string("MHGU");
    c.password = g_pass;
    c.port = static_cast<u16>(std::clamp(g_port, 1, 65535));
    c.max_players = static_cast<u32>(std::clamp(g_max_players, 2, 16));
    static constexpr const char* kModes[] = {"off", "host", "join"};
    c.mode = kModes[std::clamp(g_startup, 0, 2)];
    return c;
}

void MpMessage(const std::string& text, bool bad) {
    g_mp_message = text;
    g_mp_message_bad = bad;
}

void OpenPath(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        std::filesystem::create_directories(p, ec);
    }
    // A file URL (spaces and the like escaped) opens the folder or file with the system's
    // own handler on Windows and Linux alike.
    std::string url = "file://";
    std::string generic = Common::FS::PathToUTF8String(p);
    std::replace(generic.begin(), generic.end(), '\\', '/');
    if (!generic.empty() && generic.front() != '/') {
        url += '/';
    }
    for (const char ch : generic) {
        if (ch == ' ' || ch == '#' || ch == '%' || ch == '?') {
            url += fmt::format("%{:02X}", static_cast<unsigned char>(ch));
        } else {
            url += ch;
        }
    }
    if (!SDL_OpenURL(url.c_str())) {
        LOG_WARNING(Frontend, "game menu: couldn't open {}: {}", url, SDL_GetError());
    }
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SectionTitle(const char* text) {
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

// ── Controls ────────────────────────────────────────────────────────────────

void StartCapture(bool analog, std::size_t index, const char* label) {
    g_capture = {};
    g_capture.active = true;
    g_capture.analog = analog;
    g_capture.index = index;
    g_capture.label = label;
    g_capture.started = Clock::now();
}

void EndCapture() {
    if (g_capture.polling && g_input != nullptr) {
        g_input->StopMapping();
    }
    g_capture = {};
}

void PollCapture() {
    if (!g_capture.active || g_input == nullptr) {
        return;
    }
    const auto now = Clock::now();
    if (!g_capture.polling) {
        if (now - g_capture.started < kCaptureDelay) {
            return;
        }
        g_input->BeginMapping(g_capture.analog ? InputCommon::Polling::InputType::Stick
                                               : InputCommon::Polling::InputType::Button);
        g_capture.polling = true;
    }
    const Common::ParamPackage captured = g_input->GetNextInput();
    if (captured.Has("engine")) {
        const bool analog = g_capture.analog;
        const std::size_t index = g_capture.index;
        const std::string label = g_capture.label;
        EndCapture();
        Controls::SetBinding(analog, index, captured.Serialize());
        Notify(fmt::format("{} = {}", label, Controls::BindingText(analog, index)));
    } else if (now - g_capture.started > kCaptureTimeout) {
        EndCapture();
        Notify("Nothing pressed - unchanged");
    }
}

void DrawControls() {
    ImGui::Text("Player 1 uses:");
    ImGui::SameLine();
    ImGui::TextColored(kGreen, "%s", Controls::PlayerOneText().c_str());
    ImGui::TextColored(kGrey, "Set: %s", Controls::SetByText().c_str());
    HelpMarker("A controller you plug in is used automatically as long as you haven't chosen "
               "your controls yourself. Anything you change here counts as your choice and is "
               "never replaced; 'Back to automatic' undoes that.");

    SectionTitle("Controller");
    const auto devices = Controls::Devices();
    if (devices.empty()) {
        ImGui::TextColored(kYellow, "No controller connected.");
        ImGui::TextWrapped("Plug one in: it's picked up automatically unless you chose your "
                           "controls yourself (then pick it here).");
    } else {
        g_device_pick = std::clamp(g_device_pick, 0, static_cast<int>(devices.size()) - 1);
        const std::string current =
            devices[static_cast<std::size_t>(g_device_pick)].Get("display", "Controller");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        if (ImGui::BeginCombo("##device", current.c_str())) {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const std::string name = devices[i].Get("display", "Controller");
                const bool selected = static_cast<int>(i) == g_device_pick;
                if (ImGui::Selectable(fmt::format("{}##{}", name, i).c_str(), selected)) {
                    g_device_pick = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Use this controller")) {
            Controls::UseDevice(devices[static_cast<std::size_t>(g_device_pick)], "player");
            Notify("Player 1: " + current);
        }
    }
    if (ImGui::Button("Use keyboard (default keys)")) {
        Controls::UseDefaultKeyboard("player");
        Notify("Player 1: keyboard");
    }
    ImGui::SameLine();
    if (ImGui::Button("Back to automatic")) {
        Controls::ResetToAutomatic();
        Notify("Controls: automatic (" + Controls::PlayerOneText() + ")");
    }
    HelpMarker("Default keys, and the first connected controller is used now and whenever "
               "one is plugged in.");

    SectionTitle("Buttons");
    if (ImGui::Button("Swap A and B")) {
        Controls::SwapButtons(Settings::NativeButton::A, Settings::NativeButton::B);
        Notify("Swapped A and B");
    }
    ImGui::SameLine();
    if (ImGui::Button("Swap X and Y")) {
        Controls::SwapButtons(Settings::NativeButton::X, Settings::NativeButton::Y);
        Notify("Swapped X and Y");
    }
    HelpMarker("The game uses Nintendo's layout: A is the right face button. Swap if you want "
               "the bottom button to confirm, as on Xbox/PlayStation pads.");

    const float table_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("binds", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH,
                          ImVec2(0.0f, table_height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Switch input", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 9.0f);
        ImGui::TableSetupColumn("Bound to");
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 8.0f);
        ImGui::TableHeadersRow();
        const auto row = [](bool analog, std::size_t index, const char* label) {
            ImGui::PushID(static_cast<int>(index) + (analog ? 1000 : 0));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Controls::BindingText(analog, index).c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Change")) {
                StartCapture(analog, index, label);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                Controls::SetBinding(analog, index, {});
            }
            ImGui::PopID();
        };
        row(true, Settings::NativeAnalog::LStick, "Left stick");
        row(true, Settings::NativeAnalog::RStick, "Right stick");
        for (const auto& b : kButtonRows) {
            row(false, static_cast<std::size_t>(b.index), b.label);
        }
        ImGui::EndTable();
    }

    if (g_capture.active) {
        ImGui::OpenPopup("Press an input");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Press an input", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (!g_capture.active) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("%s", g_capture.label.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted(g_capture.analog
                                       ? "Move the stick you want for this, all the way once."
                                       : "Press the button or key you want for this.");
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                kCaptureTimeout - (Clock::now() - g_capture.started));
            ImGui::TextColored(kGrey, "Esc cancels (%lld s left)",
                               static_cast<long long>(std::max<std::int64_t>(0, left.count())));
        }
        ImGui::EndPopup();
    }
}

// ── Multiplayer ─────────────────────────────────────────────────────────────

void DoJoin() {
    Multiplayer::Config c = FieldsToConfig();
    if (!Multiplayer::ParseInvite(g_invite, &c)) {
        MpMessage("Enter the host's invite or address first (e.g. 26.12.34.56:24872).", true);
        return;
    }
    std::string error;
    if (Multiplayer::Join(c, &error)) {
        Multiplayer::Save(c);
        CopyTo(g_pass, c.password);
        g_port = c.port;
        MpMessage(g_show_join_address ? fmt::format("Joining {}:{} ...", c.address, c.port)
                                      : std::string("Joining ..."),
                  false);
    } else {
        MpMessage("Couldn't join: " + error, true);
    }
}

void DrawMultiplayer() {
    const Multiplayer::Status st = Multiplayer::GetStatus();
    using Phase = Multiplayer::Status::Phase;
    switch (st.phase) {
    case Phase::Offline:
        ImGui::TextColored(kGrey, "Offline");
        break;
    case Phase::Connecting:
        ImGui::TextColored(kYellow, "Connecting...");
        break;
    case Phase::Reconnecting:
        ImGui::TextColored(kYellow, "Connection lost - reconnecting (try %d)",
                           st.reconnect_attempt);
        break;
    case Phase::Hosting:
    case Phase::Joined:
        ImGui::TextColored(kGreen, "%s '%s'  -  %zu/%u players  -  you are %s",
                           st.phase == Phase::Hosting ? "Hosting" : "Joined", st.room.c_str(),
                           st.members.size(), st.slots, st.nickname.c_str());
        break;
    }
    if (st.phase != Phase::Offline) {
        ImGui::SameLine();
        if (ImGui::Button(st.phase == Phase::Hosting ? "Close room" : "Leave")) {
            Multiplayer::Leave();
            MpMessage("Left the room.", false);
        }
    }
    for (const auto& m : st.members) {
        ImGui::BulletText("%s", m.c_str());
    }
    if (!st.last_error.empty()) {
        ImGui::TextColored(kRed, "Last problem: %s", st.last_error.c_str());
    }
    if (!g_mp_message.empty()) {
        ImGui::TextColored(g_mp_message_bad ? kRed : kGrey, "%s", g_mp_message.c_str());
    }

    SectionTitle("Join a friend");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##invite", "invite or address, e.g. 26.12.34.56", g_invite, sizeof(g_invite),
        ImGuiInputTextFlags_EnterReturnsTrue |
            (g_show_join_address ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password));
    ImGui::SameLine();
    if (ImGui::Button(g_show_join_address ? "Hide" : "Show")) {
        g_show_join_address = !g_show_join_address;
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste")) {
        if (char* clip = SDL_GetClipboardText()) {
            CopyTo(g_invite, clip);
            SDL_free(clip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Join") || enter) {
        DoJoin();
    }
    HelpMarker("Paste the invite your friend copied in their menu (address:port, plus #password "
               "if the room has one), or type their Radmin VPN address.");
    const auto recent = Multiplayer::RecentHosts();
    if (!recent.empty()) {
        ImGui::TextColored(kGrey, "Joined before:");
        for (std::size_t i = 0; i < recent.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string name =
                g_show_join_address ? recent[i] : fmt::format("Host {}", i + 1);
            if (ImGui::Button(name.c_str())) {
                CopyTo(g_invite, recent[i]);
                DoJoin();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                Multiplayer::ForgetHost(recent[i]);
            }
            ImGui::PopID();
            if (i + 1 < recent.size()) {
                ImGui::SameLine(0.0f, ImGui::GetFontSize());
            }
        }
    }

    SectionTitle("Host a room");
    const float field = ImGui::GetFontSize() * 9.0f;
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Room name", g_room, sizeof(g_room));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Password", g_pass, sizeof(g_pass), ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(field);
    ImGui::SliderInt("Players", &g_max_players, 2, 8);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field);
    ImGui::InputInt("Port", &g_port, 0, 0);
    if (ImGui::Button("Host")) {
        const Multiplayer::Config c = FieldsToConfig();
        std::string error;
        if (g_system != nullptr && Multiplayer::Host(*g_system, c, &error)) {
            Multiplayer::Save(c);
            MpMessage("Room open. Send a friend one of the invites below.", false);
        } else {
            MpMessage("Couldn't host: " + error, true);
        }
    }
    ImGui::TextColored(kGrey, "Friends join with one of your addresses (Copy puts an invite on "
                              "the clipboard; click an address to show it):");
    const auto addresses = Multiplayer::LocalAddresses();
    bool any_vpn = false;
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        const auto& a = addresses[i];
        any_vpn |= a.label == "Radmin VPN" || a.label == "Hamachi" || a.label == "ZeroTier" ||
                   a.label == "Tailscale";
        ImGui::PushID(static_cast<int>(i) + 5000);
        if (ImGui::SmallButton("Copy invite")) {
            const std::string code =
                Multiplayer::InviteCode(a.ip, static_cast<u16>(std::clamp(g_port, 1, 65535)), g_pass);
            ImGui::SetClipboardText(code.c_str());
            Notify("Invite copied to the clipboard");
        }
        ImGui::SameLine();
        const bool shown = g_shown_addresses.contains(i);
        if (ImGui::SmallButton(shown ? a.ip.c_str() : "show address")) {
            if (shown) {
                g_shown_addresses.erase(i);
            } else {
                g_shown_addresses.insert(i);
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(a.label.c_str());
        ImGui::PopID();
    }
    if (!any_vpn) {
        ImGui::TextColored(kYellow, "No Radmin VPN address found.");
        HelpMarker("Over the internet, both of you install Radmin VPN and join the same Radmin "
                   "network; then copy the Radmin invite here. On the same home network the "
                   "Home network address works too.");
    }

    SectionTitle("You");
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("Nickname", g_nick, sizeof(g_nick));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Multiplayer::Save(FieldsToConfig());
    }
    HelpMarker("4-20 characters: letters, digits, space, . _ -");
    ImGui::TextUnformatted("When the game starts:");
    ImGui::SameLine();
    bool startup_changed = ImGui::RadioButton("Stay offline", &g_startup, 0);
    ImGui::SameLine();
    startup_changed |= ImGui::RadioButton("Host", &g_startup, 1);
    ImGui::SameLine();
    startup_changed |= ImGui::RadioButton("Join the last host", &g_startup, 2);
    if (startup_changed) {
        Multiplayer::Save(FieldsToConfig());
    }
    if (ImGui::Checkbox("Reconnect by itself when the connection drops", &g_reconnect)) {
        Multiplayer::SetAutoReconnect(g_reconnect);
    }

    const auto events = Multiplayer::Events();
    if (!events.empty()) {
        SectionTitle("Room events");
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            ImGui::TextColored(kGrey, "%s", it->c_str());
        }
    }
}

// ── Settings (game_settings.ini) ───────────────────────────────────────────

struct SettingHint {
    const char* section;
    const char* key;
    const char* label;
    const char* presets; // '|' separated, offered in a drop-down beside the field
};

constexpr SettingHint kSettingHints[] = {
    {"Display", "fps", "Frame rate", "30|60|90|120|144|auto"},
    {"Display", "resolution", "Resolution",
     "default|auto|1920x1080|2560x1080|2560x1440|3440x1440|3840x2160"},
    {"Display", "aspect", "Aspect ratio", "16:9|21:9|32:9|16:10|4:3"},
    {"Display", "fullscreen", "Start fullscreen", ""},
    {"Display", "show_fps", "FPS in the window title", ""},
    {"Display", "console_mode", "Console mode", "docked|handheld"},
    {"Graphics", "native_render", "Render at the set resolution", ""},
    {"Graphics", "draw_distance", "Draw distance", "1.0|1.5|2.0|3.0|4.0|8.0"},
    {"Debug", "diagnostics", "Run reports", ""},
    {"Debug", "rtss_overlay", "Allow RivaTuner overlay", ""},
    {"Menu", "key", "Menu key", "F10|F9|F8|Home|none"},
    {"Menu", "pad_combo", "Menu controller combo",
     "back+start|back+guide|leftstick+rightstick|none"},
#ifdef _WIN32
    {"Mods", "loader", "Mod loader", "Forge/forge.dll|none"},
#else
    {"Mods", "loader", "Mod loader", "Forge/forge.so|none"},
#endif
};

std::vector<GS::OptionInfo> g_options;
std::map<std::string, std::array<char, 160>> g_setting_text; // "section/key" -> field text
std::vector<std::string> g_restart_needed;                   // labels changed this session
std::string g_settings_message;
bool g_settings_message_bad{};

const SettingHint* HintFor(const GS::OptionInfo& o) {
    for (const SettingHint& h : kSettingHints) {
        if (o.section == h.section && o.key == h.key) {
            return &h;
        }
    }
    return nullptr;
}

bool IsBoolOption(const GS::OptionInfo& o) {
    return o.default_value == "true" || o.default_value == "false";
}

bool IsOn(const std::string& v) {
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

std::string CurrentValue(const GS::OptionInfo& o) {
    std::string v = GS::Value(o.section, o.key);
    return v.empty() ? o.default_value : v;
}

void LoadSettingsFields() {
    g_options.clear();
    for (GS::OptionInfo& o : GS::Options()) {
        if (o.section == "Multiplayer") {
            continue; // has its own tab
        }
        CopyTo(g_setting_text[o.section + "/" + o.key], CurrentValue(o));
        g_options.push_back(std::move(o));
    }
}

/// Checks a typed value; "" = fine, else what's wrong.
std::string CheckSetting(const GS::OptionInfo& o, const std::string& v) {
    const auto in_range = [&v](float lo, float hi) {
        char* end = nullptr;
        const float f = std::strtof(v.c_str(), &end);
        return end != v.c_str() && *end == '\0' && f >= lo && f <= hi;
    };
    if (o.section == "Display" && o.key == "fps" && v != "auto" && !in_range(10.0f, 360.0f)) {
        return "Frame rate: a number from 10 to 360, or auto.";
    }
    if (o.section == "Graphics" && o.key == "draw_distance" && !in_range(1.0f, 8.0f)) {
        return "Draw distance: a number from 1.0 to 8.0.";
    }
    return {};
}

void CommitSetting(const GS::OptionInfo& o, const std::string& label, const std::string& value) {
    if (const std::string bad = CheckSetting(o, value); !bad.empty()) {
        g_settings_message = bad;
        g_settings_message_bad = true;
        CopyTo(g_setting_text[o.section + "/" + o.key], CurrentValue(o));
        return;
    }
    if (CurrentValue(o) == value) {
        return;
    }
    GS::SetValue(o.section, o.key, value);
    LOG_INFO(Frontend, "game menu: [{}] {} = {}", o.section, o.key, value);
    bool live = GS::ApplyLive(o.section, o.key, value);
    if (o.section == "Menu") {
        ReadMenuSettings();
        live = true;
    }
    if (live) {
        g_settings_message = fmt::format("{}: {} (applied now)", label, value);
    } else {
        g_settings_message = fmt::format("{}: {} (saved; restart the game to apply)", label, value);
        if (std::find(g_restart_needed.begin(), g_restart_needed.end(), label) ==
            g_restart_needed.end()) {
            g_restart_needed.push_back(label);
        }
    }
    g_settings_message_bad = false;
}

void DrawSettings() {
    ImGui::TextWrapped("The options in game_settings.ini beside the game. Changes are saved right "
                       "away. Frame rate, the FPS display and the menu buttons apply at once; the "
                       "rest at the next start.");
    if (ImGui::Button("Open game_settings.ini")) {
        OpenPath(Common::FS::GetExeDirectory() / "game_settings.ini");
    }
    if (!g_restart_needed.empty()) {
        std::string list;
        for (const auto& name : g_restart_needed) {
            list += (list.empty() ? "" : ", ") + name;
        }
        ImGui::TextColored(kYellow, "Restart the game to apply: %s", list.c_str());
    }
    if (!g_settings_message.empty()) {
        ImGui::TextColored(g_settings_message_bad ? kRed : kGrey, "%s",
                           g_settings_message.c_str());
    }

    std::string section;
    bool table_open = false;
    const float label_width = ImGui::GetFontSize() * 14.0f;
    for (const GS::OptionInfo& o : g_options) {
        if (o.section != section) {
            if (table_open) {
                ImGui::EndTable();
            }
            section = o.section;
            SectionTitle(section.c_str());
            table_open = ImGui::BeginTable(section.c_str(), 2);
            if (table_open) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, label_width);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            }
        }
        if (!table_open) {
            continue;
        }
        const SettingHint* hint = HintFor(o);
        const std::string label = hint != nullptr ? hint->label : o.key;
        const std::string id = o.section + "/" + o.key;
        auto& text = g_setting_text[id];

        ImGui::PushID(id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        HelpMarker(fmt::format("[{}] {} (default {})\n\n{}", o.section, o.key, o.default_value,
                               o.comment)
                       .c_str());
        ImGui::TableNextColumn();
        if (IsBoolOption(o)) {
            bool on = IsOn(text.data());
            if (ImGui::Checkbox("##value", &on)) {
                CopyTo(text, on ? "true" : "false");
                CommitSetting(o, label, text.data());
            }
        } else {
            const bool has_presets = hint != nullptr && hint->presets[0] != '\0';
            const float combo_width = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x -
                                                 (has_presets ? combo_width + 4.0f : 0.0f),
                                             ImGui::GetFontSize() * 16.0f));
            ImGui::InputText("##value", text.data(), text.size());
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                CommitSetting(o, label, text.data());
            }
            if (has_presets) {
                ImGui::SameLine(0.0f, 4.0f);
                if (ImGui::BeginCombo("##presets", nullptr, ImGuiComboFlags_NoPreview)) {
                    const std::string_view all{hint->presets};
                    std::size_t start = 0;
                    while (start <= all.size()) {
                        const std::size_t end = std::min(all.find('|', start), all.size());
                        const std::string preset{all.substr(start, end - start)};
                        if (ImGui::Selectable(preset.c_str(), preset == text.data())) {
                            CopyTo(text, preset);
                            CommitSetting(o, label, preset);
                        }
                        start = end + 1;
                    }
                    ImGui::EndCombo();
                }
            }
        }
        ImGui::PopID();
    }
    if (table_open) {
        ImGui::EndTable();
    }
}

// ── General ─────────────────────────────────────────────────────────────────

void DrawGeneral() {
    std::string game_name;
    if (g_system != nullptr) {
        [[maybe_unused]] auto _ = g_system->GetGameName(game_name);
    }
    ImGui::Text("%s", game_name.empty() ? "Game" : game_name.c_str());
    ImGui::TextColored(kGrey, "Frame rate: %s FPS", g_fps_text.c_str());

    SectionTitle("Folders");
    const auto exe_dir = Common::FS::GetExeDirectory();
    if (ImGui::Button("Saves, config and logs (user)")) {
        OpenPath(exe_dir / "user");
    }
    ImGui::SameLine();
    if (ImGui::Button("Mods")) {
        OpenPath(exe_dir / "mods");
    }
    ImGui::SameLine();
    if (ImGui::Button("Keys")) {
        OpenPath(Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir));
    }
    if (ImGui::Button("Open game_settings.ini")) {
        OpenPath(exe_dir / "game_settings.ini");
    }
    HelpMarker("Frame rate, resolution, console mode, draw distance and more. Changes there "
               "apply the next time the game starts.");

    SectionTitle("Mods found");
    bool any = false;
    std::error_code ec;
    const auto mods_dir = exe_dir / "mods";
    if (std::filesystem::is_directory(mods_dir, ec)) {
        for (const auto& tid : std::filesystem::directory_iterator(mods_dir, ec)) {
            if (!tid.is_directory(ec)) {
                continue;
            }
            for (const auto& mod : std::filesystem::directory_iterator(tid.path(), ec)) {
                ImGui::BulletText("%s", Common::FS::PathToUTF8String(mod.path().filename()).c_str());
                any = true;
            }
        }
    }
    if (!any) {
        ImGui::TextColored(kGrey, "none (drop mods into mods/<title id>/)");
    }

    SectionTitle("Keys");
    if (!g_key_name.empty()) {
        ImGui::BulletText("%s: this menu", g_key_name.c_str());
    }
    if (!g_combo_name.empty()) {
        ImGui::BulletText("Hold %s on a controller for 1 s: this menu", g_combo_name.c_str());
    }
    ImGui::BulletText("F11 or Alt+Enter: fullscreen");
#ifdef _WIN32
    ImGui::BulletText("F12: debug panel");
#endif
    ImGui::BulletText("Esc (in this window): close the menu");
}

void Draw() {
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##game_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (!g_note.empty() && Clock::now() < g_note_until) {
        ImGui::TextColored(kYellow, "%s", g_note.c_str());
    } else {
        ImGui::TextColored(kGrey, "Settings here are saved at once.");
    }
    ImGui::SameLine();
    const float back_width =
        ImGui::CalcTextSize("Back to game").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, ImGui::GetContentRegionAvail().x - back_width));
    if (ImGui::Button("Back to game")) {
        g_close_requested = true;
    }
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Controls")) {
            ImGui::BeginChild("controls");
            DrawControls();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Multiplayer")) {
            ImGui::BeginChild("multiplayer");
            DrawMultiplayer();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            ImGui::BeginChild("settings");
            DrawSettings();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("General")) {
            ImGui::BeginChild("general");
            DrawGeneral();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    // Pad navigation only while the menu has focus (the pad drives the game otherwise), and
    // not while waiting for a button to bind.
    ImGuiIO& io = ImGui::GetIO();
    if (g_focused && !g_capture.active) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }

    ImGui::Render();
    SDL_SetRenderDrawColor(g_renderer, 24, 24, 28, 255);
    SDL_RenderClear(g_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_RenderPresent(g_renderer);
}

void LoadFont(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    static constexpr const char* kFonts[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    std::error_code ec;
    for (const char* path : kFonts) {
        if (std::filesystem::exists(path, ec) && io.Fonts->AddFontFromFileTTF(path, 18.0f)) {
            ImGui::GetStyle().FontScaleDpi = scale;
            return;
        }
    }
    io.Fonts->AddFontDefault();
    ImGui::GetStyle().FontScaleDpi = scale * 1.25f;
}

void Open() {
    if (g_window != nullptr) {
        SDL_RaiseWindow(g_window);
        return;
    }
    ReadMenuSettings();
    float scale = g_game_window != nullptr
                      ? SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(g_game_window))
                      : 1.0f;
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    std::string game_name;
    if (g_system != nullptr) {
        [[maybe_unused]] auto _ = g_system->GetGameName(game_name);
    }
    const std::string title = (game_name.empty() ? std::string("Game") : game_name) + " - Menu";
    g_window = SDL_CreateWindow(title.c_str(), static_cast<int>(820 * scale),
                                static_cast<int>(720 * scale),
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (g_window == nullptr) {
        LOG_ERROR(Frontend, "game menu: no window: {}", SDL_GetError());
        return;
    }
    if (g_game_window != nullptr) {
        // Stays above the game (also a fullscreen one) without being topmost system-wide.
        SDL_SetWindowParent(g_window, g_game_window);
        const SDL_DisplayID display = SDL_GetDisplayForWindow(g_game_window);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    }
    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (g_renderer == nullptr) {
        g_renderer = SDL_CreateRenderer(g_window, SDL_SOFTWARE_RENDERER);
    }
    if (g_renderer == nullptr) {
        LOG_ERROR(Frontend, "game menu: no renderer: {}", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return;
    }
    IMGUI_CHECKVERSION();
    g_imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ScaleAllSizes(scale);
    LoadFont(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);

    LoadMultiplayerFields();
    LoadSettingsFields();
    g_shown_addresses.clear();
    g_show_join_address = false;
    g_mp_message.clear();
    // Keys/buttons held while switching would stay down in the game (their release goes here).
    if (g_input != nullptr) {
        g_input->GetKeyboard()->ReleaseAllKeys();
        g_input->GetMouse()->ReleaseAllButtons();
    }
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    g_focused = true;
    UpdateGate();
    g_last_draw = {};
    LOG_INFO(Frontend, "game menu: opened");
}

void Close() {
    if (g_window == nullptr) {
        return;
    }
    EndCapture();
    ImGui::SetCurrentContext(g_imgui);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(g_imgui);
    g_imgui = nullptr;
    SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
    g_focused = false;
    UpdateGate();
    if (g_game_window != nullptr) {
        SDL_RaiseWindow(g_game_window);
    }
    LOG_INFO(Frontend, "game menu: closed");
}

bool ComboHeld() {
    if (g_combo.empty()) {
        return false;
    }
    for (SDL_Gamepad* pad : g_pads) {
        bool all = true;
        for (const SDL_GamepadButton b : g_combo) {
            all = all && SDL_GetGamepadButton(pad, b);
        }
        if (all) {
            return true;
        }
    }
    return false;
}

} // namespace

void Init(Core::System& system, InputCommon::InputSubsystem* input, SDL_Window* game_window) {
    g_system = &system;
    g_input = input;
    g_game_window = game_window;
    ReadMenuSettings();
    LOG_INFO(Frontend, "game menu: key {}, controller combo {}",
             g_key_name.empty() ? "none" : g_key_name,
             g_combo_name.empty() ? "none" : g_combo_name);
}

void Shutdown() {
    Close();
    for (SDL_Gamepad* pad : g_pads) {
        SDL_CloseGamepad(pad);
    }
    g_pads.clear();
}

bool IsOpen() {
    return g_window != nullptr;
}

void Toggle() {
    if (g_window == nullptr) {
        Open();
    } else if (!g_focused) {
        SDL_RaiseWindow(g_window);
    } else {
        Close();
    }
}

bool HandleEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which)) {
            g_pads.push_back(pad);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (auto it = g_pads.begin(); it != g_pads.end(); ++it) {
            if (SDL_GetGamepadID(*it) == event.gdevice.which) {
                SDL_CloseGamepad(*it);
                g_pads.erase(it);
                break;
            }
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // The menu key works from either window (but types normally while binding a key).
        if (g_key != SDL_SCANCODE_UNKNOWN && event.key.scancode == g_key && !g_capture.polling) {
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                Toggle();
            }
            return true;
        }
        break;
    default:
        break;
    }
    if (g_window == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(g_imgui);
    SDL_Window* const from = SDL_GetWindowFromEvent(&event);
    if (from != g_window) {
        if (event.type >= SDL_EVENT_GAMEPAD_AXIS_MOTION && event.type < SDL_EVENT_FINGER_DOWN) {
            ImGui_ImplSDL3_ProcessEvent(&event); // pads connecting and going away
        }
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        Close();
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        g_focused = true;
        UpdateGate();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_focused = false;
        UpdateGate();
        break;
    case SDL_EVENT_KEY_DOWN:
        if (g_capture.active) {
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                EndCapture();
            } else if (g_capture.polling && !g_capture.analog && !event.key.repeat &&
                       g_input != nullptr) {
                // Through the keyboard driver, so the mapping poll sees the key.
                g_input->GetKeyboard()->PressKey(static_cast<int>(event.key.scancode));
                g_input->GetKeyboard()->ReleaseKey(static_cast<int>(event.key.scancode));
            }
            return true;
        }
        if (event.key.scancode == SDL_SCANCODE_ESCAPE && !ImGui::GetIO().WantTextInput) {
            Close();
            return true;
        }
        break;
    default:
        break;
    }
    ImGui_ImplSDL3_ProcessEvent(&event);
    return true;
}

void Update() {
    const auto now = Clock::now();
    if (ComboHeld()) {
        if (!g_combo_held) {
            g_combo_held = true;
            g_combo_fired = false;
            g_combo_since = now;
        } else if (!g_combo_fired && now - g_combo_since >= kComboHold) {
            g_combo_fired = true;
            Toggle();
        }
    } else {
        g_combo_held = false;
    }
    if (g_window == nullptr) {
        return;
    }
    ImGui::SetCurrentContext(g_imgui);
    PollCapture();
    if (now - g_last_draw < kDrawInterval) {
        return;
    }
    g_last_draw = now;
    if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) != 0) {
        return;
    }
    Draw();
    if (g_close_requested) {
        g_close_requested = false;
        Close();
    }
}

int WaitTimeoutMs() {
    return g_window != nullptr ? 10 : 100;
}

std::string TitleNote() {
    if (!g_note.empty() && Clock::now() < g_note_until) {
        return g_note;
    }
    return Multiplayer::ShortStatus();
}

void Notify(const std::string& text) {
    g_note = text;
    g_note_until = Clock::now() + kNoteTime;
}

void SetFrameRate(double fps) {
    g_fps_text = fmt::format("{:.0f}", fps);
}

} // namespace GameMenu
