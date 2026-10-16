// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/logging/log.h"
#include "core/arm/recomp/game_settings.h"
#include "core/core.h"
#include "core/hle/service/ldn/ldn_types.h"
#include "core/internal_network/network_interface.h"
#include "network/ldn_backend.h"
#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"
#include "network/ryuldn/backend.h"
#include "network/ryuldn/room_bridge.h"
#include "network/ryuldn/server.h"
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

namespace {
LdnMode g_mode{LdnMode::Room};
std::shared_ptr<Network::HybridLdnBackend> g_hybrid;
std::unique_ptr<Network::RyuLdn::RoomBridge> g_bridge; // auto mode only
std::string g_ryuldn_problem; // why ryuldn isn't usable this run ("" = fine)

Network::LdnMode ToNetworkMode(LdnMode mode) {
    switch (mode) {
    case LdnMode::RyuLdn:
        return Network::LdnMode::RyuLdn;
    case LdnMode::Auto:
        return Network::LdnMode::Auto;
    default:
        return Network::LdnMode::Room;
    }
}

const char* ModeText(LdnMode mode) {
    switch (mode) {
    case LdnMode::RyuLdn:
        return "ryuldn";
    case LdnMode::Auto:
        return "auto";
    default:
        return "room";
    }
}
} // namespace

namespace {

bool ApplyServersLive(std::string* error);

/// "host" or "host:port"; nullopt when empty or the port isn't a number.
std::optional<std::pair<std::string, u16>> ParseServer(const std::string& text) {
    std::string host = Trimmed(text);
    u16 port = Network::RyuLdn::DefaultPort;
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        const u32 p = ParseU32(host.substr(colon + 1), 0);
        if (p == 0 || p > 65535) {
            return std::nullopt;
        }
        port = static_cast<u16>(p);
        host = Trimmed(host.substr(0, colon));
    }
    if (host.empty()) {
        return std::nullopt;
    }
    return std::pair{host, port};
}

std::string g_friend_server; // the host we joined a room of: its server too (until we leave)

/// Where lobbies are hosted and found, in order: [0] the main server (lobbies this game hosts live
/// there): this PC's own server unless ryuldn_host names another; then the server of the friend
/// whose room we joined; Ryujinx's public server (ryuldn_public = off leaves it out); the extras.
std::vector<Network::RyuLdn::Client::Settings> ConfiguredRyuLdnServers() {
    std::vector<Network::RyuLdn::Client::Settings> out;
    const std::string passphrase = Trimmed(GS::Value("Multiplayer", "ryuldn_passphrase"));
    const auto add = [&](const std::string& host, u16 port) {
        for (const auto& s : out) {
            if (s.host == host && s.port == port) {
                return;
            }
        }
        Network::RyuLdn::Client::Settings s;
        s.host = host;
        s.port = port;
        s.passphrase = passphrase;
        out.push_back(std::move(s));
    };
    const u16 port = static_cast<u16>(
        ParseU32(GS::Value("Multiplayer", "ryuldn_port"), Network::RyuLdn::DefaultPort));
    const std::string main_host = Trimmed(GS::Value("Multiplayer", "ryuldn_host"));
    if (main_host == DefaultRyuLdnHost) {
        add(DefaultRyuLdnHost, port); // hosted lobbies go to Ryujinx's public server
    } else if (main_host.empty() || main_host == SelfHostKeyword) {
        add("127.0.0.1", port); // the server this game runs while it hosts (OwnServerIsMain)
    } else if (const auto server = ParseServer(main_host)) {
        // This PC's own address (e.g. the Radmin one) names the server it runs itself.
        bool own = server->first == "localhost" || server->first == "127.0.0.1";
        for (const auto& a : LocalAddresses()) {
            own |= a.ip == server->first;
        }
        if (own) {
            add("127.0.0.1", port);
        } else {
            add(server->first, main_host.find(':') == std::string::npos ? port : server->second);
        }
    } else {
        add("127.0.0.1", port);
    }
    if (!g_friend_server.empty()) {
        if (const auto server = ParseServer(g_friend_server)) {
            add(server->first, g_friend_server.find(':') == std::string::npos ? port
                                                                               : server->second);
        }
    }
    if (Trimmed(GS::Value("Multiplayer", "ryuldn_public")) != "off") {
        add(DefaultRyuLdnHost, port);
    }
    for (const auto& entry : ExtraRyuLdnServers()) {
        if (const auto server = ParseServer(entry)) {
            add(server->first, server->second);
        }
    }
    return out;
}

