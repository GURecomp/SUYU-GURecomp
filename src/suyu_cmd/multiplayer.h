// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
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

/// Ryujinx's public RyuLdn server: the main server when [Multiplayer] ryuldn_host is empty.
inline constexpr const char* DefaultRyuLdnHost = "ldn.ryujinx.app";
/// [Multiplayer] ryuldn_host = self: this game runs the server for friends (Radmin VPN / LAN).
inline constexpr const char* SelfHostKeyword = "self";

/// Where Ryujinx LDN lobbies live (the main server).
struct RyuLdnServerChoice {
    enum class Mode { Public, Host, Friend };
    Mode mode{Mode::Public};
    std::string address; // Friend: their address ("26.1.2.3" or "26.1.2.3:30456")
};
RyuLdnServerChoice CurrentRyuLdnServerChoice();
/// Saves the choice and applies it at once (starts/stops this game's own server; the running
/// game switches servers). false + *error while in a lobby or when the port can't be opened.
bool ChooseRyuLdnServer(const RyuLdnServerChoice& choice, std::string* error);

/// This game's own RyuLdn server (Host mode).
struct OwnServerStatus {
    bool running{false};
    u16 port{};
    std::size_t connections{}; // players connected to it (including you)
    std::size_t lobbies{};
    std::size_t players{};
    std::string error; // why it couldn't start
};
OwnServerStatus GetOwnServerStatus();

/// game_settings.ini [Multiplayer] backend: room (default) or ryuldn. With ryuldn, local wireless
/// play goes through RyuLdn master servers (Ryujinx's lobby servers: ryuldn_host, default
/// ldn.ryujinx.app, plus ryuldn_servers) for the whole run; room hosting/joining is then off.
/// Call once, before the game is loaded. False (with a log line) when the setting is unknown.
bool SetupLdnBackend();
/// ryuldn was chosen at start.
bool UsingRyuLdn();
/// What game_settings.ini says now (the menu's switch; takes effect at the next start).
bool RyuLdnChosenInSettings();
void ChooseBackend(bool ryuldn);
/// One line about the RyuLdn connection for the menu ("" when the backend is room); the
/// server's address isn't in it (RyuLdnServer, shown on request).
std::string RyuLdnStatusText();
/// The servers of this run, "host:port, host:port".
std::string RyuLdnServer();

/// [Multiplayer] ryuldn_servers: extra servers ("host" or "host:port") whose lobbies are listed
/// and joinable too. Changes reach the game at the next start; the Public rooms list uses them
/// at once.
std::vector<std::string> ExtraRyuLdnServers();
/// The main server as configured now: "host" (default port) or "host:port".
std::string MainRyuLdnServer();
bool AddExtraRyuLdnServer(const std::string& text, std::string* error);
void RemoveExtraRyuLdnServer(const std::string& entry);

/// A lobby of this game on a RyuLdn server.
struct PublicRoom {
    std::string server; // "Ryujinx" for the public server, else "Server N"
    std::string host;   // the host's name
    std::vector<std::string> names;
    int players{};
    int max_players{};
};
struct PublicRooms {
    std::vector<PublicRoom> rooms;
    std::vector<std::string> errors; // servers that didn't answer
    bool refreshing{false};
    bool refreshed{false}; // at least one refresh finished
    bool private_passphrase{false};
    std::chrono::steady_clock::time_point last_refresh;
};
/// Asks every configured server for this game's lobbies in the background (works with either
/// backend; its own short connections). No-op while a refresh runs.
void RefreshPublicRooms();
PublicRooms GetPublicRooms();

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
