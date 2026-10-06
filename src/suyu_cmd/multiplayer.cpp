// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/arm/recomp/game_settings.h"
#include "core/core.h"
#include "core/internal_network/network_interface.h"
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

using Clock = std::chrono::steady_clock;
constexpr int kReconnectAttempts = 12; // one minute of tries
constexpr auto kReconnectDelay = std::chrono::seconds(5);
constexpr auto kTitleEventTime = std::chrono::seconds(6);
std::optional<Config> g_target; // the room we joined (not hosted): what a reconnect rejoins
bool g_reconnecting{false};
int g_attempt{};
Clock::time_point g_next_attempt{};
std::deque<std::string> g_events;
std::string g_title_event;
Clock::time_point g_title_event_until{};
std::vector<std::string> g_known_members;

void AddEvent(const std::string& text) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[16]{};
    std::strftime(stamp, sizeof(stamp), "%H:%M", &local);
    g_events.push_back(std::string(stamp) + "  " + text);
    while (g_events.size() > 30) {
        g_events.pop_front();
    }
    g_title_event = text;
    g_title_event_until = Clock::now() + kTitleEventTime;
    LOG_INFO(Network, "multiplayer: {}", text);
}

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
                std::scoped_lock lock{g_mutex};
                if (auto m = Network::GetRoomMember().lock()) {
                    m->SendGameInfo(g_game);
                }
                if (g_reconnecting) {
                    AddEvent("Reconnected");
                }
                g_reconnecting = false;
                g_attempt = 0;
            }
        });
        g_error_handle = member->BindOnError([](const Network::RoomMember::Error& e) {
            std::scoped_lock lock{g_mutex};
            g_last_error = ErrorText(e);
            LOG_WARNING(Network, "multiplayer: {}", g_last_error);
            // A dropped connection (or a failed retry) to a room we joined is retried; a
            // refusal (password, name, kicked, full...) isn't.
            using E = Network::RoomMember::Error;
            const bool retry =
                e == E::LostConnection || (g_reconnecting && e == E::CouldNotConnect);
            if (retry && g_target && AutoReconnect() && g_attempt < kReconnectAttempts) {
                if (!g_reconnecting) {
                    AddEvent("Connection lost - reconnecting");
                    g_attempt = 0;
                }
                g_reconnecting = true;
                g_next_attempt = Clock::now() + kReconnectDelay;
            } else {
                if (e == E::LostConnection) {
                    AddEvent("Connection lost");
                }
                g_reconnecting = false;
            }
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

void RememberHost(const std::string& address, u16 port) {
    const std::string host = fmt::format("{}:{}", address, port);
    std::vector<std::string> hosts = RecentHosts();
    hosts.erase(std::remove(hosts.begin(), hosts.end(), host), hosts.end());
    hosts.insert(hosts.begin(), host);
    if (hosts.size() > 6) {
        hosts.resize(6);
    }
    std::string joined;
    for (const auto& h : hosts) {
        joined += (joined.empty() ? "" : ", ") + h;
    }
    GS::SetValue("Multiplayer", "recent", joined);
}

std::string Trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
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
    g_target.reset();
    g_reconnecting = false;
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
    g_target = c;
    g_reconnecting = false;
    g_attempt = 0;
    g_hosting = false;
    RememberHost(c.address, c.port);
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
    g_target.reset();
    g_reconnecting = false;
    g_known_members.clear();
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

Status GetStatus() {
    std::scoped_lock lock{g_mutex};
    Status st;
    st.last_error = g_last_error;
    st.reconnect_attempt = g_attempt;
    auto member = g_network_ready ? Network::GetRoomMember().lock() : nullptr;
    if (member && member->IsConnected()) {
        const auto info = member->GetRoomInformation();
        st.phase = g_hosting ? Status::Phase::Hosting : Status::Phase::Joined;
        st.room = info.name;
        st.slots = info.member_slots;
        st.nickname = member->GetNickname();
        for (const auto& m : member->GetMemberInformation()) {
            st.members.push_back(m.game_info.name.empty()
                                     ? m.nickname
                                     : fmt::format("{}  ({})", m.nickname, m.game_info.name));
        }
    } else if (g_reconnecting) {
        st.phase = Status::Phase::Reconnecting;
    } else if (member && member->GetState() == Network::RoomMember::State::Joining) {
        st.phase = Status::Phase::Connecting;
    }
    return st;
}

std::string ShortStatus() {
    std::scoped_lock lock{g_mutex};
    if (!g_title_event.empty() && Clock::now() < g_title_event_until) {
        return g_title_event;
    }
    if (g_reconnecting) {
        return "Reconnecting...";
    }
    auto member = g_network_ready ? Network::GetRoomMember().lock() : nullptr;
    if (!member || !member->IsConnected()) {
        return {};
    }
    return fmt::format("Online {}/{}", member->GetMemberInformation().size(),
                       member->GetRoomInformation().member_slots);
}

std::vector<std::string> Events() {
    std::scoped_lock lock{g_mutex};
    return {g_events.begin(), g_events.end()};
}

std::vector<LocalAddress> LocalAddresses() {
    std::vector<LocalAddress> vpn, lan, other;
    for (const auto& iface : Network::GetAvailableNetworkInterfaces()) {
        const auto* b = reinterpret_cast<const u8*>(&iface.ip_address);
        const std::string ip = fmt::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
        std::string name_lower = iface.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (b[0] == 127 || b[0] == 0 || (b[0] == 169 && b[1] == 254)) {
            continue;
        }
        if (b[0] == 26 || name_lower.find("radmin") != std::string::npos) {
            vpn.push_back({ip, "Radmin VPN"});
        } else if (b[0] == 25 || name_lower.find("hamachi") != std::string::npos) {
            vpn.push_back({ip, "Hamachi"});
        } else if (name_lower.find("zerotier") != std::string::npos) {
            vpn.push_back({ip, "ZeroTier"});
        } else if (name_lower.find("tailscale") != std::string::npos ||
                   (b[0] == 100 && b[1] >= 64 && b[1] < 128)) {
            vpn.push_back({ip, "Tailscale"});
        } else if (b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] < 32) ||
                   (b[0] == 192 && b[1] == 168)) {
            lan.push_back({ip, "Home network (" + iface.name + ")"});
        } else {
            other.push_back({ip, iface.name});
        }
    }
    vpn.insert(vpn.end(), lan.begin(), lan.end());
    vpn.insert(vpn.end(), other.begin(), other.end());
    return vpn;
}

