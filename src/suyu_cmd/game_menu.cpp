// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "common/fs/path_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/settings_enums.h"
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
#include "suyu_cmd/relaunch.h"

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
char g_online_server[96]{};
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

/// The Ryujinx LDN side: where the RyuLdn lobbies live (Ryujinx LDN or Both).
void DrawRyuLdn() {
    static bool show_server = false;
    const std::string status = Multiplayer::RyuLdnStatusText();
    const bool ok = status.find("in a lobby") != std::string::npos;
    const bool bad = status.find("no server") != std::string::npos ||
                     status.find("not connected (") != std::string::npos;
    ImGui::TextColored(ok ? kGreen : (bad ? kRed : kGrey), "%s", status.c_str());
    if (const std::string server = Multiplayer::RyuLdnServer(); !server.empty()) {
        ImGui::TextColored(kGrey, "Server: %s", show_server ? server.c_str() : "(hidden)");
        ImGui::SameLine();
        if (ImGui::SmallButton(show_server ? "Hide##ryusrv" : "Show##ryusrv")) {
            show_server = !show_server;
        }
    }
    ImGui::TextWrapped(
        "Lobbies are found and joined in the game itself (local wireless play), together with "
        "Ryujinx players on the same server; the Public rooms tab lists them.");

    // Where the lobbies live: Ryujinx's public server, this PC, or a friend's PC.
    using Mode = Multiplayer::RyuLdnServerChoice::Mode;
    static char friend_address[128]{};
    static bool show_friend = false;
    static std::string choice_message;
    static bool choice_bad = false;
    const auto choice = Multiplayer::CurrentRyuLdnServerChoice();
    if (choice.mode == Mode::Friend && friend_address[0] == '\0') {
        CopyTo(friend_address, choice.address);
    }
    const auto choose = [](Multiplayer::RyuLdnServerChoice next, const char* done) {
        std::string error;
        if (Multiplayer::ChooseRyuLdnServer(next, &error)) {
            choice_message = done;
            choice_bad = false;
        } else {
            choice_message = "Couldn't switch: " + error;
            choice_bad = true;
        }
    };

    SectionTitle("Server");
    if (ImGui::RadioButton("Ryujinx public server", choice.mode == Mode::Public) &&
        choice.mode != Mode::Public) {
        choose({Mode::Public, {}}, "Using Ryujinx's public server.");
    }
    HelpMarker("ldn.ryujinx.app: everyone's public lobbies, no setup.");
    if (ImGui::RadioButton("Host for friends (Radmin VPN)", choice.mode == Mode::Host) &&
        choice.mode != Mode::Host) {
        choose({Mode::Host, {}}, "Your server is running; friends join your address.");
    }
    HelpMarker("This game runs the lobby server; friends connect to your address (Radmin VPN or "
               "home network). Works without the public server. Windows may ask to allow the "
               "game on the network: allow it.");
    if (ImGui::RadioButton("Join a friend's server", choice.mode == Mode::Friend) &&
        choice.mode != Mode::Friend) {
        if (friend_address[0] != '\0') {
            choose({Mode::Friend, friend_address}, "Using your friend's server.");
        } else {
            choice_message = "Enter your friend's address below, then press Use.";
            choice_bad = false;
        }
    }

    if (choice.mode == Mode::Host) {
        const auto own = Multiplayer::GetOwnServerStatus();
        if (own.running) {
            ImGui::TextColored(kGreen, "Your server: %zu connected, %zu lobbies (port %u)",
                               own.connections, own.lobbies, own.port);
        } else {
            ImGui::TextColored(kRed, "Your server isn't running%s%s", own.error.empty() ? "" : ": ",
                               own.error.c_str());
        }
        ImGui::TextUnformatted("Give friends one of these addresses:");
        const auto addresses = Multiplayer::LocalAddresses();
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            const auto& a = addresses[i];
            ImGui::PushID(static_cast<int>(i) + 7000);
            if (ImGui::SmallButton("Copy")) {
                ImGui::SetClipboardText(a.ip.c_str());
                Notify("Address copied to the clipboard");
            }
            ImGui::SameLine();
            const bool shown = g_shown_addresses.contains(i + 7000);
            if (ImGui::SmallButton(shown ? a.ip.c_str() : "show address")) {
                if (shown) {
                    g_shown_addresses.erase(i + 7000);
                } else {
                    g_shown_addresses.insert(i + 7000);
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(a.label.c_str());
            ImGui::PopID();
        }
        ImGui::TextWrapped("Friends with this game: Join a friend's server, paste the address. "
                           "Ryujinx friends: Multiplayer mode RyuLDN with your address as the "
                           "LDN server. Then host or search in the game's local play.");
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##friendsrv", "friend's address, e.g. 26.12.34.56", friend_address,
        sizeof(friend_address),
        ImGuiInputTextFlags_EnterReturnsTrue |
            (show_friend ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password));
    ImGui::SameLine();
    if (ImGui::Button(show_friend ? "Hide##fs" : "Show##fs")) {
        show_friend = !show_friend;
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste##fs")) {
        if (char* clip = SDL_GetClipboardText()) {
            CopyTo(friend_address, clip);
            SDL_free(clip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Use##fs") || enter) {
        choose({Mode::Friend, friend_address}, "Using your friend's server.");
    }
    if (!choice_message.empty()) {
        ImGui::TextColored(choice_bad ? kRed : kGrey, "%s", choice_message.c_str());
    }
}

/// Public rooms: this game's lobbies on the RyuLdn servers, refreshed while the tab is open.
void DrawPublicRooms() {
    static char new_server[128]{};
    static std::string server_message;
    static bool show_servers = false;
    constexpr auto kRefreshEvery = std::chrono::seconds(15);

    const Multiplayer::PublicRooms list = Multiplayer::GetPublicRooms();
    if (!list.refreshing &&
        (!list.refreshed || std::chrono::steady_clock::now() - list.last_refresh > kRefreshEvery)) {
        Multiplayer::RefreshPublicRooms();
    }

    if (!Multiplayer::UsingRyuLdn()) {
        ImGui::TextColored(kYellow, "Local play is set to suyu rooms only (ini backend = room).");
    }
    if (ImGui::Button("Refresh")) {
        Multiplayer::RefreshPublicRooms();
    }
    ImGui::SameLine();
    if (list.refreshing) {
        ImGui::TextColored(kGrey, "Looking for lobbies...");
    } else if (list.refreshed) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - list.last_refresh);
        ImGui::TextColored(kGrey, "%zu lobbies%s  -  updated %lld s ago", list.rooms.size(),
                           list.private_passphrase ? " (your passphrase)" : "",
                           static_cast<long long>(age.count()));
    }
    HelpMarker("Lobbies of this game that you (and Ryujinx players) can join: open local play "
               "in the game and search, as on a Switch. Hosting a lobby in the game puts it "
               "here for everyone. With a passphrase set, only lobbies with the same passphrase "
               "show up.");
    for (const auto& error : list.errors) {
        ImGui::TextColored(kRed, "%s", error.c_str());
    }

    const float rows_height = ImGui::GetFontSize() * 12.0f;
    if (ImGui::BeginTable("publicrooms", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH,
                          ImVec2(0.0f, rows_height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Host");
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Server", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 6.0f);
        ImGui::TableHeadersRow();
        for (const auto& room : list.rooms) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(room.host.c_str());
            if (room.names.size() > 1 && ImGui::IsItemHovered()) {
                std::string names;
                for (const auto& n : room.names) {
                    names += (names.empty() ? "" : "\n") + n;
                }
                ImGui::SetTooltip("%s", names.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d / %d", room.players, room.max_players);
            ImGui::TableNextColumn();
            ImGui::TextColored(kGrey, "%s", room.server.c_str());
        }
        ImGui::EndTable();
    }
    if (list.refreshed && list.rooms.empty() && list.errors.empty()) {
        ImGui::TextColored(kGrey, "No open lobbies right now.");
    }

    SectionTitle("Servers");
    ImGui::BulletText("This PC - your lobbies are hosted here while you host");
    ImGui::BulletText("Ryujinx (%s) - public lobbies", Multiplayer::DefaultRyuLdnHost);
    HelpMarker("Hosting opens a room for suyu players and a server for Ryujinx players on this "
               "PC; the game's local-play search also lists Ryujinx's public lobbies and those "
               "of the friend whose room you joined. game_settings.ini ryuldn_public = off "
               "leaves the public server out.");
    const auto extra = Multiplayer::ExtraRyuLdnServers();
    for (std::size_t i = 0; i < extra.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::BulletText("Server %zu: %s", i + 2,
                          show_servers ? extra[i].c_str() : "(hidden)");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            Multiplayer::RemoveExtraRyuLdnServer(extra[i]);
            server_message = "Removed.";
        }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##newserver", "another server, host or host:port", new_server, sizeof(new_server),
        ImGuiInputTextFlags_EnterReturnsTrue |
            (show_servers ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password));
    ImGui::SameLine();
    if (ImGui::Button("Add") || enter) {
        std::string error;
        if (Multiplayer::AddExtraRyuLdnServer(new_server, &error)) {
            new_server[0] = '\0';
            server_message = error.empty()
                                 ? "Added: its lobbies show up here and in the game's search."
                                 : "Added; " + error;
            Multiplayer::RefreshPublicRooms();
        } else {
            server_message = "Couldn't add: " + error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(show_servers ? "Hide##srv" : "Show##srv")) {
        show_servers = !show_servers;
    }
    HelpMarker("More RyuLdn servers (for example one your group runs): their lobbies are listed "
               "and joinable too, next to the Ryujinx server's.");
    if (!server_message.empty()) {
        ImGui::TextColored(kGrey, "%s", server_message.c_str());
    }
}

void DrawRoom();

// "Local Multiplayer" tab: suyu rooms, Ryujinx LDN and the public room list.
void DrawLocalMultiplayer() {
    // The switch: which network carries local play (applies at once). Both = automatic.
    static std::string mode_message;
    using LdnMode = Multiplayer::LdnMode;
    const LdnMode mode = Multiplayer::CurrentLdnMode();
    ImGui::TextUnformatted("Play over:");
    const auto pick = [&](const char* label, LdnMode value) {
        ImGui::SameLine();
        if (ImGui::RadioButton(label, mode == value) && mode != value) {
            std::string error;
            mode_message = Multiplayer::SetLdnMode(value, &error) ? std::string{} : error;
        }
    };
    pick("Suyu room", LdnMode::Room);
    pick("Ryujinx LDN", LdnMode::RyuLdn);
    pick("Both (automatic)", LdnMode::Auto);
    HelpMarker("Both (the default): hosting opens a room and a server by itself, joining picks "
               "whatever the lobby needs. Suyu room / Ryujinx LDN force one side only. Applies at "
               "once (leave a lobby first).");
    if (!mode_message.empty()) {
        ImGui::TextColored(kRed, "Couldn't switch: %s", mode_message.c_str());
    }

    const std::string bridge = Multiplayer::BridgeStatusText();
    if (!bridge.empty()) {
        ImGui::TextColored(kGrey, "%s", bridge.c_str());
    }

    if (ImGui::BeginTabBar("mptabs")) {
        if (mode != LdnMode::RyuLdn && ImGui::BeginTabItem("Join Room")) {
            DrawRoom();
            ImGui::EndTabItem();
        }
        if (mode != LdnMode::Room && ImGui::BeginTabItem("Ryujinx LDN")) {
            DrawRyuLdn();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Public rooms")) {
            DrawPublicRooms();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// "Online Multiplayer" tab: your online (NEX) server, separate from the local networks.
void DrawOnlineMultiplayer() {
    static bool online_loaded = false;
    if (!online_loaded) {
        online_loaded = true;
        const std::string v = GS::Value("Multiplayer", "online_server_host");
        std::snprintf(g_online_server, sizeof(g_online_server), "%s", v.c_str());
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputTextWithHint("Online Server IP", "IP or host (empty = off)", g_online_server,
                             sizeof(g_online_server));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::string v = g_online_server;
        v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char c) { return std::isspace(c); }),
                v.end());
        if (const auto colon = v.find(':'); colon != std::string::npos) {
            v.resize(colon); // ports are fixed by the game
        }
        std::snprintf(g_online_server, sizeof(g_online_server), "%s", v.c_str());
        GS::SetValue("Multiplayer", "online_server_host", v);
    }
    HelpMarker("Your online (NEX) server: an IPv4 address or host name. host:port keeps only the "
               "host (ports are fixed by the game). Empty = off. Applies the next time the game "
               "connects; set it before booting. Separate from Local Multiplayer.");
    ImGui::TextColored(kGrey, "Online server: %s", g_online_server[0] ? g_online_server : "(off)");
}

void DrawMultiplayer() {
    // One name for every network: online server, suyu rooms and Ryujinx lobbies.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputText("Name", g_nick, sizeof(g_nick));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Multiplayer::Save(FieldsToConfig());
    }
    HelpMarker("Shown to other players online, in suyu rooms and in Ryujinx lobbies (Public "
               "rooms). 4-20 characters: letters, digits, space, . _ -. Applies to the next room "
               "or lobby you host or join.");

    if (ImGui::BeginTabBar("mpmain")) {
        if (ImGui::BeginTabItem("Local Multiplayer")) {
            DrawLocalMultiplayer();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Online Multiplayer")) {
            DrawOnlineMultiplayer();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void DrawRoom() {
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
    for (std::size_t i = 0; i < st.members.size(); ++i) {
        ImGui::BulletText("%s", st.members[i].c_str());
        if (st.phase == Phase::Hosting && i < st.names.size() && st.names[i] != st.nickname) {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Kick")) {
                Multiplayer::Kick(st.names[i]);
                MpMessage(fmt::format("Kicked {}.", st.names[i]), false);
            }
            ImGui::PopID();
        }
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
               "if the room has one), or type their Radmin VPN address. One address covers both "
               "worlds: you join their room and see the lobbies on their server too. Then open "
               "the game's local play and join their lobby.");
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

    SectionTitle("Hosting");
    ImGui::TextWrapped(
        "Nothing to press: when you host in the game's local play, this PC opens the room and the "
        "server by itself, and suyu and Ryujinx friends join the same lobby. These are the "
        "room's options:");
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
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Multiplayer::Save(FieldsToConfig());
    }
    const auto own = Multiplayer::GetOwnServerStatus();
    if (own.running) {
        ImGui::TextColored(kGreen, "Hosting: %zu connected to your server, %zu lobbies (port %u)",
                           own.connections, own.lobbies, own.port);
    } else if (!own.error.empty()) {
        ImGui::TextColored(kRed, "Your server couldn't start: %s", own.error.c_str());
    }
    ImGui::TextColored(kGrey, "Friends join with one of your addresses (Copy puts an invite on "
                              "the clipboard; click an address to show it). Ryujinx friends set "
                              "this address as their LDN server:");
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
    bool closed;         // true: only these values (a labelled drop-down instead of a text field)
};

constexpr SettingHint kSettingHints[] = {
    {"Display", "fps", "Frame rate", "30|60|90|120|144|auto"},
    {"Display", "resolution", "Resolution",
     "default|auto|1920x1080|2560x1080|2560x1440|3440x1440|3840x2160"},
    {"Display", "aspect", "Aspect ratio", "16:9|21:9|32:9|16:10|4:3"},
    {"Display", "fullscreen", "Start fullscreen", ""},
    {"Display", "show_fps", "FPS in the window title", ""},
    {"Display", "console_mode", "Console mode", "docked|handheld"},
    {"Graphics", "renderer", "Renderer", "Emulated|Direct", true},
    {"Graphics", "native_render", "Render at the set resolution", ""},
    {"Upscaler", "nvn_fsr_quality", "Quality", "Native AA|Quality|Balanced|Performance|Ultra Performance", true},
    {"Upscaler", "nvn_fsr_sharpness", "Sharpness", ""},
    {"Upscaler", "render_scale_override", "Override render scale", ""},
    {"Upscaler", "nvn_fsr_split_scale", "Render below the output (split scale)", ""},
    {"Diagnostics", "diagnostics", "Run reports", ""},
    {"Diagnostics", "gpu_diagnostics", "Run reports (replacement renderer)", ""},
    {"Diagnostics", "log_redact_paths", "Hide personal data in logs", ""},
    {"Diagnostics", "nvn_call_sampling", "NVN call sampling", ""},
    {"Diagnostics", "nvn_perf_gpu", "GPU timestamps", "1|0"},
    {"Diagnostics", "nvn_fsr_texel_stats", "FSR texel statistics", "1|0"},
    {"Diagnostics", "nvn_bench", "Benchmark mode", "0|1"},
    {"Diagnostics", "nvn_bench_scene", "Benchmark scene", ""},
    {"Diagnostics", "nvn_bench_warmup", "Benchmark warm-up (s)", ""},
    {"Diagnostics", "nvn_bench_measure", "Benchmark measure (s)", ""},
    {"Diagnostics", "nvn_upscaler_probe", "Upscaler probes", ""},
    {"Diagnostics", "vulkan_validation", "Vulkan validation layer", ""},
    {"Diagnostics", "probes", "Wiggle-hitch probes", ""},
    {"Display", "rtss_overlay", "Allow RivaTuner overlay", ""},
    {"Menu", "key", "Menu key", "F10|F9|F8|Home|none"},
    {"Menu", "show_advanced", "Advanced mode", ""},
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

/// game_settings.ini [Menu] show_advanced (Advanced mode), re-read about once a second and right after the menu changes it.
Clock::time_point g_adv_next{};
bool ShowAdvancedTab() {
    static bool shown{};
    if (Clock::now() >= g_adv_next) {
        const std::string v = GS::Value("Menu", "show_advanced");
        shown = v == "true" || v == "1" || v == "yes" || v == "on";
        g_adv_next = Clock::now() + std::chrono::seconds(1);
    }
    return shown;
}

/// The Upscaler section's state (ini values + the Render scale the Quality mode would pick), refreshed when a setting was
/// changed through the menu and about once a second (the files are read, so not every frame).
struct UpState {
    GS::UpscalerChoice choice{};
    GS::FsrQuality quality{};
    bool direct{};         // [Graphics] renderer = Direct
    bool by_mode{};        // the Quality mode (not the saved Render scale) decides the render scale
    int effective_step{3}; // Settings::ResolutionSetup index in use
    std::string why;
    bool verbose{};        // suyu log_filter is at Debug
};
const UpState& Up() {
    static UpState up;
    static std::uint64_t gen = ~std::uint64_t{0};
    static Clock::time_point next{};
    const std::uint64_t now_gen = GS::SettingsGeneration();
    if (gen != now_gen || Clock::now() >= next) {
        gen = now_gen;
        next = Clock::now() + std::chrono::seconds(1);
        up.choice = GS::UpscalerChoiceNow();
        up.quality = GS::FsrQualityNow();
        up.direct = GS::DirectRendererNow();
        up.effective_step = GS::EffectiveResolutionStep(static_cast<int>(Settings::values.resolution_setup.GetValue()),
                                                        &up.by_mode, &up.why);
        const std::string f = Settings::values.log_filter.GetValue();
        up.verbose = f.rfind("*:Debug", 0) == 0 || f.rfind("*:Trace", 0) == 0;
    }
    return up;
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
    if (o.section == "Upscaler" && o.key == "nvn_fsr_sharpness" && !in_range(0.0f, 1.0f)) {
        return "Sharpness: a number from 0 to 1.";
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
        g_adv_next = {}; // Advanced mode: the tab and the rows follow at once
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

void DrawAdvancedRow(Settings::BasicSetting& setting);
void DrawAdvancedValue(Settings::BasicSetting& setting);

bool g_restart_confirm{}; // "Restart now" was clicked once: ask before closing
bool g_restarting{};      // the new process is started and this one is closing
std::string g_restart_error; // why the last restart could not start the new process

/// The yellow "Restart the game to apply: ..." line (settings read only at start that changed this session) with
/// a "Restart now" button beside it: a second click confirms, then the game starts again with the same command line
/// and this instance closes normally (the new one waits for it). If the start fails, the error is shown and
/// nothing closes.
void DrawRestartLine() {
    if (g_restart_needed.empty()) {
        return;
    }
    std::string list;
    for (const auto& name : g_restart_needed) {
        list += (list.empty() ? "" : ", ") + name;
    }
    ImGui::TextColored(kYellow, "Restart the game to apply: %s", list.c_str());
    if (!g_restart_error.empty()) {
        ImGui::TextColored(kRed, "%s", g_restart_error.c_str());
    }
    if (g_restarting) {
        ImGui::SameLine();
        ImGui::TextColored(kYellow, "(restarting...)");
        return;
    }
    ImGui::SameLine();
    if (!g_restart_confirm) {
        if (ImGui::SmallButton("Restart now")) {
            g_restart_confirm = true;
            g_restart_error.clear();
        }
        return;
    }
    ImGui::TextColored(kYellow, "Close the game and start it again?");
    ImGui::SameLine();
    if (ImGui::SmallButton("Yes, restart")) {
        std::string error;
        unsigned long pid = 0;
        if (Relaunch::StartNewInstance(&error, &pid)) {
            LOG_INFO(Frontend, "game menu: restart: new instance started (pid {}), closing this one", pid);
            g_restarting = true;
            g_settings_message = "Restarting: the game closes and starts again.";
            g_settings_message_bad = false;
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT; // the normal close path (window closed): saves and shuts down cleanly
            SDL_PushEvent(&quit);
        } else {
            LOG_ERROR(Frontend, "game menu: restart failed: {}", error);
            g_restart_error = "Restart failed: " + error + ". The game keeps running.";
            g_restart_confirm = false;
            return;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel")) {
        g_restart_confirm = false;
    }
}

void MarkRestart(const std::string& label) {
    if (std::find(g_restart_needed.begin(), g_restart_needed.end(), label) == g_restart_needed.end()) {
        g_restart_needed.push_back(label);
    }
}

/// Starts a two-column settings row: label (+ help marker) and the value cell.
void RowStart(const std::string& label, const std::string& help) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());
    if (!help.empty()) {
        HelpMarker(help.c_str());
    }
    ImGui::TableNextColumn();
}

/// "Upscaler": Off | FSR 3.1 | FSR 4. Stored as [Upscaler] nvn_fsr (bool) + nvn_fsr_backend (3 | 4).
void DrawUpscalerChoiceRow(const UpState& up, bool grey) {
    constexpr const char* kNames[] = {"Off", "FSR 3.1", "FSR 4"};
    ImGui::PushID("upscaler_choice");
    ImGui::BeginDisabled(grey);
    RowStart("Upscaler",
             "[Upscaler] nvn_fsr + nvn_fsr_backend\n\nOff, FSR 3.1 (Vulkan) or FSR 4 (through the graphics driver's DX12 library, "
             "falls back to 3.1). Needs the Direct renderer. Applies after a restart.");
    const int cur = static_cast<int>(up.choice);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("##value", kNames[cur])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kNames[i], i == cur) && i != cur) {
                GS::SetUpscalerChoice(static_cast<GS::UpscalerChoice>(i));
                LOG_INFO(Frontend, "game menu: Upscaler = {}", kNames[i]);
                g_settings_message = fmt::format("Upscaler: {} (saved; restart the game to apply)", kNames[i]);
                g_settings_message_bad = false;
                MarkRestart("Upscaler");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

/// "Render scale": the saved Graphics > Resolution scale (suyu's resolution_setup, kept as the backing store). Greyed and
/// showing the effective value while an upscaler's Quality mode decides it.
void DrawRenderScaleRow(const UpState& up) {
    ImGui::PushID("render_scale");
    RowStart("Render scale",
             "How much sharper than the game's own resolution the picture is drawn (1X = native). Not the window size: "
             "that is Display > Resolution. With the replacement renderer it applies after a restart.\n\nWhile an Upscaler "
             "is on (and Advanced mode's Override render scale is off) the Quality mode decides this value.");
    if (up.by_mode) {
        ImGui::BeginDisabled(true);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::BeginCombo("##value", fmt::format("{}", GS::ResolutionStepName(up.effective_step)).c_str())) {
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TextColored(kGrey, "Set by the Upscaler's Quality mode (%s). Your saved value is kept.", up.why.c_str());
    } else {
        DrawAdvancedValue(Settings::values.resolution_setup);
    }
    ImGui::PopID();
}

/// "Verbose log": suyu's log_filter *:Debug (on) or the default *:Info (off), applied to the running logger at once.
void DrawVerboseLogRow(const UpState& up) {
    ImGui::PushID("verbose_log");
    RowStart("Verbose log",
             "suyu's log_filter: on = *:Debug (much bigger suyu log), off = the default *:Info. Applies at once and is "
             "saved with suyu's config. A custom log_filter is replaced when you switch this.");
    bool on = up.verbose;
    if (ImGui::Checkbox("##value", &on)) {
        const std::string filter_text = on ? "*:Debug" : "*:Info";
        Settings::values.log_filter.SetValue(filter_text);
        Common::Log::Filter filter;
        filter.ParseFilterString(filter_text);
        Common::Log::SetGlobalFilter(filter);
        Controls::SaveConfig();
        LOG_INFO(Frontend, "game menu: verbose log {} (log_filter = {})", on ? "on" : "off", filter_text);
        g_settings_message = fmt::format("Verbose log: {} (applied now)", on ? "on" : "off");
        g_settings_message_bad = false;
    }
    ImGui::PopID();
}

void DrawSettings() {
    ImGui::TextWrapped("The options in game_settings.ini beside the game. Changes are saved right "
                       "away. Frame rate, the FPS display and the menu buttons apply at once; the "
                       "rest at the next start.");
    if (ImGui::Button("Open game_settings.ini")) {
        OpenPath(Common::FS::GetExeDirectory() / "game_settings.ini");
    }
    DrawRestartLine();
    if (!g_settings_message.empty()) {
        ImGui::TextColored(g_settings_message_bad ? kRed : kGrey, "%s",
                           g_settings_message.c_str());
    }

    const bool advanced_mode = ShowAdvancedTab();
    const UpState& up = Up();
    std::string section;
    bool table_open = false;
    const float label_width = ImGui::GetFontSize() * 14.0f;
    // rows that close a section: the Render scale row ends Graphics, Verbose log ends Diagnostics
    const auto end_section = [&](const std::string& finished) {
        if (!table_open) {
            return;
        }
        if (finished == "Graphics") {
            DrawRenderScaleRow(up);
        } else if (finished == "Diagnostics") {
            DrawVerboseLogRow(up);
        }
    };
    for (const GS::OptionInfo& o : g_options) {
        if (o.advanced && !advanced_mode) {
            continue; // Advanced mode rows (the Diagnostics group, Override render scale)
        }
        if (o.section != section) {
            end_section(section);
            if (table_open) {
                ImGui::EndTable();
            }
            section = o.section;
            SectionTitle(section.c_str());
            if (section == "Upscaler") {
                if (!up.direct) {
                    ImGui::TextColored(kYellow, "FSR needs the Direct renderer: set Renderer (Graphics) to Direct and restart.");
                } else if (up.by_mode) {
                    ImGui::TextColored(kGrey, "The Quality mode decides the Render scale (Graphics).");
                }
            }
            table_open = ImGui::BeginTable(section.c_str(), 2);
            if (table_open) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, label_width);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                if (section == "Upscaler") {
                    DrawUpscalerChoiceRow(up, !up.direct);
                }
            }
        }
        if (!table_open) {
            continue;
        }
        const SettingHint* hint = HintFor(o);
        const std::string label = hint != nullptr ? hint->label : o.key;
        const std::string id = o.section + "/" + o.key;
        auto& text = g_setting_text[id];
        // Upscaler rows are greyed with the Direct renderer missing; Quality and Sharpness also with the Upscaler off
        const bool grey = o.section == "Upscaler" &&
                          (!up.direct || ((o.key == "nvn_fsr_quality" || o.key == "nvn_fsr_sharpness") &&
                                          up.choice == GS::UpscalerChoice::Off));

        ImGui::PushID(id.c_str());
        ImGui::BeginDisabled(grey);
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
        } else if (hint != nullptr && hint->closed) {
            // a labelled drop-down with only the listed values
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::BeginCombo("##value", text.data())) {
                const std::string_view all{hint->presets};
                std::size_t start = 0;
                while (start <= all.size()) {
                    const std::size_t end = std::min(all.find('|', start), all.size());
                    const std::string choice{all.substr(start, end - start)};
                    std::string a = choice, b = text.data();
                    for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ImGui::Selectable(choice.c_str(), a == b)) {
                        CopyTo(text, choice);
                        CommitSetting(o, label, choice);
                    }
                    start = end + 1;
                }
                ImGui::EndCombo();
            }
        } else if (o.section == "Upscaler" && o.key == "nvn_fsr_sharpness") {
            float f = std::strtof(text.data(), nullptr);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::SliderFloat("##value", &f, 0.0f, 1.0f, "%.2f")) {
                CopyTo(text, fmt::format("{:.2f}", f));
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
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
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    end_section(section);
    if (table_open) {
        ImGui::EndTable();
    }
}

// ── Advanced: every suyu setting ───────────────────────────────────────────

/// "Res3_4X" -> "3/4X (0.75X)", "Res2X" -> "2X".
std::string PrettyResolution(const std::string& name) {
    std::string body = name.substr(3);
    const auto us = body.find('_');
    if (us == std::string::npos) {
        return body;
    }
    const int num = std::atoi(body.c_str());
    const int den = std::atoi(body.c_str() + us + 1);
    const std::string frac = fmt::format("{}/{}X", num, den);
    return den == 0 ? frac : fmt::format("{} ({:g}X)", frac, static_cast<double>(num) / den);
}

/// The names an enum setting accepts (empty when the setting isn't one of the known enums).
template <typename... Es>
std::vector<std::pair<std::string, u32>> EnumNamesFor(u32 index) {
    std::vector<std::pair<std::string, u32>> out;
    (
        [&] {
            if (out.empty() && Settings::EnumMetadata<Es>::Index() == index) {
                for (const auto& entry : Settings::EnumMetadata<Es>::Canonicalizations()) {
                    out.emplace_back(std::string{entry.first}, static_cast<u32>(entry.second));
                }
            }
        }(),
        ...);
    return out;
}

std::vector<std::pair<std::string, u32>> EnumNames(const Settings::BasicSetting& setting) {
    using namespace Settings;
    return EnumNamesFor<AudioEngine, AudioMode, Language, Region, TimeZone, AnisotropyMode,
                        AstcDecodeMode, AstcRecompression, FramePacingMode, VSyncMode,
                        VramUsageMode, RendererBackend, GpuAccuracy, DmaAccuracy, GpuFenceBehavior,
                        CpuBackend, CpuAccuracy, CpuClock, MemoryLayout, ConfirmStop,
                        FullscreenMode, NvdecEmulation, ResolutionSetup, ScalingFilter,
                        AntiAliasing, AspectRatio, ConsoleMode, AppletMode, SpirvOptimizeMode,
                        GpuOverclock, GpuUnswizzleSize, GpuUnswizzle, GpuUnswizzleChunk,
                        TemperatureUnits, ExtendedDynamicState, GpuLogLevel, GameListMode,
                        SpeedMode, DarkModeState>(setting.EnumIndex());
}

const char* CategoryTitle(Settings::Category c) {
    using Settings::Category;
    switch (c) {
    case Category::Audio:
    case Category::SystemAudio:
        return "Audio";
    case Category::Core:
        return "Core";
    case Category::Cpu:
    case Category::CpuDebug:
    case Category::CpuUnsafe:
        return "CPU";
    case Category::Overlay:
    case Category::Renderer:
    case Category::RendererAdvanced:
    case Category::RendererHacks:
    case Category::RendererExtensions:
    case Category::RendererDebug:
    case Category::GpuDriver:
        return "Graphics";
    case Category::System:
    case Category::Services:
    case Category::LibraryApplet:
        return "System";
    case Category::DataStorage:
        return "Storage";
    case Category::Debugging:
    case Category::DebuggingGraphics:
        return "Debug";
    case Category::Linux:
    case Category::Miscellaneous:
    case Category::AddOns:
        return "Miscellaneous";
    case Category::Network:
        return "Network";
    default:
        return nullptr; // controls, shortcuts, paths, UI...: not shown here
    }
}

std::map<std::string, std::array<char, 160>> g_adv_text; // setting label -> field text
std::string g_adv_message;
bool g_adv_message_bad{};
char g_adv_filter[64]{};

void RefreshAdvText(const Settings::BasicSetting& setting) {
    CopyTo(g_adv_text[setting.GetLabel()], setting.ToString());
}

void CommitAdvanced(Settings::BasicSetting& setting, const std::string& value) {
    const std::string before = setting.ToString();
    setting.LoadString(value);
    const std::string after = setting.ToString();
    CopyTo(g_adv_text[setting.GetLabel()], after);
    if (after == before) {
        if (value != before) {
            g_adv_message = fmt::format("{}: '{}' wasn't accepted (kept {})", setting.GetLabel(),
                                        value, after);
            g_adv_message_bad = true;
        }
        return;
    }
    LOG_INFO(Frontend, "game menu: advanced {} = {}", setting.GetLabel(), after);
    Controls::SaveConfig();
    if (g_system != nullptr && setting.RuntimeModifiable()) {
        g_system->ApplySettings();
        g_adv_message = fmt::format("{}: {} (applied now)", setting.GetLabel(), after);
    } else {
        g_adv_message =
            fmt::format("{}: {} (saved; restart the game to apply)", setting.GetLabel(), after);
        // read only at start: listed for the Settings tab's "Restart now" line too
        if (std::find(g_restart_needed.begin(), g_restart_needed.end(), setting.GetLabel()) ==
            g_restart_needed.end()) {
            g_restart_needed.push_back(setting.GetLabel());
        }
    }
    g_adv_message_bad = false;
}

/// The editor for one setting: a checkbox, a drop-down for enums, or a text field.
void DrawAdvancedValue(Settings::BasicSetting& setting) {
    auto& text = g_adv_text[setting.GetLabel()];
    if (text[0] == '\0') {
        RefreshAdvText(setting);
    }
    if (setting.TypeId() == "bool") {
        bool on = setting.ToString() == "true";
        if (ImGui::Checkbox("##value", &on)) {
            CommitAdvanced(setting, on ? "true" : "false");
        }
        return;
    }
    if (setting.IsEnum()) {
        // ToString/LoadString speak the enum's number; the drop-down shows the names.
        const auto names = EnumNames(setting);
        if (!names.empty()) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            const std::string current = setting.ToString();
            std::string shown = setting.Canonicalize();
            if (shown.rfind("Res", 0) == 0 && shown.size() > 3 && std::isdigit(shown[3])) {
                shown = PrettyResolution(shown);
            }
            if (ImGui::BeginCombo("##value", shown.c_str())) {
                for (const auto& [name, value] : names) {
                    const std::string label =
                        name.rfind("Res", 0) == 0 && name.size() > 3 && std::isdigit(name[3])
                            ? PrettyResolution(name)
                            : name;
                    if (ImGui::Selectable(label.c_str(), std::to_string(value) == current)) {
                        CommitAdvanced(setting, std::to_string(value));
                    }
                }
                ImGui::EndCombo();
            }
            return;
        }
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    ImGui::InputText("##value", text.data(), text.size());
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        CommitAdvanced(setting, text.data());
    }
}

void DrawAdvancedRow(Settings::BasicSetting& setting) {
    ImGui::PushID(setting.GetLabel().c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(setting.GetLabel().c_str());
    std::string help = fmt::format("{} (default {})", setting.GetLabel(), setting.DefaultToString());
    if (setting.Ranged()) {
        help += fmt::format("\nrange {} to {}", setting.MinVal(), setting.MaxVal());
    }
    if (!setting.RuntimeModifiable()) {
        help += "\nApplies at the next start of the game.";
    }
    HelpMarker(help.c_str());
    ImGui::TableNextColumn();
    DrawAdvancedValue(setting);
    ImGui::SameLine();
    if (setting.ToString() != setting.DefaultToString() && ImGui::SmallButton("Reset")) {
        CommitAdvanced(setting, setting.DefaultToString());
    }
    ImGui::PopID();
}

void DrawAdvanced() {
    ImGui::TextWrapped("Every setting of suyu's own configuration, grouped like its config tab. "
                       "Changes are saved with the game's config when it closes; settings that "
                       "can change while a game runs apply at once, the rest at the next start. "
                       "Controls and Multiplayer have their own tabs.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    ImGui::InputText("Search", g_adv_filter, sizeof(g_adv_filter));
    if (!g_adv_message.empty()) {
        ImGui::TextColored(g_adv_message_bad ? kRed : kGrey, "%s", g_adv_message.c_str());
    }
    DrawRestartLine();
    std::string needle = g_adv_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Group the categories under their headings, in a fixed order.
    constexpr const char* kGroups[] = {"Graphics", "CPU",     "Audio",   "System",
                                       "Storage",  "Network", "Debug",   "Miscellaneous"};
    for (const char* group : kGroups) {
        std::vector<Settings::BasicSetting*> rows;
        for (const auto& [category, list] : Settings::values.linkage.by_category) {
            const char* title = CategoryTitle(category);
            if (title == nullptr || std::string_view{title} != group) {
                continue;
            }
            for (Settings::BasicSetting* setting : list) {
                if (!setting->Save()) {
                    continue;
                }
                std::string label = setting->GetLabel();
                std::transform(label.begin(), label.end(), label.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (needle.empty() || label.find(needle) != std::string::npos) {
                    rows.push_back(setting);
                }
            }
        }
        if (rows.empty()) {
            continue;
        }
        const bool open = !needle.empty() || ImGui::CollapsingHeader(group);
        if (!open) {
            continue;
        }
        if (!needle.empty()) {
            SectionTitle(group);
        }
        if (ImGui::BeginTable(group, 2)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 18.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            for (Settings::BasicSetting* setting : rows) {
                DrawAdvancedRow(*setting);
            }
            ImGui::EndTable();
        }
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
        if (ShowAdvancedTab() && ImGui::BeginTabItem("Advanced")) {
            ImGui::BeginChild("advanced");
            DrawAdvanced();
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
    // RivaTuner's injected hooks (RTSSHooks64.dll, loaded even with its Vulkan layer kept out) crash
    // when this window presents through D3D11 on the main thread while the game presents through
    // Vulkan on the GPU thread (null read in d3d11.dll under RTSSHooks64, called from the Vulkan
    // driver's present). With RTSS in the process the menu draws in software and blits through GDI,
    // so it has no D3D/GL/Vulkan swapchain for RTSS to hook.
    bool software = false;
#ifdef _WIN32
    software = GetModuleHandleW(L"RTSSHooks64.dll") != nullptr;
#endif
    if (software) {
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0"); // window surface via GDI, not a D3D11 texture
        g_renderer = SDL_CreateRenderer(g_window, SDL_SOFTWARE_RENDERER);
        LOG_INFO(Frontend, "game menu: RivaTuner hooks in the process, software renderer ({})",
                 g_renderer != nullptr ? "ok" : SDL_GetError());
    }
    if (g_renderer == nullptr) {
        g_renderer = SDL_CreateRenderer(g_window, nullptr);
    }
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
