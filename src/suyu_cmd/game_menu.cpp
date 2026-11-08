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
    {Settings::NativeButton::Plus, "加号 (+)"},
    {Settings::NativeButton::Minus, "减号 (-)"},
    {Settings::NativeButton::DUp, "十字键上"},
    {Settings::NativeButton::DDown, "十字键下"},
    {Settings::NativeButton::DLeft, "十字键左"},
    {Settings::NativeButton::DRight, "十字键右"},
    {Settings::NativeButton::LStick, "左摇杆按下"},
    {Settings::NativeButton::RStick, "右摇杆按下"},
    {Settings::NativeButton::Home, "主页"},
    {Settings::NativeButton::Screenshot, "截图"},
};

void UpdateGate() {
    HidController::SetInputBlocked(HidController::BlockerMenu, g_window != nullptr && g_focused);
}

void ReadMenuSettings() {
    std::string key = GS::Value("菜单", "key");
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
            LOG_WARNING(Frontend, "游戏菜单：[Menu] 键中有未知按键名“{}”", key);
        }
    }

    std::string combo = GS::Value("菜单", "pad_combo");
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
                LOG_WARNING(Frontend, "游戏菜单：[Menu] pad_combo 中有未知按钮“{}”", name);
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
        LOG_WARNING(Frontend, "游戏菜单：无法打开 {}：{}", url, SDL_GetError());
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
        Notify("未按下任何按键 - 保持不变");
    }
}

