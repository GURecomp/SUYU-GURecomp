// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/arm/recomp/game_settings.h"
#include "core/core.h"
#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"
#include "network/verify_user.h"
#include "suyu_cmd/multiplayer.h"

namespace Multiplayer {

namespace {

namespace GS = Core::GameSettings;

std::recursive_mutex g_mutex; // Join can report an error synchronously, under the lock
bool g_network_ready{false};
bool g_hosting{false};
std::string g_last_error;
Network::GameInfo g_game; // announced to the room once joined
Network::RoomMember::CallbackHandle<Network::RoomMember::State> g_state_handle;
Network::RoomMember::CallbackHandle<Network::RoomMember::Error> g_error_handle;

const char* ErrorText(Network::RoomMember::Error error) {
    using E = Network::RoomMember::Error;
    switch (error) {
    case E::LostConnection:
        return "lost the connection to the room";
    case E::HostKicked:
        return "kicked by the host";
    case E::CouldNotConnect:
        return "could not connect (check the address, port and that the host is up)";
    case E::NameCollision:
        return "that nickname is taken in the room (or isn't 4-20 letters, digits, space . _ -)";
    case E::IpCollision:
        return "address collision in the room";
    case E::WrongVersion:
        return "the room runs a different version";
    case E::WrongPassword:
        return "wrong password";
    case E::UnknownError:
        return "network error (missing permission or blocked by a firewall?)";
    case E::RoomIsFull:
        return "the room is full";
    case E::HostBanned:
        return "banned by the host";
    case E::PermissionDenied:
        return "permission denied";
    case E::NoSuchUser:
        return "no such user";
    default:
        return "unknown error";
    }
}

/// Network::Init once; binds state and error reporting (logged, never fatal).
bool EnsureNetwork() {
    if (g_network_ready) {
        return true;
    }
    if (!Network::Init()) {
        g_last_error = "network start-up failed";
        LOG_ERROR(Network, "multiplayer: Network::Init failed");
        return false;
    }
    g_network_ready = true;
    if (auto member = Network::GetRoomMember().lock()) {
        g_state_handle = member->BindOnStateChanged([](const Network::RoomMember::State& s) {
            LOG_INFO(Network, "multiplayer: {}", Network::GetStateStr(s));
            if (s == Network::RoomMember::State::Joined ||
                s == Network::RoomMember::State::Moderator) {
                if (auto m = Network::GetRoomMember().lock()) {
                    m->SendGameInfo(g_game);
                }
            }
        });
        g_error_handle = member->BindOnError([](const Network::RoomMember::Error& e) {
            std::scoped_lock lock{g_mutex};
            g_last_error = ErrorText(e);
            LOG_WARNING(Network, "multiplayer: {}", g_last_error);
        });
    }
    return true;
}

u32 ParseU32(const std::string& s, u32 fallback) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 10);
    return (s.empty() || end == s.c_str()) ? fallback : static_cast<u32>(v);
}

/// The room's own rule (Room::IsValidNickname); it reports a breach as a name collision.
bool ValidNickname(const std::string& n, std::string* error) {
    const bool ok = n.size() >= 4 && n.size() <= 20 &&
                    std::all_of(n.begin(), n.end(), [](char ch) {
                        return std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' ||
                               ch == '.' || ch == '_' || ch == '-';
                    });
    if (!ok) {
        *error = "the nickname must be 4-20 characters: letters, digits, space, . _ -";
    }
    return ok;
}

} // namespace

void Init() {
    std::scoped_lock lock{g_mutex};
    EnsureNetwork();
}

Config Load() {
    Config c;
    const auto get = [](const char* key, const std::string& fallback) {
        const std::string v = GS::Value("Multiplayer", key);
        return v.empty() ? fallback : v;
    };
    c.mode = get("mode", c.mode);
    c.nickname = get("nickname", c.nickname);
    c.address = GS::Value("Multiplayer", "address");
    c.port = static_cast<u16>(std::clamp<u32>(ParseU32(get("port", ""), c.port), 1, 65535));
    c.password = GS::Value("Multiplayer", "password");
    c.room_name = get("room_name", c.room_name);
    c.max_players = std::clamp<u32>(ParseU32(get("max_players", ""), c.max_players), 2, 16);
    return c;
}

void Save(const Config& c) {
    GS::SetValue("Multiplayer", "mode", c.mode);
    GS::SetValue("Multiplayer", "nickname", c.nickname);
    GS::SetValue("Multiplayer", "address", c.address);
    GS::SetValue("Multiplayer", "port", std::to_string(c.port));
    GS::SetValue("Multiplayer", "password", c.password);
    GS::SetValue("Multiplayer", "room_name", c.room_name);
    GS::SetValue("Multiplayer", "max_players", std::to_string(c.max_players));
}

