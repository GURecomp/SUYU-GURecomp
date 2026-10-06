// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/param_package.h"

class SdlConfig;

namespace Core {
class System;
}
namespace InputCommon {
class InputSubsystem;
}

/**
 * Player 1's controls in the exported game, shared by the game menu, the F12 panel and the
 * start-up check. sdl2-config.ini [Controls] controls_set_by records who chose the mapping:
 *   ""     never set (the keyboard defaults)
 *   suyu   copied from suyu's own controller setup by the export
 *   auto   the game mapped a connected controller by itself
 *   player the player changed it in the game (never replaced automatically)
 * A connected controller is mapped automatically only while nobody chose anything: the
 * mapping is still the default keyboard layout and the flag isn't "player" (or the game
 * mapped one itself earlier and that one is gone).
 */
namespace Controls {

void Init(Core::System& system, InputCommon::InputSubsystem* input, SdlConfig* config);

/// Connected controllers (no keyboard/mouse/"any" entries).
std::vector<Common::ParamPackage> Devices();

std::string SetBy();
/// "chosen by you", "copied from suyu", ... for the menu.
std::string SetByText();
/// What Player 1 uses: a controller's name, "Keyboard (default layout)", ...
std::string PlayerOneText();
bool IsDefaultKeyboard();

/// Maps every button, stick and motion of the device to Player 1 and saves.
void UseDevice(const Common::ParamPackage& device, const std::string& set_by);
/// The keyboard layout the game ships with.
void UseDefaultKeyboard(const std::string& set_by);
/// One entry (button index, or stick index when analog), set by the player. Empty = cleared.
void SetBinding(bool analog, std::size_t index, const std::string& param);
/// Swaps two buttons' bindings (A/B, X/Y), set by the player.
void SwapButtons(std::size_t a, std::size_t b);
/// Back to the defaults and to automatic controller pick-up.
void ResetToAutomatic();

/// A controller was connected (or found at start): maps it when allowed (see above).
/// Returns the device name it mapped, or "" when it left the mapping alone.
std::string OnDeviceAdded();

/// The current binding of a button/stick as text, e.g. "Button 0 (Xbox Controller)".
std::string BindingText(bool analog, std::size_t index);

} // namespace Controls
