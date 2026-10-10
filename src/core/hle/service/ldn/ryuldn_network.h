// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/hle/service/ldn/ldn_network.h"

namespace Network::RyuLdn {
class Backend;
}

namespace Service::LDN {

/**
 * LDN through a RyuLdn master server (the server Ryujinx uses), so this game can share lobbies
 * with Ryujinx players. The server owns the lobby: it lists networks for Scan, assigns every
 * node an address in 10.114.0.0/16 and pushes the node list (SyncNetwork); the game's sockets
 * are relayed by Network::RyuLdn::Backend. States and results follow suyu's LANDiscovery where
 * the game can see them; the transitions follow Ryujinx's AccessPoint/Station.
 */
class RyuLdnNetwork final : public ILdnNetwork {
public:
    struct AppInfo {
        u64 local_communication_id; // the game's own id (NACP), replaces -1 in requests
        std::string display_version; // e.g. "1.4.0", sent with created lobbies
    };

    /// app_info is asked once, at Initialize (the game is running by then).
    RyuLdnNetwork(std::shared_ptr<Network::RyuLdn::Backend> backend,
                  std::function<AppInfo()> app_info);
    ~RyuLdnNetwork() override;

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

private:
    /// Server pushes (client reader thread).
    void OnNetworkChange(const std::array<u8, 0x480>& info, bool connected);
    void SetStateLocked(State new_state);
    void Signal();
    u64 ResolveId(u64 local_communication_id) const;

    std::shared_ptr<Network::RyuLdn::Backend> backend;
    std::function<AppInfo()> app_info_provider;
    AppInfo app_info{};

    mutable std::mutex mutex; // never held across a server request or the state event
    State state{State::None};
    NetworkInfo network_info{};
    std::array<NodeLatestUpdate, NodeCountMax> latest_updates{};
    DisconnectReason disconnect_reason{DisconnectReason::None};
    std::vector<u8> advertise_data;
    LanEventFunc lan_event;
};

} // namespace Service::LDN
