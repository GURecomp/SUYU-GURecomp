// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "network/ryuldn/protocol.h"

namespace Network::RyuLdn {

class P2pLink;

/**
 * Connection to a RyuLdn master server (LdnServer): lobby control (scan, create, connect,
 * advertise data, accept policy, reject) and the relayed game traffic (ProxyData).
 *
 * Requests block the caller like the HLE calls they serve, with the timeouts Ryujinx's client
 * uses (connect 4 s, scan 1 s, reject 6 s). Server pushes (network changes, proxy config,
 * proxied data, lost connection) arrive on the client's reader thread through the callbacks;
 * the client holds no lock while calling them.
 */
class Client {
public:
    struct Callbacks {
        /// The lobby changed (SyncNetwork / Connected: connected = true), or this node left it
        /// (Disconnect from the server or a lost connection: connected = false, info zeroed).
        std::function<void(const NetworkInfoBlob& info, bool connected)> network_change;
        /// This node's address in the lobby's virtual network (arrives before Connected).
        std::function<void(const ProxyConfig& config)> proxy_config;
        /// Relayed game traffic for this node.
        std::function<void(const ProxyDataHeader& header, std::span<const u8> data)> proxy_data;
    };

    struct Settings {
        std::string host;
        u16 port = DefaultPort;
        std::string passphrase;  // "" = public lobbies
        std::string game_version; // display version for RyuNetworkConfig ("1.4.0")
        /// false: a throwaway session (lobby list) that neither reuses nor replaces the id and
        /// MAC the server gave this process's real connection.
        bool keep_identity = true;
    };

    Client(Settings settings, Callbacks callbacks);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Opens the connection if needed (TCP + Initialize + Passphrase). False with a message in
    /// LastError() when the server can't be reached.
    bool EnsureConnected();

    /// Closes the connection (leaves any lobby).
    void Close();

    bool IsConnected() const {
        return connected.load();
    }

    /// In a lobby (created or joined) right now.
    bool InNetwork() const {
        return in_network.load();
    }

    std::optional<ProxyConfig> GetProxyConfig() const;

    /// Lobbies matching the filter; nullopt when the server didn't answer in time.
    std::optional<std::vector<NetworkInfoBlob>> Scan(const ScanFilterBlob& filter);

    /// Host a lobby. The advertise data follows the request. None = created (network_change
    /// fired with the lobby already); otherwise why not.
    NetworkError CreateNetwork(CreateAccessPointRequest request, std::span<const u8> advertise);
    NetworkError CreateNetworkPrivate(CreateAccessPointPrivateRequest request,
                                      std::span<const u8> advertise);

    /// Join a lobby. None = joined; else the server's error (ConnectTimeout if it said nothing).
    NetworkError Connect(const ConnectRequest& request);
    NetworkError ConnectPrivate(const ConnectPrivateRequest& request);

    /// Leave the current lobby (as host: closes it).
    void DisconnectNetwork();

    /// Host only: kick a node. False when the server refused it or didn't answer.
    bool Reject(u32 node_id, u32 disconnect_reason);

    void SetAdvertiseData(std::span<const u8> data);
    void SetAcceptPolicy(u8 policy);

    /// Relay game traffic (dropped while not in a lobby).
    void SendProxyData(const ProxyInfo& info, std::span<const u8> data);

    /// The last connection problem, for logs and the menu.
    std::string LastError() const;

    /// The game's display version sent with created lobbies (Ryujinx sets it at LDN init).
    void SetGameVersion(std::string version);

    /// Milliseconds the requests wait (Ryujinx: FailureTimeout, ScanTimeout, InactiveTimeout).
    static constexpr int ConnectTimeoutMs = 4000;
    static constexpr int ScanTimeoutMs = 1000;
    static constexpr int RejectTimeoutMs = 6000;

private:
    bool Send(const std::vector<u8>& bytes);
    void ReaderLoop(std::uintptr_t socket_handle);
    void Handle(const Packet& packet);
    void OnConnectionLost(const std::string& why);
    NetworkError WaitForJoin();
    void BeginJoin();
    /// Joining a lobby whose host runs its own P2P proxy (Ryujinx with UPnP): the game traffic
    /// goes over a second connection, to that proxy.
    void StartP2p(const ExternalProxyConfig& config);
    void HandleP2p(const Packet& packet);
    void OnP2pLost(const std::string& why);
    void ResetP2p();

    const Settings settings;
    const Callbacks callbacks;

    mutable std::mutex lock;          // state below
    std::condition_variable changed;  // any reply arrived
    std::mutex send_lock;
    std::mutex connect_lock;          // one EnsureConnected/Close at a time

    std::uintptr_t sock;              // platform socket, kInvalidSocket when closed
    std::thread reader;
    std::atomic<bool> connected{false};
    std::atomic<bool> in_network{false};

    std::optional<ProxyConfig> proxy;
    std::vector<NetworkInfoBlob> scan_results;
    bool scan_done = false;
    bool join_done = false;          // Connected arrived
    NetworkError join_error = NetworkError::None;
    bool reject_done = false;
    NetworkError last_network_error = NetworkError::None;
    std::string last_error;
    std::string game_version;
    u64 generation = 0;
    bool p2p_expected = false; // the server pointed this join at a host's P2P proxy
    bool p2p_ready = false;    // that proxy gave us our address

    std::mutex p2p_lock;
    std::unique_ptr<P2pLink> p2p;              // bumps on every (re)connect / loss
};

} // namespace Network::RyuLdn
