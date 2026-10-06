// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

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

/// What the game menu shows.
struct Status {
    enum class Phase { Offline, Connecting, Hosting, Joined, Reconnecting };
    Phase phase{Phase::Offline};
    std::string room;
    std::string nickname;
    std::vector<std::string> members; // "Name (Game)"
    u32 slots{};
    std::string last_error;
    int reconnect_attempt{}; // while Reconnecting
};
Status GetStatus();

/// Short form for the window title: "" offline, "Online 2/4", "Reconnecting...", or a recent
/// event ("Mira joined") for a few seconds.
std::string ShortStatus();
/// Recent room events, newest last ("12:04 Mira joined").
std::vector<std::string> Events();

/// One of this PC's addresses friends can join, labelled (Radmin VPN, Hamachi, LAN, ...).
struct LocalAddress {
    std::string ip;
    std::string label;
};
/// VPN addresses first (Radmin 26.x, Hamachi 25.x, ZeroTier/Tailscale), then LAN.
std::vector<LocalAddress> LocalAddresses();
/// "26.1.2.3:24872", plus "#password" when the room has one.
std::string InviteCode(const std::string& ip, u16 port, const std::string& password);
/// Reads "address", "address:port" or "address:port#password" into c (port and password only
/// when given). false when no address is in it.
bool ParseInvite(const std::string& text, Config* c);

/// Hosts joined before, newest first, as "address:port" (game_settings.ini [Multiplayer] recent).
std::vector<std::string> RecentHosts();
void ForgetHost(const std::string& host);

bool AutoReconnect();
void SetAutoReconnect(bool on);

/// Called by the main loop a few times a second: reconnects after a dropped connection and
/// notes members joining/leaving.
void Tick();

} // namespace Multiplayer
