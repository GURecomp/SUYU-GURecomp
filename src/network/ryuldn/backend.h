// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "network/ldn_backend.h"
#include "network/ryuldn/client.h"

namespace Network::RyuLdn {

/**
 * LDN over RyuLdn master servers for a whole run: owns the server connections, gives the game's
 * proxied sockets their lobby address (ProxyConfig) and relays their packets as ProxyData.
 * The LDN state machine (Service::LDN::RyuLdnNetwork) drives the lobby side through the lobby
 * calls below and listens for lobby changes with SetNetworkListener.
 *
 * Several servers: server 0 is the main one (lobbies we host live there); a scan asks every
 * server and merges the lobbies, and joining one goes through the server that listed it. Only
 * one server connection is in a lobby at a time (the "active" one).
 */
class Backend final : public LdnBackend {
public:
    using NetworkListener = std::function<void(const NetworkInfoBlob& info, bool connected)>;

    /// servers: at least one; [0] is the main server.
    explicit Backend(std::vector<Client::Settings> servers);
    ~Backend() override;

    /// The connection the current (or last) lobby goes through.
    Client& client();
    std::size_t ServerCount() const;
    Client::Settings settings(std::size_t server = 0) const;

    /// Replaces the servers while the game runs (the menu's host/join). Refused (false + error)
    /// while in a lobby. The old connections are closed; lobbies found before are forgotten.
    bool SetServers(std::vector<Client::Settings> servers, std::string& error);

    // Lobby calls (RyuLdnNetwork).
    /// Lobbies of every server; nullopt when no server answered.
    std::optional<std::vector<NetworkInfoBlob>> Scan(const ScanFilterBlob& filter);
    NetworkError CreateNetwork(const CreateAccessPointRequest& request,
                               std::span<const u8> advertise);
    /// Joins through the server whose scan listed the lobby (the main server if none did).
    NetworkError Connect(const ConnectRequest& request);
    void SetGameVersion(const std::string& version);
    /// Closes every server connection.
    void CloseAll();

    /// Lobby changes go to one listener at a time; the newest owner wins. Clearing only works
    /// for the current owner (an older LDN session can't unhook a newer one).
    void SetNetworkListener(NetworkListener listener, const void* owner = nullptr);
    /// True when owner was the listener (and is now removed).
    bool ClearNetworkListener(const void* owner);

    bool IsConnected() const override;
    IPv4Address GetFakeIpAddress() const override;
    std::optional<IPv4Address> GetFakeSubnetMask() const override;
    void SendProxyPacket(const ProxyPacket& packet) override;
    void SendLdnPacket(const LDNPacket& packet) override;
    CallbackHandle<ProxyPacket> BindOnProxyPacketReceived(
        std::function<void(const ProxyPacket&)> callback) override;
    CallbackHandle<LDNPacket> BindOnLdnPacketReceived(
        std::function<void(const LDNPacket&)> callback) override;
    void Unbind(CallbackHandle<ProxyPacket> handle) override;
    void Unbind(CallbackHandle<LDNPacket> handle) override;

    /// Conversions between the wire's host-order u32 and suyu's a.b.c.d byte arrays.
    static u32 ToU32(const IPv4Address& ip);
    static IPv4Address ToAddress(u32 ip);

    /// An extra server that couldn't be reached is skipped by scans for this long.
    static constexpr std::chrono::seconds UnreachableRetry{30};

private:
    using NetworkKey = std::array<u8, 0x20>; // NetworkInfo's NetworkId (intent + session id)

    /// One list of servers and their connections. Replaced as a whole by SetServers; old sets
    /// stay alive (closed) until the backend goes, so references handed out stay valid.
    struct ServerSet {
        u64 generation = 0;
        std::vector<Client::Settings> settings;
        std::vector<std::shared_ptr<Client>> clients;
    };

    std::shared_ptr<ServerSet> Current() const;
    std::shared_ptr<ServerSet> MakeSet(std::vector<Client::Settings> servers, u64 generation);
    Client& Active() const;
    bool IsCurrent(u64 generation, std::size_t server) const;
    void OnProxyData(const ProxyDataHeader& header, std::span<const u8> data);
    void OnNetworkChange(const NetworkInfoBlob& info, bool connected);

    mutable std::mutex set_lock; // below
    std::shared_ptr<ServerSet> current;
    std::vector<std::shared_ptr<ServerSet>> retired;
    std::string game_version;
    std::atomic<u64> generation{0};
    std::atomic<std::size_t> active{0};

    std::mutex scan_lock; // below
    std::map<NetworkKey, std::size_t> lobby_server;
    std::vector<std::chrono::steady_clock::time_point> retry_after;

    std::mutex listener_lock;
    NetworkListener network_listener;
    const void* listener_owner = nullptr;

    std::mutex callbacks_lock;
    std::vector<CallbackHandle<ProxyPacket>> proxy_callbacks;
};

/// The RyuLdn backend of this run, null when LDN goes through a suyu room.
std::shared_ptr<Backend> GetActiveBackend();

/// Makes backend the run's LDN backend (also for GetLdnBackend()). Call once at start.
void Activate(std::shared_ptr<Backend> backend);

} // namespace Network::RyuLdn
