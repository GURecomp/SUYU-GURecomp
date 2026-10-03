// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>

#include "common/logging/log.h"
#include "common/string_util.h"
#include "suyu_cmd/sdl_software_keyboard.h"

namespace {

using Service::AM::Frontend::SwkbdReplyType;
using Service::AM::Frontend::SwkbdResult;

enum class Mode { None, Normal, Inline };

struct State {
    std::mutex lock;
    std::atomic<Mode> mode{Mode::None};
    bool is_inline{};
    std::u16string text;
    std::u16string header;
    u32 max_length{};
    bool cancel_allowed{true};
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback submit_normal;
    Core::Frontend::SoftwareKeyboardApplet::SubmitInlineCallback submit_inline;
};

State g;

// Callbacks run outside the lock: the applet may call back into this frontend.
void Changed() {
    std::u16string text;
    Core::Frontend::SoftwareKeyboardApplet::SubmitInlineCallback cb;
    {
        std::scoped_lock l{g.lock};
        if (g.mode != Mode::Inline || !g.submit_inline) {
            return;
        }
        text = g.text;
        cb = g.submit_inline;
    }
    cb(SwkbdReplyType::ChangedString, text, static_cast<s32>(text.size()));
}

void Finish(bool ok) {
    const Mode mode = g.mode.exchange(Mode::None);
    std::u16string text;
    Core::Frontend::SoftwareKeyboardApplet::SubmitNormalCallback normal;
    Core::Frontend::SoftwareKeyboardApplet::SubmitInlineCallback inl;
    {
        std::scoped_lock l{g.lock};
        text = g.text;
        normal = g.submit_normal;
        inl = g.submit_inline;
    }
    if (mode == Mode::Normal && normal) {
        normal(ok ? SwkbdResult::Ok : SwkbdResult::Cancel, text, true);
    } else if (mode == Mode::Inline && inl) {
        inl(ok ? SwkbdReplyType::DecidedEnter : SwkbdReplyType::DecidedCancel, text,
            static_cast<s32>(text.size()));
    }
    LOG_INFO(Frontend, "keyboard: {} \"{}\"", ok ? "entered" : "cancelled",
             Common::UTF16ToUTF8(text));
}

} // namespace

void SdlSoftwareKeyboard::Close() const {
    g.mode = Mode::None;
}

void SdlSoftwareKeyboard::InitializeKeyboard(
    bool is_inline, Core::Frontend::KeyboardInitializeParameters p,
    SubmitNormalCallback submit_normal_callback_, SubmitInlineCallback submit_inline_callback_) {
    std::scoped_lock l{g.lock};
    g.is_inline = is_inline;
    g.text = p.initial_text;
    g.header = !p.header_text.empty() ? p.header_text : p.guide_text;
    g.max_length = p.max_text_length;
    g.cancel_allowed = !p.disable_cancel_button;
    if (is_inline) {
        g.submit_inline = std::move(submit_inline_callback_);
    } else {
        g.submit_normal = std::move(submit_normal_callback_);
    }
}

void SdlSoftwareKeyboard::ShowNormalKeyboard() const {
    g.mode = Mode::Normal;
    LOG_INFO(Frontend, "keyboard: the game asks for text; type it, Enter = OK, Esc = cancel");
}

void SdlSoftwareKeyboard::ShowTextCheckDialog(
    Service::AM::Frontend::SwkbdTextCheckResult, std::u16string text_check_message) const {
    // The game rejected the text (e.g. a banned word): ask again.
    LOG_INFO(Frontend, "keyboard: the game rejected the text: {}",
             Common::UTF16ToUTF8(text_check_message));
    g.mode = Mode::Normal;
}

void SdlSoftwareKeyboard::ShowInlineKeyboard(
    Core::Frontend::InlineAppearParameters p) const {
    {
        std::scoped_lock l{g.lock};
        if (p.max_text_length != 0) {
            g.max_length = p.max_text_length;
        }
        g.cancel_allowed = !p.disable_cancel_button;
    }
    g.mode = Mode::Inline;
    LOG_INFO(Frontend, "keyboard: the game asks for text; type it, Enter = OK, Esc = cancel");
}

void SdlSoftwareKeyboard::HideInlineKeyboard() const {
    g.mode = Mode::None;
}

void SdlSoftwareKeyboard::InlineTextChanged(Core::Frontend::InlineTextParameters p) const {
    std::scoped_lock l{g.lock};
    g.text = p.input_text;
}

void SdlSoftwareKeyboard::ExitKeyboard() const {
    g.mode = Mode::None;
}

namespace SdlKeyboard {

bool Active() {
    return g.mode.load() != Mode::None;
}

bool HandleEvent(const SDL_Event& event) {
    if (!Active()) {
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_TEXT_INPUT: {
        const std::u16string add = Common::UTF8ToUTF16(event.text.text);
        {
            std::scoped_lock l{g.lock};
            for (const char16_t ch : add) {
                if (g.max_length == 0 || g.text.size() < g.max_length) {
                    g.text.push_back(ch);
                }
            }
        }
        Changed();
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
        switch (event.key.scancode) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if ((event.key.mod & SDL_KMOD_ALT) != 0) {
                return false; // Alt+Enter stays the fullscreen toggle
            }
            Finish(true);
            return true;
        case SDL_SCANCODE_ESCAPE:
            if (g.cancel_allowed) {
                Finish(false);
            }
            return true;
        case SDL_SCANCODE_BACKSPACE: {
            bool changed = false;
            {
                std::scoped_lock l{g.lock};
                if (!g.text.empty()) {
                    g.text.pop_back();
                    changed = true;
                }
            }
            if (changed) {
                Changed();
            }
            return true;
        }
        case SDL_SCANCODE_F11:
        case SDL_SCANCODE_F12:
            return false; // fullscreen and the F12 panel keep working
        default:
            return true; // typing keys don't reach the controller mapping
        }
    case SDL_EVENT_KEY_UP:
        return true;
    default:
        return false;
    }
}

std::string TitleText() {
    if (!Active()) {
        return {};
    }
    std::scoped_lock l{g.lock};
    std::string out = g.header.empty() ? std::string("Type") : Common::UTF16ToUTF8(g.header);
    out += ": " + Common::UTF16ToUTF8(g.text) + "_   (Enter = OK, Esc = cancel)";
    return out;
}

} // namespace SdlKeyboard