void DrawControls() {
    ImGui::Text("玩家 1 使用：");
    ImGui::SameLine();
    ImGui::TextColored(kGreen, "%s", Controls::PlayerOneText().c_str());
    ImGui::TextColored(kGrey, "设置：%s", Controls::SetByText().c_str());
    HelpMarker("只要您还没有自行选择"
               "控制方式。您在此处的任何更改都算作您的选择，并且"
               "绝不会被替换；“恢复自动”可撤销该操作。");

    SectionTitle("控制器");
    const auto devices = Controls::Devices();
    if (devices.empty()) {
        ImGui::TextColored(kYellow, "未连接控制器。");
        ImGui::TextWrapped("插入一个控制器：它会自动被识别，除非您已自行选择"
                           "控制方式（此时请在此处选择）。");
    } else {
        g_device_pick = std::clamp(g_device_pick, 0, static_cast<int>(devices.size()) - 1);
        const std::string current =
            devices[static_cast<std::size_t>(g_device_pick)].Get("display", "控制器");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        if (ImGui::BeginCombo("##device", current.c_str())) {
            for (std::size_t i = 0; i < devices.size(); ++i) {
                const std::string name = devices[i].Get("display", "控制器");
                const bool selected = static_cast<int>(i) == g_device_pick;
                if (ImGui::Selectable(fmt::format("{}##{}", name, i).c_str(), selected)) {
                    g_device_pick = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("使用此控制器")) {
            Controls::UseDevice(devices[static_cast<std::size_t>(g_device_pick)], "player");
            Notify("玩家 1：" + current);
        }
    }
    if (ImGui::Button("使用键盘（默认按键）")) {
        Controls::UseDefaultKeyboard("player");
        Notify("玩家 1：键盘");
    }
    ImGui::SameLine();
    if (ImGui::Button("恢复自动")) {
        Controls::ResetToAutomatic();
        Notify("控制：自动（" + Controls::PlayerOneText() + ")");
    }
    HelpMarker("默认按键，现在会使用第一个连接的控制器，并且每当"
               "插入一个控制器时都会使用它。");

    SectionTitle("按键");
    if (ImGui::Button("交换 A 和 B")) {
        Controls::SwapButtons(Settings::NativeButton::A, Settings::NativeButton::B);
        Notify("已交换 A 和 B");
    }
    ImGui::SameLine();
    if (ImGui::Button("交换 X 和 Y")) {
        Controls::SwapButtons(Settings::NativeButton::X, Settings::NativeButton::Y);
        Notify("已交换 X 和 Y");
    }
    HelpMarker("游戏采用任天堂布局：A 是右侧面键。如果您希望"
               "下方按键用于确认（如 Xbox/PlayStation 手柄），请交换。");

    const float table_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("binds", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH,
                          ImVec2(0.0f, table_height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("切换输入", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 9.0f);
        ImGui::TableSetupColumn("绑定到");
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
            if (ImGui::SmallButton("更改")) {
                StartCapture(analog, index, label);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("清除")) {
                Controls::SetBinding(analog, index, {});
            }
            ImGui::PopID();
        };
        row(true, Settings::NativeAnalog::LStick, "左摇杆");
        row(true, Settings::NativeAnalog::RStick, "右摇杆");
        for (const auto& b : kButtonRows) {
            row(false, static_cast<std::size_t>(b.index), b.label);
        }
        ImGui::EndTable();
    }

    if (g_capture.active) {
        ImGui::OpenPopup("请按下一个输入");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("请按下一个输入", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (!g_capture.active) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("%s", g_capture.label.c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted(g_capture.analog
                                       ? "将想要用于此操作的摇杆推到底一次。"
                                       : "按下您想用于此操作的按钮或按键。");
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                kCaptureTimeout - (Clock::now() - g_capture.started));
            ImGui::TextColored(kGrey, "按 Esc 取消（剩余 %lld 秒）",
                               static_cast<long long>(std::max<std::int64_t>(0, left.count())));
        }
        ImGui::EndPopup();
    }
}

// ── Multiplayer ─────────────────────────────────────────────────────────────

void DoJoin() {
    Multiplayer::Config c = FieldsToConfig();
    if (!Multiplayer::ParseInvite(g_invite, &c)) {
        MpMessage("请先输入主持人的邀请码或地址（例如 26.12.34.56:24872）。", true);
        return;
    }
    std::string error;
    if (Multiplayer::Join(c, &error)) {
        Multiplayer::Save(c);
        CopyTo(g_pass, c.password);
        g_port = c.port;
        MpMessage(g_show_join_address ? fmt::format("正在加入 {}:{}……", c.address, c.port)
                                      : std::string("正在加入……"),
                  false);
    } else {
        MpMessage("无法加入：" + error, true);
    }
}

/// The Ryujinx LDN side: where the RyuLdn lobbies live (Ryujinx LDN or Both).
void DrawRyuLdn() {
    static bool show_server = false;
    const std::string status = Multiplayer::RyuLdnStatusText();
    const bool ok = status.find("在大厅中") != std::string::npos;
    const bool bad = status.find("无服务器") != std::string::npos ||
                     status.find("未连接（") != std::string::npos;
    ImGui::TextColored(ok ? kGreen : (bad ? kRed : kGrey), "%s", status.c_str());
    if (const std::string server = Multiplayer::RyuLdnServer(); !server.empty()) {
        ImGui::TextColored(kGrey, "服务器：%s", show_server ? server.c_str() : "(hidden)");
        ImGui::SameLine();
        if (ImGui::SmallButton(show_server ? "隐藏##ryusrv" : "显示##ryusrv")) {
            show_server = !show_server;
        }
    }
    ImGui::TextWrapped(
        "大厅可在游戏内查找并加入（本地无线游玩），并与"
        "同一服务器上的 Ryujinx 玩家一起；公共房间标签页会列出这些大厅。");

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
            choice_message = "无法切换：" + error;
            choice_bad = true;
        }
    };

    SectionTitle("服务器");
    if (ImGui::RadioButton("Ryujinx 公共服务器", choice.mode == Mode::Public) &&
        choice.mode != Mode::Public) {
        choose({Mode::Public, {}}, "正在使用 Ryujinx 的公共服务器。");
    }
    HelpMarker("ldn.ryujinx.app：所有人的公共大厅，无需设置。");
    if (ImGui::RadioButton("为好友主持（Radmin VPN）", choice.mode == Mode::Host) &&
        choice.mode != Mode::Host) {
        choose({Mode::Host, {}}, "您的服务器正在运行；好友可通过您的地址加入。");
    }
    HelpMarker("本游戏会运行大厅服务器；好友可连接到您的地址（Radmin VPN 或"
               "家庭网络）。无需公共服务器即可运行。Windows 可能会询问是否允许"
               "游戏访问网络：请允许。");
    if (ImGui::RadioButton("加入好友的服务器", choice.mode == Mode::Friend) &&
        choice.mode != Mode::Friend) {
        if (friend_address[0] != '\0') {
            choose({Mode::Friend, friend_address}, "正在使用好友的服务器。");
        } else {
            choice_message = "在下方输入好友的地址，然后按“使用”。";
            choice_bad = false;
        }
    }

    if (choice.mode == Mode::Host) {
        const auto own = Multiplayer::GetOwnServerStatus();
        if (own.running) {
            ImGui::TextColored(kGreen, "您的服务器：%zu 个已连接，%zu 个大厅（端口 %u）",
                               own.connections, own.lobbies, own.port);
        } else {
            ImGui::TextColored(kRed, "您的服务器未运行%s%s", own.error.empty() ? "" : ": ",
                               own.error.c_str());
        }
        ImGui::TextUnformatted("将以下地址之一提供给好友：");
        const auto addresses = Multiplayer::LocalAddresses();
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            const auto& a = addresses[i];
            ImGui::PushID(static_cast<int>(i) + 7000);
            if (ImGui::SmallButton("复制")) {
                ImGui::SetClipboardText(a.ip.c_str());
                Notify("地址已复制到剪贴板");
            }
            ImGui::SameLine();
            const bool shown = g_shown_addresses.contains(i + 7000);
            if (ImGui::SmallButton(shown ? a.ip.c_str() : "显示地址")) {
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
        ImGui::TextWrapped("使用本游戏的好友：加入好友的服务器，粘贴地址。"
                           "Ryujinx 好友：多人模式 RyuLDN，以你的地址作为 "
                           "LDN 服务器。然后在游戏的本地联机中主持或搜索。");
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##friendsrv", "好友地址，例如 26.12.34.56", friend_address,
        sizeof(friend_address),
        ImGuiInputTextFlags_EnterReturnsTrue |
            (show_friend ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password));
    ImGui::SameLine();
    if (ImGui::Button(show_friend ? "隐藏##fs" : "显示##fs")) {
        show_friend = !show_friend;
    }
    ImGui::SameLine();
    if (ImGui::Button("粘贴##fs")) {
        if (char* clip = SDL_GetClipboardText()) {
            CopyTo(friend_address, clip);
            SDL_free(clip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("使用##fs") || enter) {
        choose({Mode::Friend, friend_address}, "正在使用好友的服务器。");
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
        ImGui::TextColored(kYellow, "本地联机已设为仅使用 suyu 房间（ini backend = room）。");
    }
    if (ImGui::Button("刷新")) {
        Multiplayer::RefreshPublicRooms();
    }
    ImGui::SameLine();
    if (list.refreshing) {
        ImGui::TextColored(kGrey, "正在查找大厅……");
    } else if (list.refreshed) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - list.last_refresh);
        ImGui::TextColored(kGrey, "%zu lobbies%s  -  updated %lld s ago", list.rooms.size(),
                           list.private_passphrase ? "（你的密码）" : "",
                           static_cast<long long>(age.count()));
    }
    HelpMarker("你（以及 Ryujinx 玩家）可加入的本游戏大厅：打开本地联机"
               "并在游戏中搜索，如同在 Switch 上。在游戏中主持大厅会将其"
               "显示给所有人。设置密码后，只有相同密码的大厅"
               "才会显示。");
    for (const auto& error : list.errors) {
        ImGui::TextColored(kRed, "%s", error.c_str());
    }

    const float rows_height = ImGui::GetFontSize() * 12.0f;
    if (ImGui::BeginTable("publicrooms", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH,
                          ImVec2(0.0f, rows_height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("主持");
        ImGui::TableSetupColumn("玩家", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("服务器", ImGuiTableColumnFlags_WidthFixed,
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
        ImGui::TextColored(kGrey, "当前没有开放的大厅。");
    }

    SectionTitle("服务器");
    ImGui::BulletText("本机 - 你主持时，大厅会在此托管");
    ImGui::BulletText("Ryujinx（%s） - 公共大厅", Multiplayer::DefaultRyuLdnHost);
    HelpMarker("主持会为 suyu 玩家开启房间，并在此 PC 上"
               "为 Ryujinx 玩家开启服务器；游戏内搜索也列出 Ryujinx 公共大厅及"
               "你所加入房间的好友的大厅。game_settings.ini ryuldn_public = off "
               "会排除公共服务器。");
    const auto extra = Multiplayer::ExtraRyuLdnServers();
    for (std::size_t i = 0; i < extra.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::BulletText("服务器 %zu：%s", i + 2,
                          show_servers ? extra[i].c_str() : "(hidden)");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            Multiplayer::RemoveExtraRyuLdnServer(extra[i]);
            server_message = "已移除。";
        }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##newserver", "其他服务器、主机或主机:端口", new_server, sizeof(new_server),
        ImGuiInputTextFlags_EnterReturnsTrue |
            (show_servers ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password));
    ImGui::SameLine();
    if (ImGui::Button("添加") || enter) {
        std::string error;
        if (Multiplayer::AddExtraRyuLdnServer(new_server, &error)) {
            new_server[0] = '\0';
            server_message = error.empty()
                                 ? "已添加：其大厅会显示在此处及游戏搜索中。"
                                 : "已添加；" + error;
            Multiplayer::RefreshPublicRooms();
        } else {
            server_message = "无法添加：" + error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(show_servers ? "隐藏##srv" : "显示##srv")) {
        show_servers = !show_servers;
    }
    HelpMarker("更多 RyuLdn 服务器（例如你团队运行的）：它们的大厅会"
               "列出并可加入，与 Ryujinx 服务器并列。");
    if (!server_message.empty()) {
        ImGui::TextColored(kGrey, "%s", server_message.c_str());
    }
}

void DrawRoom();

// "本地多人游戏" tab: suyu rooms, Ryujinx LDN and the public room list.
void DrawLocalMultiplayer() {
    // The switch: which network carries local play (applies at once). Both = automatic.
    static std::string mode_message;
    using LdnMode = Multiplayer::LdnMode;
    const LdnMode mode = Multiplayer::CurrentLdnMode();
    ImGui::TextUnformatted("连接方式：");
    const auto pick = [&](const char* label, LdnMode value) {
        ImGui::SameLine();
        if (ImGui::RadioButton(label, mode == value) && mode != value) {
            std::string error;
            mode_message = Multiplayer::SetLdnMode(value, &error) ? std::string{} : error;
        }
    };
    pick("Suyu 房间", LdnMode::Room);
    pick("Ryujinx LDN", LdnMode::RyuLdn);
    pick("两者（自动）", LdnMode::Auto);
    HelpMarker("两者（默认）：主持会自动开启房间和服务器，加入时会"
               "按大厅需求选择。Suyu 房间 / Ryujinx LDN 仅用一侧。立即"
               "生效（请先离开大厅）。");
    if (!mode_message.empty()) {
        ImGui::TextColored(kRed, "无法切换：%s", mode_message.c_str());
    }

    const std::string bridge = Multiplayer::BridgeStatusText();
    if (!bridge.empty()) {
        ImGui::TextColored(kGrey, "%s", bridge.c_str());
    }

    if (ImGui::BeginTabBar("mptabs")) {
        if (mode != LdnMode::RyuLdn && ImGui::BeginTabItem("加入房间")) {
            DrawRoom();
            ImGui::EndTabItem();
        }
        if (mode != LdnMode::Room && ImGui::BeginTabItem("Ryujinx LDN")) {
            DrawRyuLdn();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("公共房间")) {
            DrawPublicRooms();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// "在线多人游戏" tab: your online (NEX) server, separate from the local networks.
void DrawOnlineMultiplayer() {
    static bool online_loaded = false;
    if (!online_loaded) {
        online_loaded = true;
        const std::string v = GS::Value("多人游戏", "online_server_host");
        std::snprintf(g_online_server, sizeof(g_online_server), "%s", v.c_str());
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputTextWithHint("在线服务器 IP", "IP 或主机（留空 = 关闭）", g_online_server,
                             sizeof(g_online_server));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::string v = g_online_server;
        v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char c) { return std::isspace(c); }),
                v.end());
        if (const auto colon = v.find(':'); colon != std::string::npos) {
            v.resize(colon); // ports are fixed by the game
        }
        std::snprintf(g_online_server, sizeof(g_online_server), "%s", v.c_str());
        GS::SetValue("多人游戏", "online_server_host", v);
    }
    HelpMarker("你的在线（NEX）服务器：IPv4 地址或主机名。host:port 只保留"
               "主机（端口由游戏固定）。留空 = 关闭。下次游戏"
               "连接时生效；请在启动前设置。与本地多人游戏分开。");
    ImGui::TextColored(kGrey, "在线服务器：%s", g_online_server[0] ? g_online_server : "(off)");
}

void DrawMultiplayer() {
    // One name for every network: online server, suyu rooms and Ryujinx lobbies.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputText("名称", g_nick, sizeof(g_nick));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Multiplayer::Save(FieldsToConfig());
    }
    HelpMarker("会在线显示给其他玩家，在 suyu 房间和 Ryujinx 大厅（公共"
               "房间）中。4-20 个字符：字母、数字、空格、. _ -。应用到下次"
               "你主持或加入的房间或大厅。");

    if (ImGui::BeginTabBar("mpmain")) {
        if (ImGui::BeginTabItem("本地多人游戏")) {
            DrawLocalMultiplayer();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("在线多人游戏")) {
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
        ImGui::TextColored(kGrey, "离线");
        break;
    case Phase::Connecting:
        ImGui::TextColored(kYellow, "连接中……");
        break;
    case Phase::Reconnecting:
        ImGui::TextColored(kYellow, "连接断开 - 正在重连（第 %d 次尝试）",
                           st.reconnect_attempt);
        break;
    case Phase::Hosting:
    case Phase::Joined:
        ImGui::TextColored(kGreen, "%s '%s'  -  %zu/%u players  -  you are %s",
                           st.phase == Phase::Hosting ? "主持中" : "已加入", st.room.c_str(),
                           st.members.size(), st.slots, st.nickname.c_str());
        break;
    }
    if (st.phase != Phase::Offline) {
        ImGui::SameLine();
        if (ImGui::Button(st.phase == Phase::Hosting ? "关闭房间" : "离开")) {
            Multiplayer::Leave();
            MpMessage("已离开房间。", false);
        }
    }
    for (std::size_t i = 0; i < st.members.size(); ++i) {
        ImGui::BulletText("%s", st.members[i].c_str());
        if (st.phase == Phase::Hosting && i < st.names.size() && st.names[i] != st.nickname) {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("踢出")) {
                Multiplayer::Kick(st.names[i]);
                MpMessage(fmt::format("已踢出 {}。", st.names[i]), false);
            }
            ImGui::PopID();
        }
    }
    if (!st.last_error.empty()) {
        ImGui::TextColored(kRed, "上次问题：%s", st.last_error.c_str());
    }
    if (!g_mp_message.empty()) {
        ImGui::TextColored(g_mp_message_bad ? kRed : kGrey, "%s", g_mp_message.c_str());
    }

    SectionTitle("加入好友");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##invite", "邀请码或地址，例如 26.12.34.56", g_invite, sizeof(g_invite),
        ImGuiInputTextFlags_EnterReturnsTrue |
            (g_show_join_address ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password));
    ImGui::SameLine();
    if (ImGui::Button(g_show_join_address ? "隐藏" : "显示")) {
        g_show_join_address = !g_show_join_address;
    }
    ImGui::SameLine();
    if (ImGui::Button("粘贴")) {
        if (char* clip = SDL_GetClipboardText()) {
            CopyTo(g_invite, clip);
            SDL_free(clip);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("加入") || enter) {
        DoJoin();
    }
    HelpMarker("粘贴好友从其菜单复制的邀请码（地址:端口，若房间有密码则"
               "加上 #password），或输入 Radmin VPN 地址。一个地址覆盖两种"
               "场景：你加入他们的房间，也能看到其服务器上的大厅。然后打开"
               "游戏的本地联机并加入他们的大厅。");
    const auto recent = Multiplayer::RecentHosts();
    if (!recent.empty()) {
        ImGui::TextColored(kGrey, "之前加入过：");
        for (std::size_t i = 0; i < recent.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string name =
                g_show_join_address ? recent[i] : fmt::format("主持 {}", i + 1);
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

    SectionTitle("主持中");
    ImGui::TextWrapped(
        "无需操作：当你在游戏的本地联机中主持时，本机会自动开启房间和"
        "服务器，suyu 和 Ryujinx 好友会加入同一大厅。这些是"
        "房间选项：");
    const float field = ImGui::GetFontSize() * 9.0f;
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("房间名称", g_room, sizeof(g_room));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field);
    ImGui::InputText("密码", g_pass, sizeof(g_pass), ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(field);
    ImGui::SliderInt("玩家", &g_max_players, 2, 8);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field);
    ImGui::InputInt("端口", &g_port, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Multiplayer::Save(FieldsToConfig());
    }
    const auto own = Multiplayer::GetOwnServerStatus();
    if (own.running) {
        ImGui::TextColored(kGreen, "主持中：%zu 个连接，%zu 个大厅（端口 %u）",
                           own.connections, own.lobbies, own.port);
    } else if (!own.error.empty()) {
        ImGui::TextColored(kRed, "你的服务器无法启动：%s", own.error.c_str());
    }
    ImGui::TextColored(kGrey, "好友可通过你的地址加入（“复制”会把邀请放到 "
                              "剪贴板；点击地址可显示）。Ryujinx 好友将 "
                              "此地址设为其 LDN 服务器：");
    const auto addresses = Multiplayer::LocalAddresses();
    bool any_vpn = false;
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        const auto& a = addresses[i];
        any_vpn |= a.label == "Radmin VPN" || a.label == "Hamachi" || a.label == "ZeroTier" ||
                   a.label == "Tailscale";
        ImGui::PushID(static_cast<int>(i) + 5000);
        if (ImGui::SmallButton("复制邀请")) {
            const std::string code =
                Multiplayer::InviteCode(a.ip, static_cast<u16>(std::clamp(g_port, 1, 65535)), g_pass);
            ImGui::SetClipboardText(code.c_str());
            Notify("邀请已复制到剪贴板");
        }
        ImGui::SameLine();
        const bool shown = g_shown_addresses.contains(i);
        if (ImGui::SmallButton(shown ? a.ip.c_str() : "显示地址")) {
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
        ImGui::TextColored(kYellow, "未找到 Radmin VPN 地址。");
        HelpMarker("通过互联网时，双方都安装 Radmin VPN 并加入同一 Radmin "
                   "网络，然后把 Radmin 邀请复制到这里。在同一家庭网络中，"
                   "家庭网络地址也可用。");
    }

    SectionTitle("你");
    ImGui::TextUnformatted("游戏启动时：");
    ImGui::SameLine();
    bool startup_changed = ImGui::RadioButton("保持离线", &g_startup, 0);
    ImGui::SameLine();
    startup_changed |= ImGui::RadioButton("主持", &g_startup, 1);
    ImGui::SameLine();
    startup_changed |= ImGui::RadioButton("加入上一个主持", &g_startup, 2);
    if (startup_changed) {
        Multiplayer::Save(FieldsToConfig());
    }
    if (ImGui::Checkbox("连接断开时自动重连", &g_reconnect)) {
        Multiplayer::SetAutoReconnect(g_reconnect);
    }

    const auto events = Multiplayer::Events();
    if (!events.empty()) {
        SectionTitle("房间事件");
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
    {"显示", "fps", "帧率", "30|60|90|120|144|自动"},
    {"显示", "resolution", "分辨率",
     "默认|自动|1920x1080|2560x1080|2560x1440|3440x1440|3840x2160"},
    {"显示", "aspect", "宽高比", "16:9|21:9|32:9|16:10|4:3"},
    {"显示", "fullscreen", "启动时全屏", ""},
    {"显示", "show_fps", "窗口标题显示 FPS", ""},
    {"显示", "console_mode", "控制台模式", "底座|掌机"},
    {"画面", "renderer", "渲染器", "模拟|直接", true},
    {"画面", "native_render", "按设定分辨率渲染", ""},
    {"超分辨率", "nvn_fsr_quality", "质量", "原生 AA|质量|均衡|性能|极致性能", true},
    {"超分辨率", "nvn_fsr_sharpness", "锐化", ""},
    {"超分辨率", "render_scale_override", "覆盖渲染缩放", ""},
    {"超分辨率", "nvn_fsr_split_scale", "低于输出分辨率渲染（分离缩放）", ""},
    {"诊断", "diagnostics", "运行报告", ""},
    {"诊断", "gpu_diagnostics", "运行报告（替换渲染器）", ""},
    {"诊断", "log_redact_paths", "日志中隐藏个人数据", ""},
    {"诊断", "nvn_call_sampling", "NVN 调用采样", ""},
    {"诊断", "nvn_perf_gpu", "GPU 时间戳", "1|0"},
    {"诊断", "nvn_fsr_texel_stats", "FSR 纹素统计", "1|0"},
    {"诊断", "nvn_bench", "基准测试模式", "0|1"},
    {"诊断", "nvn_bench_scene", "基准测试场景", ""},
    {"诊断", "nvn_bench_warmup", "基准预热（秒）", ""},
    {"诊断", "nvn_bench_measure", "基准测量（秒）", ""},
    {"诊断", "nvn_upscaler_probe", "超分辨率探针", ""},
    {"诊断", "vulkan_validation", "Vulkan 验证层", ""},
    {"诊断", "probes", "抖动卡顿探针", ""},
    {"显示", "rtss_overlay", "允许 RivaTuner 叠加层", ""},
    {"菜单", "key", "菜单键", "F10|F9|F8|Home|无"},
    {"菜单", "show_advanced", "高级模式", ""},
    {"菜单", "pad_combo", "菜单控制器组合键",
     "返回+开始|返回+指南|左摇杆+右摇杆|无"},
#ifdef _WIN32
    {"模组", "loader", "模组加载器", "Forge/forge.dll|无"},
#else
    {"模组", "loader", "模组加载器", "Forge/forge.so|无"},
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
        const std::string v = GS::Value("菜单", "show_advanced");
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
        if (o.section == "多人游戏") {
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
    if (o.section == "显示" && o.key == "fps" && v != "auto" && !in_range(10.0f, 360.0f)) {
        return "帧率：10 到 360 的数字，或自动。";
    }
    if (o.section == "超分辨率" && o.key == "nvn_fsr_sharpness" && !in_range(0.0f, 1.0f)) {
        return "锐化：0 到 1 的数字。";
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
    LOG_INFO(Frontend, "游戏菜单：[{}] {} = {}", o.section, o.key, value);
    bool live = GS::ApplyLive(o.section, o.key, value);
    if (o.section == "菜单") {
        ReadMenuSettings();
        g_adv_next = {}; // Advanced mode: the tab and the rows follow at once
        live = true;
    }
    if (live) {
        g_settings_message = fmt::format("{}：{}（已应用）", label, value);
    } else {
        g_settings_message = fmt::format("{}：{}（已保存；重启游戏后生效）", label, value);
        if (std::find(g_restart_needed.begin(), g_restart_needed.end(), label) ==
            g_restart_needed.end()) {
            g_restart_needed.push_back(label);
        }
    }
    g_settings_message_bad = false;
}

void DrawAdvancedRow(Settings::BasicSetting& setting);
void DrawAdvancedValue(Settings::BasicSetting& setting);

bool g_restart_confirm{}; // "立即重启" was clicked once: ask before closing
bool g_restarting{};      // the new process is started and this one is closing
std::string g_restart_error; // why the last restart could not start the new process

/// The yellow "重启游戏以应用：..." line (settings read only at start that changed this session) with
/// a "立即重启" button beside it: a second click confirms, then the game starts again with the same command line
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
    ImGui::TextColored(kYellow, "重启游戏以应用：%s", list.c_str());
    if (!g_restart_error.empty()) {
        ImGui::TextColored(kRed, "%s", g_restart_error.c_str());
    }
    if (g_restarting) {
        ImGui::SameLine();
        ImGui::TextColored(kYellow, "（正在重启……）");
        return;
    }
    ImGui::SameLine();
    if (!g_restart_confirm) {
        if (ImGui::SmallButton("立即重启")) {
            g_restart_confirm = true;
            g_restart_error.clear();
        }
        return;
    }
    ImGui::TextColored(kYellow, "关闭游戏并重新启动？");
    ImGui::SameLine();
    if (ImGui::SmallButton("是，重启")) {
        std::string error;
        unsigned long pid = 0;
        if (Relaunch::StartNewInstance(&error, &pid)) {
            LOG_INFO(Frontend, "游戏菜单：重启：新实例已启动（pid {}），正在关闭当前实例", pid);
            g_restarting = true;
            g_settings_message = "正在重启：游戏将关闭并重新启动。";
            g_settings_message_bad = false;
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT; // the normal close path (window closed): saves and shuts down cleanly
            SDL_PushEvent(&quit);
        } else {
            LOG_ERROR(Frontend, "游戏菜单：重启失败：{}", error);
            g_restart_error = "重启失败：" + error + "。游戏继续运行。";
            g_restart_confirm = false;
            return;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("取消")) {
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

/// "超分辨率": Off | FSR 3.1 | FSR 4. Stored as [Upscaler] nvn_fsr (bool) + nvn_fsr_backend (3 | 4).
void DrawUpscalerChoiceRow(const UpState& up, bool grey) {
    constexpr const char* kNames[] = {"关闭", "FSR 3.1", "FSR 4"};
    ImGui::PushID("upscaler_choice");
    ImGui::BeginDisabled(grey);
    RowStart("超分辨率",
             "[Upscaler] nvn_fsr + nvn_fsr_backend

关闭、FSR 3.1 (Vulkan) 或 FSR 4 (通过显卡驱动的 DX12 库，"
             "回退到 3.1)。需要 Direct 渲染器。重启后生效。");
    const int cur = static_cast<int>(up.choice);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("##value", kNames[cur])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kNames[i], i == cur) && i != cur) {
                GS::SetUpscalerChoice(static_cast<GS::UpscalerChoice>(i));
                LOG_INFO(Frontend, "游戏内菜单: 超分辨率 = {}", kNames[i]);
                g_settings_message = fmt::format("超分辨率: {} (已保存; 重启游戏后生效)", kNames[i]);
                g_settings_message_bad = false;
                MarkRestart("超分辨率");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

/// "渲染缩放": the saved Graphics > Resolution scale (suyu's resolution_setup, kept as the backing store). Greyed and
/// showing the effective value while an upscaler's Quality mode decides it.
void DrawRenderScaleRow(const UpState& up) {
    ImGui::PushID("render_scale");
    RowStart("渲染缩放",
             "画面以高于游戏自身分辨率的清晰度绘制 (1X = 原生)。不是窗口大小："
             "那是显示 > 分辨率。使用替换渲染器时重启后生效。

当超分辨率"
             "开启时 (且高级模式的覆盖渲染缩放关闭)，此值由质量模式决定。");
    if (up.by_mode) {
        ImGui::BeginDisabled(true);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::BeginCombo("##value", fmt::format("{}", GS::ResolutionStepName(up.effective_step)).c_str())) {
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TextColored(kGrey, "由超分辨率的质量模式设定 (%s)。已保存的值会保留。", up.why.c_str());
    } else {
        DrawAdvancedValue(Settings::values.resolution_setup);
    }
    ImGui::PopID();
}

/// "详细日志": suyu's log_filter *:Debug (on) or the default *:Info (off), applied to the running logger at once.
void DrawVerboseLogRow(const UpState& up) {
    ImGui::PushID("verbose_log");
    RowStart("详细日志",
             "suyu 的 log_filter：开 = *:Debug (suyu 日志大得多)，关 = 默认 *:Info。立即生效并"
             "随 suyu 配置保存。切换此项时会替换自定义 log_filter。");
    bool on = up.verbose;
    if (ImGui::Checkbox("##value", &on)) {
        const std::string filter_text = on ? "*:Debug" : "*:Info";
        Settings::values.log_filter.SetValue(filter_text);
        Common::Log::Filter filter;
        filter.ParseFilterString(filter_text);
        Common::Log::SetGlobalFilter(filter);
        Controls::SaveConfig();
        LOG_INFO(Frontend, "游戏内菜单: 详细日志 {} (log_filter = {})", on ? "on" : "off", filter_text);
        g_settings_message = fmt::format("详细日志: {} (已立即生效)", on ? "on" : "off");
        g_settings_message_bad = false;
    }
    ImGui::PopID();
}

void DrawSettings() {
    ImGui::TextWrapped("游戏旁 game_settings.ini 中的选项。更改会立即"
                       "保存。帧率、FPS 显示和菜单按钮立即生效；"
                       "其余在下次启动时生效。");
    if (ImGui::Button("打开 game_settings.ini")) {
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
        if (finished == "画面") {
            DrawRenderScaleRow(up);
        } else if (finished == "诊断") {
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
            if (section == "超分辨率") {
                if (!up.direct) {
                    ImGui::TextColored(kYellow, "FSR 需要 Direct 渲染器：将渲染器 (画面) 设为 Direct 并重启。");
                } else if (up.by_mode) {
                    ImGui::TextColored(kGrey, "渲染缩放 (画面) 由质量模式决定。");
                }
            }
            table_open = ImGui::BeginTable(section.c_str(), 2);
            if (table_open) {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, label_width);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                if (section == "超分辨率") {
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
        const bool grey = o.section == "超分辨率" &&
                          (!up.direct || ((o.key == "nvn_fsr_quality" || o.key == "nvn_fsr_sharpness") &&
                                          up.choice == GS::UpscalerChoice::Off));

        ImGui::PushID(id.c_str());
        ImGui::BeginDisabled(grey);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        HelpMarker(fmt::format("[{}] {} (默认 {})

{}", o.section, o.key, o.default_value,
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
        } else if (o.section == "超分辨率" && o.key == "nvn_fsr_sharpness") {
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
        return "音频";
    case Category::Core:
        return "核心";
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
        return "画面";
    case Category::System:
    case Category::Services:
    case Category::LibraryApplet:
        return "系统";
    case Category::DataStorage:
        return "存储";
    case Category::Debugging:
    case Category::DebuggingGraphics:
        return "调试";
    case Category::Linux:
    case Category::Miscellaneous:
    case Category::AddOns:
        return "杂项";
    case Category::Network:
        return "网络";
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
            g_adv_message = fmt::format("{}: '{}' 未被接受 (保留 {})", setting.GetLabel(),
                                        value, after);
            g_adv_message_bad = true;
        }
        return;
    }
    LOG_INFO(Frontend, "游戏内菜单: 高级 {} = {}", setting.GetLabel(), after);
    Controls::SaveConfig();
    if (g_system != nullptr && setting.RuntimeModifiable()) {
        g_system->ApplySettings();
        g_adv_message = fmt::format("{}：{}（已应用）", setting.GetLabel(), after);
    } else {
        g_adv_message =
            fmt::format("{}：{}（已保存；重启游戏后生效）", setting.GetLabel(), after);
        // read only at start: listed for the Settings tab's "立即重启" line too
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
            if (shown.rfind("分辨率", 0) == 0 && shown.size() > 3 && std::isdigit(shown[3])) {
                shown = PrettyResolution(shown);
            }
            if (ImGui::BeginCombo("##value", shown.c_str())) {
                for (const auto& [name, value] : names) {
                    const std::string label =
                        name.rfind("分辨率", 0) == 0 && name.size() > 3 && std::isdigit(name[3])
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
    std::string help = fmt::format("{} (默认 {})", setting.GetLabel(), setting.DefaultToString());
    if (setting.Ranged()) {
        help += fmt::format("
范围 {} 到 {}", setting.MinVal(), setting.MaxVal());
    }
    if (!setting.RuntimeModifiable()) {
        help += "
下次启动游戏时生效。";
    }
    HelpMarker(help.c_str());
    ImGui::TableNextColumn();
    DrawAdvancedValue(setting);
    ImGui::SameLine();
    if (setting.ToString() != setting.DefaultToString() && ImGui::SmallButton("重置")) {
        CommitAdvanced(setting, setting.DefaultToString());
    }
    ImGui::PopID();
}

void DrawAdvanced() {
    ImGui::TextWrapped("suyu 自身配置的所有设置，按配置选项卡分组。"
                       "更改在游戏关闭时随游戏配置保存；"
                       "运行时可改的设置立即生效，其余在下次启动时生效。"
                       "控制和多人游戏有各自的选项卡。");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    ImGui::InputText("搜索", g_adv_filter, sizeof(g_adv_filter));
    if (!g_adv_message.empty()) {
        ImGui::TextColored(g_adv_message_bad ? kRed : kGrey, "%s", g_adv_message.c_str());
    }
    DrawRestartLine();
    std::string needle = g_adv_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Group the categories under their headings, in a fixed order.
    constexpr const char* kGroups[] = {"画面", "CPU",     "音频",   "系统",
                                       "存储",  "网络", "调试",   "杂项"};
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
    ImGui::Text("%s", game_name.empty() ? "游戏" : game_name.c_str());
    ImGui::TextColored(kGrey, "帧率: %s FPS", g_fps_text.c_str());

    SectionTitle("文件夹");
    const auto exe_dir = Common::FS::GetExeDirectory();
    if (ImGui::Button("存档、配置和日志 (用户)")) {
        OpenPath(exe_dir / "user");
    }
    ImGui::SameLine();
    if (ImGui::Button("模组")) {
        OpenPath(exe_dir / "mods");
    }
    ImGui::SameLine();
    if (ImGui::Button("密钥")) {
        OpenPath(Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir));
    }
    if (ImGui::Button("打开 game_settings.ini")) {
        OpenPath(exe_dir / "game_settings.ini");
    }
    HelpMarker("帧率、分辨率、主机模式、绘制距离等。那里的更改"
               "在下次游戏启动时生效。");

    SectionTitle("找到的模组");
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
        ImGui::TextColored(kGrey, "无 (将模组放入 mods/<title id>/)");
    }

    SectionTitle("密钥");
    if (!g_key_name.empty()) {
        ImGui::BulletText("%s: this menu", g_key_name.c_str());
    }
    if (!g_combo_name.empty()) {
        ImGui::BulletText("按住控制器 %s 1 秒：打开此菜单", g_combo_name.c_str());
    }
    ImGui::BulletText("F11 或 Alt+Enter：全屏");
#ifdef _WIN32
    ImGui::BulletText("F12：调试面板");
#endif
    ImGui::BulletText("Esc (在此窗口)：关闭菜单");
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
        ImGui::TextColored(kGrey, "此处的设置会立即保存。");
    }
    ImGui::SameLine();
    const float back_width =
        ImGui::CalcTextSize("返回游戏").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, ImGui::GetContentRegionAvail().x - back_width));
    if (ImGui::Button("返回游戏")) {
        g_close_requested = true;
    }
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("控制")) {
            ImGui::BeginChild("controls");
            DrawControls();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("多人游戏")) {
            ImGui::BeginChild("multiplayer");
            DrawMultiplayer();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("设置")) {
            ImGui::BeginChild("settings");
            DrawSettings();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ShowAdvancedTab() && ImGui::BeginTabItem("高级")) {
            ImGui::BeginChild("advanced");
            DrawAdvanced();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("通用")) {
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
    // zh patch: prefer a CJK font so the translated menu can render Han glyphs
    static constexpr const char* kCjkFonts[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\Deng.ttf",
#endif
    };
    std::error_code cjk_ec;
    const ImWchar* cjk_ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    for (const char* path : kCjkFonts) {
        if (std::filesystem::exists(path, cjk_ec) &&
            io.Fonts->AddFontFromFileTTF(path, 18.0f, nullptr, cjk_ranges)) {
            ImGui::GetStyle().FontScaleDpi = scale;
            return;
        }
    }
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
    const std::string title = (game_name.empty() ? std::string("游戏") : game_name) + " - 菜单";
    g_window = SDL_CreateWindow(title.c_str(), static_cast<int>(820 * scale),
                                static_cast<int>(720 * scale),
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (g_window == nullptr) {
        LOG_ERROR(Frontend, "游戏内菜单: 无窗口: {}", SDL_GetError());
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
        LOG_INFO(Frontend, "游戏内菜单: 进程中有 RivaTuner 钩子，软件渲染器 ({})",
                 g_renderer != nullptr ? "ok" : SDL_GetError());
    }
    if (g_renderer == nullptr) {
        g_renderer = SDL_CreateRenderer(g_window, nullptr);
    }
    if (g_renderer == nullptr) {
        g_renderer = SDL_CreateRenderer(g_window, SDL_SOFTWARE_RENDERER);
    }
    if (g_renderer == nullptr) {
        LOG_ERROR(Frontend, "游戏内菜单: 无渲染器: {}", SDL_GetError());
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
    LOG_INFO(Frontend, "游戏内菜单: 已打开");
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
    LOG_INFO(Frontend, "游戏内菜单: 已关闭");
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
    LOG_INFO(Frontend, "游戏内菜单: 按键 {}，控制器组合 {}",
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