std::string InviteCode(const std::string& ip, u16 port, const std::string& password) {
    return password.empty() ? fmt::format("{}:{}", ip, port)
                            : fmt::format("{}:{}#{}", ip, port, password);
}

bool ParseInvite(const std::string& text, Config* c) {
    std::string t = Trimmed(text);
    std::string password = c->password;
    if (const auto hash = t.find('#'); hash != std::string::npos) {
        password = t.substr(hash + 1);
        t = Trimmed(t.substr(0, hash));
    }
    u16 port = c->port;
    if (const auto colon = t.rfind(':'); colon != std::string::npos) {
        const u32 p = ParseU32(t.substr(colon + 1), 0);
        if (p < 1 || p > 65535) {
            return false;
        }
        port = static_cast<u16>(p);
        t = Trimmed(t.substr(0, colon));
    }
    if (t.empty() || t.find(' ') != std::string::npos) {
        return false;
    }
    c->address = t;
    c->port = port;
    c->password = password;
    return true;
}

std::vector<std::string> RecentHosts() {
    std::vector<std::string> out;
    const std::string all = GS::Value("Multiplayer", "recent");
    std::size_t start = 0;
    while (start <= all.size()) {
        const auto comma = all.find(',', start);
        const std::string h = Trimmed(
            all.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!h.empty() && std::find(out.begin(), out.end(), h) == out.end()) {
            out.push_back(h);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

void ForgetHost(const std::string& host) {
    std::vector<std::string> hosts = RecentHosts();
    hosts.erase(std::remove(hosts.begin(), hosts.end(), host), hosts.end());
    std::string joined;
    for (const auto& h : hosts) {
        joined += (joined.empty() ? "" : ", ") + h;
    }
    GS::SetValue("Multiplayer", "recent", joined);
}

bool AutoReconnect() {
    return GS::Value("Multiplayer", "auto_reconnect") != "false";
}

void SetAutoReconnect(bool on) {
    GS::SetValue("Multiplayer", "auto_reconnect", on ? "true" : "false");
}

void Tick() {
    std::scoped_lock lock{g_mutex};
    if (!g_network_ready) {
        return;
    }
    auto member = Network::GetRoomMember().lock();
    if (!member) {
        return;
    }
    if (g_reconnecting && g_target && Clock::now() >= g_next_attempt) {
        if (g_attempt >= kReconnectAttempts) {
            g_reconnecting = false;
            g_last_error = fmt::format("gave up reconnecting after {} tries", g_attempt);
            AddEvent("Couldn't reconnect - join again from the menu");
        } else if (!member->IsConnected() &&
                   member->GetState() != Network::RoomMember::State::Joining) {
            ++g_attempt;
            g_next_attempt = Clock::now() + kReconnectDelay;
            LOG_INFO(Network, "multiplayer: reconnect attempt {} to {}:{}", g_attempt,
                     g_target->address, g_target->port);
            const Config target = *g_target; // Join's callbacks may touch g_target
            member->Join(target.nickname, target.address.c_str(), target.port, 0,
                         Network::NoPreferredIP, target.password);
        }
    }
    // Members coming and going (only while connected; a reconnect starts the list afresh).
    std::vector<std::string> now_members;
    if (member->IsConnected()) {
        for (const auto& m : member->GetMemberInformation()) {
            now_members.push_back(m.nickname);
        }
        if (!g_known_members.empty()) {
            for (const auto& n : now_members) {
                if (n != member->GetNickname() &&
                    std::find(g_known_members.begin(), g_known_members.end(), n) ==
                        g_known_members.end()) {
                    AddEvent(n + " joined");
                }
            }
            for (const auto& n : g_known_members) {
                if (std::find(now_members.begin(), now_members.end(), n) == now_members.end()) {
                    AddEvent(n + " left");
                }
            }
        }
    }
    g_known_members = std::move(now_members);
}

} // namespace Multiplayer
