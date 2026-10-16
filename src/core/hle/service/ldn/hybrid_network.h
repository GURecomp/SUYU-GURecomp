// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include "core/hle/service/ldn/lan_discovery.h"
#include "core/hle/service/ldn/ldn_network.h"
#include "core/hle/service/ldn/ryuldn_network.h"
#include "network/ldn_backend.h"
#include "network/ryuldn/backend.h"

namespace Service::LDN {

/**
 * LDN over both transports at once (mode "auto"): a suyu room (LANDiscovery) and RyuLdn servers
 * (RyuLdnNetwork). Nothing has to be switched by hand:
 *  - Scan asks both and lists the lobbies of both (the same lobby reached both ways once),
 *  - Connect goes through the transport the lobby was found on,
 *  - a lobby this game hosts is created on the RyuLdn server, and the room bridge
 *    (Network::RyuLdn::RoomBridge) lets the room's players join it too, so suyu and Ryujinx
 *    players share it. With no reachable server it is a plain room lobby instead.
 * The data plane follows: Network::HybridLdnBackend::SetActive tells the sockets which
 * transport carries the lobby we are in.
 */
class HybridLdnNetwork final : public ILdnNetwork {
public:
    HybridLdnNetwork(std::shared_ptr<Network::HybridLdnBackend> backend,
                     std::shared_ptr<Network::RyuLdn::Backend> ryu,
                     std::function<RyuLdnNetwork::AppInfo()> app_info);
    ~HybridLdnNetwork() override;

    State GetState() const override;
    Result GetNetworkInfo(NetworkInfo& out_network) const override;
    Result GetNetworkInfo(NetworkInfo& out_network,
                          std::span<NodeLatestUpdate> out_updates) override;
    DisconnectReason GetDisconnectReason() const override;
    Result Scan(std::span<NetworkInfo> out_networks, s16& out_count,
                const ScanFilter& filter) override;
    Result SetAdvertiseData(std::span<const u8> data) override;
    Result OpenAccessPoint() override;
    Result CloseAccessPoint() override;
    Result OpenStation() override;
    Result CloseStation() override;
    Result CreateNetwork(const SecurityConfig& security_config, const UserConfig& user_config,
                         const NetworkConfig& network_config) override;
    Result DestroyNetwork() override;
    Result Connect(const NetworkInfo& network_info, const UserConfig& user_config,
                   u16 local_communication_version) override;
    Result Disconnect() override;
    Result SetStationAcceptPolicy(AcceptPolicy accept_policy) override;
    Result Initialize(LanEventFunc lan_event, bool listening = true) override;
    Result Finalize() override;

    /// The room's LDN packets: only the room network takes them, and only while no RyuLdn lobby
    /// is in use (the bridge answers the room for a lobby we host on a server).
    void ReceivePacket(const Network::LDNPacket& packet);

private:
    using Via = Network::LdnTransport;

    ILdnNetwork& Net(Via via);
    /// The network whose state the game sees: the lobby's, else the RyuLdn one (both are kept in
    /// the same open/closed state by the Open*/Close* calls).
    ILdnNetwork& Visible() const;
    void Enter(Via via);
    void Leave();
    bool RoomUp() const;
    Via GuessVia(const NetworkInfo& info) const;

    std::shared_ptr<Network::HybridLdnBackend> hybrid;
    std::shared_ptr<Network::RyuLdn::Backend> server;
    mutable LANDiscovery lan;
    mutable RyuLdnNetwork ryuldn;

    std::atomic<bool> hosting_hook_run{false}; // the frontend was told we host
    void StopHosting();

    std::atomic<ILdnNetwork*> current{nullptr}; // the network the lobby we are in belongs to

    mutable std::mutex origins_lock;
    struct Origin {
        Via via;
        SessionId session;
    };
    std::vector<Origin> origins; // where the last scan found each lobby

    /// Set when the servers didn't answer: scans and new lobbies skip them for a while.
    std::chrono::steady_clock::time_point ryuldn_down_until{};
};

} // namespace Service::LDN
