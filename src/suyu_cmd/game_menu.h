// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

union SDL_Event;
struct SDL_Window;

namespace Core {
class System;
}
namespace InputCommon {
class InputSubsystem;
}

/**
 * The exported game's menu: a window of its own (Dear ImGui on SDL_Renderer), opened with a
 * key (game_settings.ini [Menu] key, F10) or by holding a controller combo ([Menu] pad_combo,
 * Minus + Plus). Controls (pick a controller, rebind, back to automatic), multiplayer (host,
 * join by invite or recent host, own VPN/LAN addresses, status) and the game's folders. Works
 * on Windows and Linux, fullscreen or not, with mouse, keyboard or controller. While it has
 * focus the game gets no controller input. The F12 panel (Windows) stays as it is.
 */
namespace GameMenu {

void Init(Core::System& system, InputCommon::InputSubsystem* input, SDL_Window* game_window);
void Shutdown();

bool IsOpen();
void Toggle();

/// Every SDL event goes through here first (main thread). true = it belonged to the menu
/// (its window, its key) and must not reach the game.
bool HandleEvent(const SDL_Event& event);

/// Each main-loop pass: watches the controller combo, draws the open menu.
void Update();

/// How long the main loop may wait for the next event (ms).
int WaitTimeoutMs();

/// Text for the game window's title ("Online 2/4", "Controller connected: ..."), or "".
std::string TitleNote();
/// Shows a short note in the title and the menu (e.g. a controller was mapped).
void Notify(const std::string& text);

/// The window measures the frame rate once a second and hands it here for the menu.
void SetFrameRate(double fps);

} // namespace GameMenu
