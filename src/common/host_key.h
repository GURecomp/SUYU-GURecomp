// Polled host keyboard state for the debug / benchmark hotkeys.
// Windows: GetAsyncKeyState. Elsewhere: a poller registered by the window code (suyu-cmd: SDL scancodes).
#pragma once

#ifndef _WIN32
// Windows virtual-key codes used by the hotkey polls (windows.h defines these on Windows).
#define VK_CONTROL 0x11
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#endif

namespace Common {

using HostKeyPoller = bool (*)(int windows_vk);

/// Registers the non-Windows key poller (ignored on Windows).
void SetHostKeyPoller(HostKeyPoller poller);

/// True while the key with the given Windows virtual-key code is held down.
bool HostKeyDown(int windows_vk);

} // namespace Common