/// Lobbies this game hosts go to a server it runs itself.
bool OwnServerIsMain() {
    return ConfiguredRyuLdnServers().front().host == "127.0.0.1";
}

} // namespace

std::vector<std::string> ExtraRyuLdnServers() {
    std::vector<std::string> out;
    const std::string list = GS::Value("Multiplayer", "ryuldn_servers");
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t comma = std::min(list.find(',', start), list.size());
        if (const std::string entry = Trimmed(list.substr(start, comma - start)); !entry.empty()) {
            out.push_back(entry);
        }
        start = comma + 1;
    }
    return out;
}

std::string MainRyuLdnServer() {
    const auto main = ConfiguredRyuLdnServers().front();
    return main.port == Network::RyuLdn::DefaultPort ? main.host
                                                     : fmt::format("{}:{}", main.host, main.port);
}

bool AddExtraRyuLdnServer(const std::string& text, std::string* error) {
    const auto server = ParseServer(text);
    if (!server) {
        *error = "not a server address (host or host:port)";
        return false;
    }
    auto list = ExtraRyuLdnServers();
    const std::string entry = server->second == Network::RyuLdn::DefaultPort
                                  ? server->first
                                  : fmt::format("{}:{}", server->first, server->second);
    if (std::find(list.begin(), list.end(), entry) != list.end()) {
        *error = "already in the list";
        return false;
    }
    list.push_back(entry);
    GS::SetValue("Multiplayer", "ryuldn_servers", fmt::format("{}", fmt::join(list, ",")));
    std::string live_error;
    if (!ApplyServersLive(&live_error)) {
        *error = "saved; the game uses it once you leave the lobby (" + live_error + ")";
    }
    return true;
}

void RemoveExtraRyuLdnServer(const std::string& entry) {
    auto list = ExtraRyuLdnServers();
    std::erase(list, entry);
    GS::SetValue("Multiplayer", "ryuldn_servers", fmt::format("{}", fmt::join(list, ",")));
    std::string ignored;
    ApplyServersLive(&ignored);
}

namespace {

std::mutex g_own_server_lock;
std::unique_ptr<Network::RyuLdn::Server> g_own_server;
std::string g_own_server_error;

/// Starts the in-game RyuLdn server on [Multiplayer] ryuldn_port (all adapters) if needed.
bool StartOwnServer(std::string* error) {
    std::scoped_lock lk{g_own_server_lock};
    if (g_own_server && g_own_server->GetStatus().running) {
        return true;
    }
    if (!g_own_server) {
        g_own_server = std::make_unique<Network::RyuLdn::Server>();
    }
    const u16 port = static_cast<u16>(
        ParseU32(GS::Value("Multiplayer", "ryuldn_port"), Network::RyuLdn::DefaultPort));
    std::string why;
    if (!g_own_server->Start("", port, why)) {
        g_own_server_error = why;
        *error = why;
        return false;
    }
    g_own_server_error.clear();
    return true;
}

void StopOwnServer() {
    std::scoped_lock lk{g_own_server_lock};
    if (g_own_server) {
        g_own_server->Stop();
    }
    g_own_server_error.clear();
}

/// Gives the running backend the servers game_settings.ini names now.
bool ApplyServersLive(std::string* error) {
    const auto ryu = Network::RyuLdn::GetActiveBackend();
    if (!ryu) {
        return true; // room backend: takes effect when switching to Ryujinx LDN
    }
    std::string why;
    if (!ryu->SetServers(ConfiguredRyuLdnServers(), why)) {
        *error = why;
        return false;
    }
    return true;
}

} // namespace

