// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/common_types.h"

namespace Core {
class System;
}

/**
 * Local multiplayer for the exported game: host or join a suyu room (for example over Radmin VPN)
 * without the Qt frontend. Settings live in game_settings.ini [Multiplayer]; the F12 panel offers
 * the same actions while playing. A failed join is reported, never fatal.
 */
namespace Multiplayer {

struct Config {
    std::string mode{"off"}; // off | host | join
    std::string nickname{"Hunter"};
    std::string address;
    u16 port{24872};
    std::string password;
    std::string room_name{"MHGU"};
    u32 max_players{4};
};

/// Starts the network library (before the game loads, as the Qt frontend does).
void Init();
/// Records the running game, announced to the room once joined.
void SetGame(Core::System& system);

Config Load();
void Save(const Config& config);

/// Starts the room inside this process and joins it. false + *error when the room can't open.
bool Host(Core::System& system, const Config& config, std::string* error);
/// Starts joining config.address:config.port (the result arrives asynchronously).
bool Join(const Config& config, std::string* error);
/// Leaves the room and closes it if this process hosts it.
void Leave();

/// One-line state ("Joined MHGU (2/4) as Hunter") plus the member list, for the panel.
std::string StatusText();

/// At start: does what game_settings.ini [Multiplayer] mode asks for.
void AutoStart(Core::System& system);

} // namespace Multiplayer
