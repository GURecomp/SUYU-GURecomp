// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <SDL3/SDL_events.h>

#include "core/frontend/applets/software_keyboard.h"

/**
 * The game's software keyboard (chat, names...) typed on the PC keyboard: while the game asks
 * for text, typed characters go to it instead of the controller mapping. Enter submits,
 * Escape cancels, Backspace deletes; the window title shows the text being typed. Replaces
 * the default frontend, which answered every request with the word "suyu".
 */
class SdlSoftwareKeyboard final : public Core::Frontend::SoftwareKeyboardApplet {
public:
    void Close() const override;
    void InitializeKeyboard(bool is_inline,
                            Core::Frontend::KeyboardInitializeParameters initialize_parameters,
                            SubmitNormalCallback submit_normal_callback_,
                            SubmitInlineCallback submit_inline_callback_) override;
    void ShowNormalKeyboard() const override;
    void ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
                             std::u16string text_check_message) const override;
    void ShowInlineKeyboard(
        Core::Frontend::InlineAppearParameters appear_parameters) const override;
    void HideInlineKeyboard() const override;
    void InlineTextChanged(Core::Frontend::InlineTextParameters text_parameters) const override;
    void ExitKeyboard() const override;
};

namespace SdlKeyboard {

/// True while the game is waiting for typed text (the window then turns on SDL text input).
bool Active();

/// Called by the window for every SDL event; true = it was typing input, don't pass it on.
bool HandleEvent(const SDL_Event& event);

/// What the window title should show while typing ("" when not typing).
std::string TitleText();

} // namespace SdlKeyboard