RyuLdnServerChoice CurrentRyuLdnServerChoice() {
    const std::string host = Trimmed(GS::Value("Multiplayer", "ryuldn_host"));
    if (host == DefaultRyuLdnHost) {
        return {RyuLdnServerChoice::Mode::Public, {}};
    }
    if (host.empty() || host == SelfHostKeyword) {
        return {RyuLdnServerChoice::Mode::Host, {}};
    }
    return {RyuLdnServerChoice::Mode::Friend, host};
}

bool ChooseRyuLdnServer(const RyuLdnServerChoice& choice, std::string* error) {
    using Mode = RyuLdnServerChoice::Mode;
    if (choice.mode == Mode::Friend && !ParseServer(choice.address)) {
        *error = "enter your friend's address (e.g. 26.12.34.56)";
        return false;
    }
    const auto ryu = Network::RyuLdn::GetActiveBackend();
    if (ryu && ryu->client().InNetwork()) {
        *error = "leave the lobby first";
        return false;
    }
    const std::string old = GS::Value("Multiplayer", "ryuldn_host");
    if (choice.mode == Mode::Host) {
        if (!StartOwnServer(error)) {
            return false;
        }
    } else {
        StopOwnServer();
    }
    GS::SetValue("Multiplayer", "ryuldn_host",
                 choice.mode == Mode::Public ? std::string{DefaultRyuLdnHost}
                 : choice.mode == Mode::Host ? std::string{SelfHostKeyword}
                                             : Trimmed(choice.address));
    if (!ApplyServersLive(error)) {
        GS::SetValue("Multiplayer", "ryuldn_host", old);
        return false;
    }
    return true;
}

OwnServerStatus GetOwnServerStatus() {
    std::scoped_lock lk{g_own_server_lock};
    OwnServerStatus out;
    if (g_own_server) {
        const auto st = g_own_server->GetStatus();
        out.running = st.running;
        out.port = st.port;
        out.connections = st.connections;
        out.lobbies = st.lobbies;
        out.players = st.players;
    }
    out.error = g_own_server_error;
    return out;
}

bool HostLocked(Core::System* system, const Config& c, std::string* error);
void LeaveLocked();

namespace {

bool g_auto_hosted{false}; // the room is open because the game hosts a lobby
bool g_auto_server{false}; // so is this PC's RyuLdn server

/// The game starts (true) or stops (false) hosting a lobby: hosting means a room for suyu players
/// and a server for Ryujinx players, both of this PC, and the bridge links them. Nothing to press.
void OnGameHosting(bool start) {
    std::scoped_lock lock{g_mutex};
    if (start) {
        std::string error;
        if (g_mode != LdnMode::Room && OwnServerIsMain() && !g_auto_server) {
            if (StartOwnServer(&error)) {
                g_auto_server = true;
            } else {
                LOG_ERROR(Network, "multiplayer: opening the RyuLdn server failed: {}", error);
            }
        }
        const auto member = EnsureNetwork() ? Network::GetRoomMember().lock() : nullptr;
        if (g_mode != LdnMode::RyuLdn && member && !member->IsConnected() && !g_hosting) {
            if (HostLocked(nullptr, Load(), &error)) {
                g_auto_hosted = true;
            } else {
                LOG_ERROR(Network, "multiplayer: opening the room failed: {}", error);
            }
        }
    } else {
        if (g_auto_hosted) {
            LeaveLocked();
            g_auto_hosted = false;
        }
        if (g_auto_server) {
            StopOwnServer();
            g_auto_server = false;
        }
    }
}

/// Applies mode to the running game (lock held): the sockets, the next LDN session and the
/// room bridge follow at once; a lobby that is open keeps working until it is left.
void ApplyModeLocked(LdnMode mode) {
    g_mode = mode;
    if (g_hybrid) {
        g_hybrid->SetMode(ToNetworkMode(mode));
    }
    // Hosting in the game opens what the mode needs: the room (Suyu room), this PC's server
    // (Ryujinx LDN), or both (Both).
    Network::SetHostingHook(OnGameHosting);
    if (mode == LdnMode::Auto) {
        if (!g_bridge) {
            if (const auto ryu = Network::RyuLdn::GetActiveBackend()) {
                g_bridge = std::make_unique<Network::RyuLdn::RoomBridge>(ryu);
            }
        }
    } else {
        g_bridge.reset();
    }
}

} // namespace

