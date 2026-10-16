// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <thread>

#include "common/logging.h"
#include "core/hle/service/ldn/hybrid_network.h"
#include "core/hle/service/ldn/ldn_results.h"
#include "network/network.h"
#include "network/ryuldn/backend.h"

namespace Service::LDN {

namespace {

/// A server that didn't answer is left alone for this long when hosting (each try can cost the
/// connect timeout).
constexpr auto kServerRetry = std::chrono::seconds(10);

} // namespace

HybridLdnNetwork::HybridLdnNetwork(std::shared_ptr<Network::HybridLdnBackend> backend,
                                   std::shared_ptr<Network::RyuLdn::Backend> ryu,
                                   std::function<RyuLdnNetwork::AppInfo()> app_info)
    : hybrid{std::move(backend)}, server{ryu}, ryuldn{std::move(ryu), std::move(app_info)} {}

HybridLdnNetwork::~HybridLdnNetwork() {
    Finalize();
}

ILdnNetwork& HybridLdnNetwork::Net(Via via) {
    return via == Via::Room ? static_cast<ILdnNetwork&>(lan) : static_cast<ILdnNetwork&>(ryuldn);
}

ILdnNetwork& HybridLdnNetwork::Visible() const {
    if (ILdnNetwork* net = current.load()) {
        return *net;
    }
    return ryuldn;
}

void HybridLdnNetwork::Enter(Via via) {
    hybrid->SetActive(via);
    current = &Net(via);
}

void HybridLdnNetwork::Leave() {
    current = nullptr;
    hybrid->SetActive(Network::LdnTransport::None);
}

bool HybridLdnNetwork::RoomUp() const {
    const auto member = Network::GetRoomMember().lock();
    return member && member->IsConnected();
}

HybridLdnNetwork::Via HybridLdnNetwork::GuessVia(const NetworkInfo& info) const {
    {
        std::scoped_lock lk{origins_lock};
        for (const auto& origin : origins) {
            if (origin.session == info.network_id.session_id) {
                return origin.via;
            }
        }
    }
    // Not from a scan of this session: the host's address tells the network (the server hands
    // out 10.114.x.x; room addresses are 192.168.x.y). Stored byte-reversed.
    const auto& ip = info.ldn.nodes[0].ipv4_address;
    return ip[3] == 10 && ip[2] == 114 ? Via::RyuLdn : Via::Room;
}

State HybridLdnNetwork::GetState() const {
    return Visible().GetState();
}

Result HybridLdnNetwork::GetNetworkInfo(NetworkInfo& out_network) const {
    if (ILdnNetwork* net = current.load()) {
        return net->GetNetworkInfo(out_network);
    }
    return ResultBadState;
}

Result HybridLdnNetwork::GetNetworkInfo(NetworkInfo& out_network,
                                        std::span<NodeLatestUpdate> out_updates) {
    if (ILdnNetwork* net = current.load()) {
        return net->GetNetworkInfo(out_network, out_updates);
    }
    return ResultBadState;
}

DisconnectReason HybridLdnNetwork::GetDisconnectReason() const {
    return Visible().GetDisconnectReason();
}

Result HybridLdnNetwork::Scan(std::span<NetworkInfo> out_networks, s16& out_count,
                              const ScanFilter& filter) {
    out_count = 0;
    std::vector<NetworkInfo> from_server(out_networks.size());
    std::vector<NetworkInfo> from_room(out_networks.size());
    s16 server_count = 0;
    s16 room_count = 0;

    // Both scans take about a second: run them side by side.
    std::thread room_scan;
    if (RoomUp()) {
        room_scan = std::thread([&] {
            [[maybe_unused]] const Result rc = lan.Scan(from_room, room_count, filter);
        });
    }
    // Servers that don't answer (this PC's own, while nobody hosts) fail fast.
    const Result server_result = ryuldn.Scan(from_server, server_count, filter);
    if (room_scan.joinable()) {
        room_scan.join();
    }
    if (server_result.IsError() && room_count == 0) {
        return server_result;
    }

    std::vector<Origin> found;
    const auto add = [&](const NetworkInfo& info, Via via) {
        if (out_count >= static_cast<s16>(out_networks.size())) {
            return;
        }
        // The same lobby listed by the server and (through the bridge) by the room: once.
        for (const auto& known : found) {
            if (known.session == info.network_id.session_id) {
                return;
            }
        }
        found.push_back({via, info.network_id.session_id});
        out_networks[out_count++] = info;
    };
    for (s16 i = 0; i < server_count; i++) {
        add(from_server[i], Via::RyuLdn);
    }
    for (s16 i = 0; i < room_count; i++) {
        add(from_room[i], Via::Room);
    }
    {
        std::scoped_lock lk{origins_lock};
        origins = std::move(found);
    }
    LOG_INFO(Service_LDN, "hybrid scan: {} lobbies ({} on RyuLdn servers, {} in the room)",
             out_count, server_count, room_count);
    return ResultSuccess;
}

Result HybridLdnNetwork::SetAdvertiseData(std::span<const u8> data) {
    const Result rc = ryuldn.SetAdvertiseData(data);
    if (rc.IsError()) {
        return rc;
    }
    // The room network keeps its own copy for a lobby it hosts itself.
    return lan.SetAdvertiseData(data);
}

Result HybridLdnNetwork::OpenAccessPoint() {
    Leave();
    const Result rc = ryuldn.OpenAccessPoint();
    if (rc.IsError()) {
        return rc;
    }
    return lan.OpenAccessPoint();
}

Result HybridLdnNetwork::CloseAccessPoint() {
    Result rc = ryuldn.CloseAccessPoint();
    const Result lan_rc = lan.CloseAccessPoint();
    Leave();
    StopHosting();
    return rc.IsError() ? rc : lan_rc;
}

Result HybridLdnNetwork::OpenStation() {
    Leave();
    const Result rc = ryuldn.OpenStation();
    if (rc.IsError()) {
        return rc;
    }
    return lan.OpenStation();
}

Result HybridLdnNetwork::CloseStation() {
    Result rc = ryuldn.CloseStation();
    const Result lan_rc = lan.CloseStation();
    Leave();
    return rc.IsError() ? rc : lan_rc;
}

Result HybridLdnNetwork::CreateNetwork(const SecurityConfig& security_config,
                                       const UserConfig& user_config,
                                       const NetworkConfig& network_config) {
    // On a RyuLdn server when there is one: Ryujinx players can only get there, and the bridge
    // opens the same lobby to the room's players. Without a server, the room alone.
    // The frontend opens the room and this PC's server for friends first (hosting = both).
    Network::RunHostingHook(true);
    hosting_hook_run = true;
    Result server_rc = ResultAccessPointConnectionFailed;
    if (std::chrono::steady_clock::now() >= ryuldn_down_until) {
        Enter(Via::RyuLdn); // the sockets' address comes from the server once it is up
        server_rc = ryuldn.CreateNetwork(security_config, user_config, network_config);
        if (server_rc.IsSuccess()) {
            LOG_INFO(Service_LDN, "hybrid: lobby created on the RyuLdn server{}",
                     RoomUp() ? " (open to the room through the bridge)" : "");
            return server_rc;
        }
        Leave();
        ryuldn_down_until = std::chrono::steady_clock::now() + kServerRetry;
    }
    // The room the hook just opened joins itself asynchronously: give it a moment.
    for (int i = 0; i < 40 && !RoomUp(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (RoomUp()) {
        Enter(Via::Room);
        const Result rc = lan.CreateNetwork(security_config, user_config, network_config);
        if (rc.IsSuccess()) {
            LOG_INFO(Service_LDN, "hybrid: no RyuLdn server answered; lobby created in the room");
            return rc;
        }
        Leave();
        StopHosting();
        return rc;
    }
    StopHosting();
    return server_rc;
}

void HybridLdnNetwork::StopHosting() {
    if (hosting_hook_run.exchange(false)) {
        Network::RunHostingHook(false);
    }
}

Result HybridLdnNetwork::DestroyNetwork() {
    ILdnNetwork* net = current.load();
    const Result rc = net ? net->DestroyNetwork() : ResultSuccess;
    Leave();
    StopHosting();
    return rc;
}

Result HybridLdnNetwork::Connect(const NetworkInfo& network_info, const UserConfig& user_config,
                                 u16 local_communication_version) {
    const Via via = GuessVia(network_info);
    LOG_INFO(Service_LDN, "hybrid: joining through {}",
             via == Via::RyuLdn ? "the RyuLdn server" : "the room");
    Enter(via);
    const Result rc = Net(via).Connect(network_info, user_config, local_communication_version);
    if (rc.IsError()) {
        Leave();
    }
    return rc;
}

Result HybridLdnNetwork::Disconnect() {
    ILdnNetwork* net = current.load();
    const Result rc = net ? net->Disconnect() : ResultSuccess;
    Leave();
    return rc;
}

Result HybridLdnNetwork::SetStationAcceptPolicy(AcceptPolicy accept_policy) {
    if (ILdnNetwork* net = current.load()) {
        return net->SetStationAcceptPolicy(accept_policy);
    }
    return ResultSuccess;
}

Result HybridLdnNetwork::Initialize(LanEventFunc lan_event, bool listening) {
    const Result rc = ryuldn.Initialize(lan_event, listening);
    if (rc.IsError()) {
        return rc;
    }
    return lan.Initialize(std::move(lan_event), listening);
}

Result HybridLdnNetwork::Finalize() {
    const Result rc = ryuldn.Finalize();
    lan.Finalize();
    Leave();
    StopHosting();
    return rc;
}

void HybridLdnNetwork::ReceivePacket(const Network::LDNPacket& packet) {
    if (current.load() == &ryuldn) {
        return; // a RyuLdn lobby: its traffic isn't the room's business (the bridge answers)
    }
    lan.ReceivePacket(packet);
}

} // namespace Service::LDN
