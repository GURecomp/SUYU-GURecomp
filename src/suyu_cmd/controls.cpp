// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <utility>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/settings.h"
#include "common/settings_input.h"
#include "core/core.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "suyu_cmd/controls.h"
#include "suyu_cmd/sdl_config.h"

namespace Controls {

namespace {

Core::System* g_system{};
InputCommon::InputSubsystem* g_input{};
SdlConfig* g_config{};
std::string g_set_by;

Settings::PlayerInput& PlayerOne() {
    // GetValue() hands back a reference to the live array.
    return Settings::values.players.GetValue()[0];
}

/// Real controllers: SDL pads, Joy-Cons, GameCube adapter (not keyboard, mouse, UDP motion).
bool IsController(const Common::ParamPackage& device) {
    const std::string engine = device.Get("engine", "");
    return engine == "sdl" || engine == "joycon" || engine == "gcpad";
}

bool IsKeyboardKey(const std::string& param, int code) {
    const Common::ParamPackage p{param};
    return p.Get("engine", "") == "keyboard" && p.Get("code", -1) == code;
}

bool IsKeyboardStick(const std::string& param, const std::array<int, 4>& keys) {
    const Common::ParamPackage p{param};
    if (p.Get("engine", "") != "analog_from_button") {
        return false;
    }
    static constexpr const char* kDirs[] = {"up", "down", "left", "right"};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!IsKeyboardKey(p.Get(kDirs[i], ""), keys[i])) {
            return false;
        }
    }
    return true;
}

/// The device a binding belongs to (engine + guid + port), "" for keyboard/none.
std::string DeviceOf(const std::string& param) {
    const Common::ParamPackage p{param};
    const std::string engine = p.Get("engine", "");
    if (engine.empty() || engine == "keyboard" || engine == "mouse" ||
        engine == "analog_from_button") {
        return {};
    }
    return fmt::format("{}|{}|{}", engine, p.Get("guid", ""), p.Get("port", 0));
}

std::string DeviceKey(const Common::ParamPackage& device) {
    return fmt::format("{}|{}|{}", device.Get("engine", ""), device.Get("guid", ""),
                       device.Get("port", 0));
}

/// Player 1's controller (the device most of its buttons are on), "" if none.
std::string PlayerOneDevice() {
    const auto& player = PlayerOne();
    for (const auto& b : player.buttons) {
        if (std::string d = DeviceOf(b); !d.empty()) {
            return d;
        }
    }
    for (const auto& a : player.analogs) {
        if (std::string d = DeviceOf(a); !d.empty()) {
            return d;
        }
    }
    return {};
}

bool DeviceConnected(const std::string& key) {
    for (const auto& device : Devices()) {
        if (DeviceKey(device) == key) {
            return true;
        }
    }
    return false;
}

void Apply(const std::string& set_by) {
    auto& player = PlayerOne();
    player.connected = true;
    if (g_system != nullptr) {
        g_system->HIDCore().ReloadInputDevices();
    }
    g_set_by = set_by;
    if (g_config != nullptr) {
        g_config->WriteControlsSetBy(set_by);
    }
}

} // namespace

void Init(Core::System& system, InputCommon::InputSubsystem* input, SdlConfig* config) {
    g_system = &system;
    g_input = input;
    g_config = config;
    g_set_by = config != nullptr ? config->ReadControlsSetBy() : std::string{};
    LOG_INFO(Frontend, "Controls: Player 1 = {} ({})", PlayerOneText(), SetByText());
}

std::vector<Common::ParamPackage> Devices() {
    std::vector<Common::ParamPackage> out;
    if (g_input == nullptr) {
        return out;
    }
    for (const auto& device : g_input->GetInputDevices()) {
        if (IsController(device)) {
            out.push_back(device);
        }
    }
    return out;
}

std::string SetBy() {
    return g_set_by;
}

std::string SetByText() {
    if (g_set_by == "player") {
        return "chosen by you";
    }
    if (g_set_by == "suyu") {
        return "copied from suyu's controller setup";
    }
    if (g_set_by == "auto") {
        return "picked automatically";
    }
    return "default";
}