bool SetupLdnBackend() {
    std::scoped_lock lock{g_mutex};
    const std::string backend = Trimmed(GS::Value("Multiplayer", "backend"));
    LdnMode mode = LdnMode::Auto; // default: whatever the lobby needs, no switching
    bool known = true;
    if (backend == "ryuldn") {
        mode = LdnMode::RyuLdn;
    } else if (backend == "room") {
        mode = LdnMode::Room;
    } else if (!backend.empty() && backend != "auto") {
        LOG_ERROR(Network,
                  "multiplayer: unknown [Multiplayer] backend '{}' (room, ryuldn or auto)",
                  backend);
        known = false;
    }

    // Both transports are always set up (a RyuLdn server is only contacted once it is used), so
    // the mode can change while the game runs.
    auto servers = ConfiguredRyuLdnServers();
    const std::string& passphrase = servers[0].passphrase;
    if (!passphrase.empty() &&
        (passphrase.size() != 16 || passphrase.rfind("Ryujinx-", 0) != 0)) {
        LOG_WARNING(Network, "multiplayer: ryuldn_passphrase '{}' isn't Ryujinx-xxxxxxxx; the "
                             "server treats it as empty (public lobbies)",
                    passphrase);
    }
    LOG_INFO(Network, "multiplayer: local play over {} (RyuLdn server {}:{}{}{})", ModeText(mode),
             servers[0].host, servers[0].port,
             servers.size() > 1 ? fmt::format(" + {} more", servers.size() - 1) : std::string{},
             passphrase.empty() ? "" : ", private passphrase");
    const auto ryu = std::make_shared<Network::RyuLdn::Backend>(std::move(servers));
    Network::RyuLdn::Register(ryu);
    g_hybrid = std::make_shared<Network::HybridLdnBackend>(
        std::make_shared<Network::RoomLdnBackend>(), ryu);
    Network::SetHybridLdnBackend(g_hybrid);
    ApplyModeLocked(mode);
    return known;
}

LdnMode CurrentLdnMode() {
    std::scoped_lock lock{g_mutex};
    return g_mode;
}

bool UsingRyuLdn() {
    std::scoped_lock lock{g_mutex};
    return g_mode != LdnMode::Room;
}

bool SetLdnMode(LdnMode mode, std::string* error) {
    std::scoped_lock lock{g_mutex};
    if (!g_hybrid) {
        *error = "local play isn't set up";
        return false;
    }
    if (mode != g_mode) {
        const auto ryu = Network::RyuLdn::GetActiveBackend();
        if (g_hybrid->GetActive() != Network::LdnTransport::None ||
            (ryu && ryu->client().InNetwork())) {
            *error = "leave the lobby first";
            return false;
        }
    }
    GS::SetValue("Multiplayer", "backend", ModeText(mode));
    ApplyModeLocked(mode);
    LOG_INFO(Network, "multiplayer: local play now over {}", ModeText(mode));
    return true;
}

std::string BridgeStatusText() {
    std::scoped_lock lock{g_mutex};
    if (!g_bridge) {
        return {};
    }
    const auto status = g_bridge->GetStatus();
    if (!status.active) {
        return "Bridge: ready (a lobby you host is open to this room too)";
    }
    std::string out = fmt::format("Bridge: lobby open to the room, {} room player{} in it",
                                  status.players, status.players == 1 ? "" : "s");
    for (const auto& name : status.names) {
        out += "\n  - " + name;
    }
    return out;
}

std::string RyuLdnServer() {
    const auto ryu = Network::RyuLdn::GetActiveBackend();
    if (!ryu) {
        return {};
    }
    std::string out;
    for (std::size_t i = 0; i < ryu->ServerCount(); i++) {
        out += fmt::format("{}{}:{}", i ? ", " : "", ryu->settings(i).host, ryu->settings(i).port);
    }
    return out;
}

// ── Public rooms: the lobbies of this game on the RyuLdn servers (a scan per server) ──