bool Host(Core::System& system, const Config& c, std::string* error) {
    std::scoped_lock lock{g_mutex};
    if (!ValidNickname(c.nickname, error)) {
        return false;
    }
    if (!EnsureNetwork()) {
        *error = g_last_error;
        return false;
    }
    auto room = Network::GetRoom().lock();
    auto member = Network::GetRoomMember().lock();
    if (!room || !member) {
        *error = "network objects unavailable";
        return false;
    }
    if (member->IsConnected()) {
        member->Leave();
    }
    if (room->GetState() == Network::Room::State::Open) {
        room->Destroy();
    }
    Network::GameInfo game;
    [[maybe_unused]] auto _ = system.GetGameName(game.name);
    game.id = system.GetApplicationProcessProgramID();
    g_game = game;
    if (!room->Create(c.room_name, "", "", c.port, c.password, c.max_players, c.nickname, game,
                      std::make_unique<Network::VerifyUser::NullBackend>(), {})) {
        *error = fmt::format("could not open a room on port {} (already in use?)", c.port);
        LOG_ERROR(Network, "multiplayer: {}", *error);
        return false;
    }
    g_hosting = true;
    g_last_error.clear();
    LOG_INFO(Network, "multiplayer: hosting '{}' on port {} (up to {} players)", c.room_name,
             c.port, c.max_players);
    member->Join(c.nickname, "127.0.0.1", c.port, 0, Network::NoPreferredIP, c.password);
    return true;
}

bool Join(const Config& c, std::string* error) {
    std::scoped_lock lock{g_mutex};
    if (!ValidNickname(c.nickname, error)) {
        return false;
    }
    if (c.address.empty()) {
        *error = "enter the host's address first";
        return false;
    }
    if (!EnsureNetwork()) {
        *error = g_last_error;
        return false;
    }
    auto member = Network::GetRoomMember().lock();
    if (!member) {
        *error = "network objects unavailable";
        return false;
    }
    if (member->IsConnected()) {
        member->Leave();
    }
    g_last_error.clear();
    LOG_INFO(Network, "multiplayer: joining {}:{} as {}", c.address, c.port, c.nickname);
    member->Join(c.nickname, c.address.c_str(), c.port, 0, Network::NoPreferredIP, c.password);
    return true;
}

void Leave() {
    std::scoped_lock lock{g_mutex};
    if (!g_network_ready) {
        return;
    }
    if (auto member = Network::GetRoomMember().lock(); member && member->IsConnected()) {
        member->Leave();
    }
    if (auto room = Network::GetRoom().lock();
        g_hosting && room && room->GetState() == Network::Room::State::Open) {
        room->Destroy();
    }
    g_hosting = false;
    LOG_INFO(Network, "multiplayer: left the room");
}

std::string StatusText() {
    std::scoped_lock lock{g_mutex};
    if (!g_network_ready) {
        return "Offline";
    }
    auto member = Network::GetRoomMember().lock();
    if (!member) {
        return "Offline";
    }
    const auto state = member->GetState();
    std::string out;
    if (member->IsConnected()) {
        const auto info = member->GetRoomInformation();
        const auto& members = member->GetMemberInformation();
        out = fmt::format("{} '{}' ({}/{}) as {}", g_hosting ? "Hosting" : "Joined", info.name,
                          members.size(), info.member_slots, member->GetNickname());
        for (const auto& m : members) {
            out += "\r\n  - " + m.nickname;
            if (!m.game_info.name.empty()) {
                out += "  (" + m.game_info.name + ")";
            }
        }
    } else {
        out = Network::GetStateStr(state);
    }
    if (!g_last_error.empty()) {
        out += "\r\n  Last error: " + g_last_error;
    }
    return out;
}

void SetGame(Core::System& system) {
    std::scoped_lock lock{g_mutex};
    [[maybe_unused]] auto _ = system.GetGameName(g_game.name);
    g_game.id = system.GetApplicationProcessProgramID();
}

void AutoStart(Core::System& system) {
    SetGame(system);
    const Config c = Load();
    std::string error;
    if (c.mode == "host") {
        if (!Host(system, c, &error)) {
            LOG_ERROR(Network, "multiplayer: hosting at start failed: {}", error);
        }
    } else if (c.mode == "join") {
        if (!Join(c, &error)) {
            LOG_ERROR(Network, "multiplayer: joining at start failed: {}", error);
        }
    }
}

} // namespace Multiplayer
