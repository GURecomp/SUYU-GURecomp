// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Common::Log {

/// Path / name redaction for logs and reports, so they can be shared without personal data.
/// Applied at the sinks (suyu's log, the replacement renderer's report writers).
///
///  * the user-profile directory      -> %USERPROFILE%   (Linux/macOS $HOME, /home/<n>, /Users/<n> -> ~)
///  * registered roots                -> their placeholder (<game> install folder, <user> data folder)
///  * any other absolute path         -> <path>/<file name>
///  * the Windows user name and the computer name, as bare words -> <username> / <computername>
///  * IPv4 addresses -> <ip-N lan|vpn|public> (N = order of first appearance, same IP = same tag,
///    :port stays; 127.x and 0.0.0.0 unchanged). IPv6 and nicknames are not touched.
/// Relative parts after a placeholder stay, so the structure is still readable.
/// tools/scrub-log.ps1 implements the same rules for existing files; keep the two in step.

/// [Debug] log_redact_paths (default true). Only the developer turns this off for own debugging.
void SetRedactEnabled(bool enabled);
bool RedactEnabled();

/// Registers a folder to be shown as `placeholder` (longest registered folder wins).
/// Case- and slash-insensitive on Windows. Empty paths are ignored.
void AddRedactRoot(std::string_view path, std::string_view placeholder);

/// Returns `text` redacted (a copy of `text` when redaction is off).
std::string RedactText(std::string_view text);

} // namespace Common::Log