namespace {

struct RoomsState {
    std::mutex lock;
    PublicRooms rooms;
};
const auto g_rooms = std::make_shared<RoomsState>();

std::string ServerLabel(const std::string& host, std::size_t index) {
    if (host == DefaultRyuLdnHost) {
        return "Ryujinx";
    }
    if (host == "127.0.0.1") {
        return "This PC";
    }
    return fmt::format("Server {}", index + 1);
}

std::string NodeName(const std::array<u8, 0x21>& raw) {
    std::string name(reinterpret_cast<const char*>(raw.data()), raw.size());
    name.resize(std::strlen(name.c_str()));
    return name.empty() ? std::string{"?"} : name;
}

void ScanPublicRooms(std::shared_ptr<RoomsState> state, u64 local_communication_id) {
    namespace RL = Network::RyuLdn;
    const auto servers = ConfiguredRyuLdnServers();
    RL::ScanFilterBlob filter{};
    Service::LDN::ScanFilter f{};
    f.network_id.intent_id.local_communication_id = local_communication_id;
    f.flag = Service::LDN::ScanFilterFlag::LocalCommunicationId;
    std::memcpy(filter.data(), &f, sizeof(f));

    std::vector<std::optional<std::vector<RL::NetworkInfoBlob>>> found(servers.size());
    std::vector<std::string> errors(servers.size());
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < servers.size(); i++) {
        threads.emplace_back([&, i] {
            // A short session of its own: doesn't disturb the game's connection.
            auto settings = servers[i];
            settings.keep_identity = false;
            RL::Client client{std::move(settings), {}};
            found[i] = client.Scan(filter);
            if (!found[i]) {
                errors[i] = client.LastError().empty() ? "no answer" : client.LastError();
            }
            client.Close();
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    PublicRooms result;
    result.refreshed = true;
    for (std::size_t i = 0; i < servers.size(); i++) {
        const std::string label = ServerLabel(servers[i].host, i);
        if (!found[i]) {
            if (servers[i].host != "127.0.0.1") { // our own server only runs while we host
                result.errors.push_back(label + ": " + errors[i]);
            }
            continue;
        }
        for (const auto& blob : *found[i]) {
            Service::LDN::NetworkInfo info;
            std::memcpy(&info, blob.data(), sizeof(info));
            PublicRoom room;
            room.server = label;
            room.players = info.ldn.node_count;
            room.max_players = info.ldn.node_count_max;
            for (const auto& node : info.ldn.nodes) {
                if (node.is_connected) {
                    room.names.push_back(NodeName(node.user_name));
                }
            }
            room.host = room.names.empty() ? std::string{"?"} : room.names.front();
            result.rooms.push_back(std::move(room));
        }
    }
    std::scoped_lock lk{state->lock};
    result.private_passphrase = !servers[0].passphrase.empty();
    result.last_refresh = std::chrono::steady_clock::now();
    state->rooms = std::move(result);
}

} // namespace

void RefreshPublicRooms() {
    u64 id;
    {
        std::scoped_lock lock{g_mutex};
        id = g_game.id;
    }
    std::scoped_lock lk{g_rooms->lock};
    if (g_rooms->rooms.refreshing) {
        return;
    }
    if (id == 0) {
        g_rooms->rooms.errors = {"the game hasn't started yet"};
        return;
    }
    g_rooms->rooms.refreshing = true;
    // Detached: holds its own reference to the state, finishes within the connect timeouts.
    std::thread([state = g_rooms, id] {
        ScanPublicRooms(state, id);
        std::scoped_lock done{state->lock};
        state->rooms.refreshing = false;
    }).detach();
}

PublicRooms GetPublicRooms() {
    std::scoped_lock lk{g_rooms->lock};
    return g_rooms->rooms;
}

std::string RyuLdnStatusText() {
    std::scoped_lock lock{g_mutex};
    if (g_mode == LdnMode::Room) {
        return {};
    }
    if (!g_ryuldn_problem.empty()) {
        return "RyuLdn: " + g_ryuldn_problem;
    }
    const auto ryu = Network::RyuLdn::GetActiveBackend();
    if (!ryu) {
        return "RyuLdn: off";
    }
    std::string out = "RyuLdn: ";
    auto& client = ryu->client();
    if (ryu->IsConnected()) {
        const auto ip = ryu->GetFakeIpAddress();
        out += fmt::format("in a lobby as {}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3]);
    } else if (client.IsConnected()) {
        out += "connected, not in a lobby";
    } else {
        const std::string error = client.LastError();
        out += error.empty() ? "not connected yet (connects when the game looks for lobbies)"
                             : "not connected (" + error + ")";
    }
    return out;
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

bool HostLocked(Core::System* system, const Config& c, std::string* error) {
    if (g_mode == LdnMode::RyuLdn) {
        *error = "local play is set to Ryujinx LDN only; pick Both or Suyu room first";
        return false;
    }
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
    if (system != nullptr) {
        Network::GameInfo game;
        [[maybe_unused]] auto _ = system->GetGameName(game.name);
        game.id = system->GetApplicationProcessProgramID();
        g_game = game;
    }
    if (!room->Create(c.room_name, "", "", c.port, c.password, c.max_players, c.nickname, g_game,
                      std::make_unique<Network::VerifyUser::NullBackend>(), {})) {
        *error = fmt::format("could not open a room on port {} (already in use?)", c.port);
        LOG_ERROR(Network, "multiplayer: {}", *error);
        return false;
    }
    // Players of this room may need addresses that belong to the Ryujinx side (the bridge).
    room->SetHostGateway(true);
    // A room hosted in a mode that includes Ryujinx LDN brings this PC's LDN server up too, however
    // the room was started (game, ini or the dev panel), so Ryujinx friends can join.
    if (g_mode != LdnMode::Room && OwnServerIsMain() && !g_auto_server) {
        std::string server_error;
        if (StartOwnServer(&server_error)) {
            g_auto_server = true;
        } else {
            LOG_ERROR(Network, "multiplayer: opening the RyuLdn server failed: {}", server_error);
        }
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

bool Host(Core::System& system, const Config& c, std::string* error) {
    std::scoped_lock lock{g_mutex};
    return HostLocked(&system, c, error);
}

bool Join(const Config& c, std::string* error) {
    std::scoped_lock lock{g_mutex};
    if (g_mode == LdnMode::RyuLdn) {
        *error = "local play is set to Ryujinx LDN only; pick Both or Suyu room first";
        return false;
    }
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
    // A friend who hosts runs their RyuLdn server on the same address: lobbies there are
    // found and joined too (Ryujinx players use that server).
    g_friend_server = c.address;
    {
        std::string ignored;
        ApplyServersLive(&ignored);
    }
    LOG_INFO(Network, "multiplayer: joining {}:{} as {}", c.address, c.port, c.nickname);
    member->Join(c.nickname, c.address.c_str(), c.port, 0, Network::NoPreferredIP, c.password);
    return true;
}

void LeaveLocked() {
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

void Leave() {
    std::scoped_lock lock{g_mutex};
    LeaveLocked();
    if (!g_friend_server.empty()) {
        g_friend_server.clear();
        std::string ignored;
        ApplyServersLive(&ignored);
    }
}

void Shutdown() {
    std::scoped_lock lock{g_mutex};
    g_bridge.reset();
    if (g_auto_server) {
        StopOwnServer();
        g_auto_server = false;
    }
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
    if (CurrentLdnMode() == LdnMode::RyuLdn) {
        if (c.mode != "off") {
            LOG_INFO(Network, "multiplayer: [Multiplayer] mode = {} ignored (backend = ryuldn)",
                     c.mode);
        }
        return;
    }
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

bool Kick(const std::string& nickname) {
    std::scoped_lock lock{g_mutex};
    if (!g_hosting) {
        return false;
    }
    const auto room = Network::GetRoom().lock();
    if (!room || room->GetState() != Network::Room::State::Open) {
        return false;
    }
    LOG_INFO(Network, "multiplayer: kicking {}", nickname);
    room->KickMember(nickname);
    return true;
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
            st.names.push_back(m.nickname);
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
