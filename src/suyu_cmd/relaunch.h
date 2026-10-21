// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

/// Restart of the game from its own menu (F10 > Settings > Restart now): a new process with the same
/// command line, which waits for this one to finish its normal shutdown before it starts (saves, config,
/// logs and caches are never open twice).
namespace Relaunch {

/// main(): remembers argv (Linux; Windows uses its own command line) and, when this process was started by
/// a restart, waits until the previous instance has exited (at most 60 s). Call before anything else.
void OnStart(int argc, char** argv);

/// Starts the new process. false = it could not be started (*error says why); this process keeps running.
bool StartNewInstance(std::string* error, unsigned long* pid);

} // namespace Relaunch