bool IsDefaultKeyboard() {
    const auto& player = PlayerOne();
    for (std::size_t i = 0; i < SdlConfig::default_buttons.size(); ++i) {
        if (!IsKeyboardKey(player.buttons[i], SdlConfig::default_buttons[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < SdlConfig::default_analogs.size(); ++i) {
        if (!IsKeyboardStick(player.analogs[i], SdlConfig::default_analogs[i])) {
            return false;
        }
    }
    return true;
}

std::string PlayerOneText() {
    const std::string device = PlayerOneDevice();
    if (device.empty()) {
        return IsDefaultKeyboard() ? "Keyboard (default layout)" : "Keyboard (custom layout)";
    }
    for (const auto& d : Devices()) {
        if (DeviceKey(d) == device) {
            return d.Get("display", "Controller");
        }
    }
    return "A controller that isn't connected";
}

void UseDevice(const Common::ParamPackage& device, const std::string& set_by) {
    if (g_input == nullptr) {
        return;
    }
    auto& player = PlayerOne();
    for (const auto& [button, param] : g_input->GetButtonMappingForDevice(device)) {
        player.buttons[button] = param.Serialize();
    }
    for (const auto& [analog, param] : g_input->GetAnalogMappingForDevice(device)) {
        player.analogs[analog] = param.Serialize();
    }
    for (const auto& [motion, param] : g_input->GetMotionMappingForDevice(device)) {
        player.motions[motion] = param.Serialize();
    }
    LOG_INFO(Frontend, "Controls: Player 1 mapped to {} ({})", device.Get("display", "?"),
             set_by);
    Apply(set_by);
}

void UseDefaultKeyboard(const std::string& set_by) {
    auto& player = PlayerOne();
    for (std::size_t i = 0; i < SdlConfig::default_buttons.size(); ++i) {
        player.buttons[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_buttons[i]);
    }
    for (std::size_t i = 0; i < SdlConfig::default_analogs.size(); ++i) {
        const auto& k = SdlConfig::default_analogs[i];
        player.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(
            k[0], k[1], k[2], k[3], SdlConfig::default_stick_mod[i], 0.5f);
    }
    for (std::size_t i = 0; i < SdlConfig::default_motions.size(); ++i) {
        player.motions[i] = InputCommon::GenerateKeyboardParam(SdlConfig::default_motions[i]);
    }
    LOG_INFO(Frontend, "Controls: Player 1 uses the default keyboard layout");
    Apply(set_by);
}

void SetBinding(bool analog, std::size_t index, const std::string& param) {
    auto& player = PlayerOne();
    if (analog ? index >= player.analogs.size() : index >= player.buttons.size()) {
        return;
    }
    (analog ? player.analogs[index] : player.buttons[index]) = param;
    Apply("player");
}

void SwapButtons(std::size_t a, std::size_t b) {
    auto& player = PlayerOne();
    if (a >= player.buttons.size() || b >= player.buttons.size()) {
        return;
    }
    std::swap(player.buttons[a], player.buttons[b]);
    Apply("player");
}

void ResetToAutomatic() {
    UseDefaultKeyboard("");
    OnDeviceAdded();
}

std::string OnDeviceAdded() {
    if (g_set_by == "player") {
        return {};
    }
    const auto devices = Devices();
    if (devices.empty()) {
        return {};
    }
    // Only replace what nobody chose: the default keyboard, or an earlier automatic pick whose
    // controller is gone (a different pad plugged in instead).
    const bool replace_auto = g_set_by == "auto" && !DeviceConnected(PlayerOneDevice());
    if (!IsDefaultKeyboard() && !replace_auto) {
        return {};
    }
    UseDevice(devices.front(), "auto");
    return devices.front().Get("display", "Controller");
}

std::string BindingText(bool analog, std::size_t index) {
    const auto& player = PlayerOne();
    const std::string& param = analog ? player.analogs[index] : player.buttons[index];
    if (param.empty()) {
        return "(not set)";
    }
    const Common::ParamPackage p{param};
    const std::string engine = p.Get("engine", "");
    const auto key_name = [](int code) {
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
        return std::string(name != nullptr && *name != '\0' ? name : fmt::format("key {}", code));
    };
    if (engine == "keyboard") {
        return "Key " + key_name(p.Get("code", 0));
    }
    if (engine == "analog_from_button") {
        std::string out = "Keys";
        for (const char* dir : {"up", "down", "left", "right"}) {
            const Common::ParamPackage d{p.Get(dir, "")};
            out += " " + (d.Get("engine", "") == "keyboard" ? key_name(d.Get("code", 0))
                                                            : std::string("?"));
        }
        return out;
    }
    std::string what;
    if (p.Has("axis_x")) {
        what = fmt::format("Stick (axes {}, {})", p.Get("axis_x", 0), p.Get("axis_y", 1));
    } else if (p.Has("button")) {
        what = fmt::format("Button {}", p.Get("button", 0));
    } else if (p.Has("hat")) {
        what = fmt::format("D-pad {}", p.Get("direction", ""));
    } else if (p.Has("axis")) {
        what = fmt::format("Axis {}{}", p.Get("axis", 0), p.Get("invert", "+") == "-" ? "-" : "+");
    } else {
        what = engine;
    }
    return what;
}

} // namespace Controls
