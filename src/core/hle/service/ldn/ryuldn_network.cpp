// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "core/hle/service/ldn/ldn_results.h"
#include "core/hle/service/ldn/ryuldn_network.h"
#include "network/ryuldn/backend.h"

namespace Service::LDN {

namespace RL = Network::RyuLdn;

static_assert(sizeof(NetworkInfo) == sizeof(RL::NetworkInfoBlob));
static_assert(sizeof(ScanFilter) == sizeof(RL::ScanFilterBlob));
static_assert(sizeof(SecurityConfig) == sizeof(RL::SecurityConfigBlob));
static_assert(sizeof(UserConfig) == sizeof(RL::UserConfigBlob));
static_assert(sizeof(NetworkConfig) == sizeof(RL::NetworkConfigBlob));

namespace {

constexpr u64 kDefaultLocalCommunicationId = ~u64{0}; // "this application's own id"

const char* StateName(State state) {
    switch (state) {
    case State::None:
        return "None";
    case State::Initialized:
        return "Initialized";
    case State::AccessPointOpened:
        return "AccessPointOpened";
    case State::AccessPointCreated:
        return "AccessPointCreated";
    case State::StationOpened:
        return "StationOpened";
    case State::StationConnected:
        return "StationConnected";
    case State::Error:
        return "Error";
    }
    return "?";
}

template <typename To, typename From>
To Blob(const From& from) {
    static_assert(sizeof(To) == sizeof(From));
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

} // namespace

RyuLdnNetwork::RyuLdnNetwork(std::shared_ptr<RL::Backend> backend_,
                             std::function<AppInfo()> app_info)
    : backend{std::move(backend_)}, app_info_provider{std::move(app_info)} {}

RyuLdnNetwork::~RyuLdnNetwork() {
    Finalize();
}

void RyuLdnNetwork::SetStateLocked(State new_state) {
    if (state != new_state) {
        LOG_INFO(Service_LDN, "ryuldn: state {} -> {}", StateName(state), StateName(new_state));
    }
    state = new_state;
}

void RyuLdnNetwork::Signal() {
    LanEventFunc event;
    {
        std::scoped_lock lk{mutex};
        event = lan_event;
    }
    if (event) {
        event();
    }
}

u64 RyuLdnNetwork::ResolveId(u64 local_communication_id) const {
    return local_communication_id == kDefaultLocalCommunicationId
               ? app_info.local_communication_id
               : local_communication_id;
}

Result RyuLdnNetwork::Initialize(LanEventFunc lan_event_, bool) {
    {
        std::scoped_lock lk{mutex};
        if (state != State::None) {
            return ResultSuccess;
        }
        app_info = app_info_provider ? app_info_provider() : AppInfo{};
        lan_event = std::move(lan_event_);
        SetStateLocked(State::Initialized);
    }
    backend->SetGameVersion(app_info.display_version);
    backend->SetNetworkListener(
        [this](const RL::NetworkInfoBlob& info, bool connected) { OnNetworkChange(info, connected); },
        this);
    LOG_INFO(Service_LDN,
             "ryuldn: LDN through {}:{} (+{} more servers), local communication id {:016X}, "
             "version '{}'",
             backend->settings().host, backend->settings().port, backend->ServerCount() - 1,
             app_info.local_communication_id, app_info.display_version);
    return ResultSuccess;
}

Result RyuLdnNetwork::Finalize() {
    State current;
    {
        std::scoped_lock lk{mutex};
        current = state;
    }
    if (current == State::None) {
        return ResultSuccess;
    }
    if (current == State::AccessPointCreated) {
        DestroyNetwork();
    } else if (current == State::StationConnected) {
        Disconnect();
    }
    if (backend->ClearNetworkListener(this)) {
        backend->CloseAll(); // Ryujinx drops its server connection here too
    }
    {
        std::scoped_lock lk{mutex};
        SetStateLocked(State::None);
        lan_event = {};
    }
    return ResultSuccess;
}

void RyuLdnNetwork::OnNetworkChange(const RL::NetworkInfoBlob& blob, bool connected) {
    {
        std::scoped_lock lk{mutex};
        if (connected) {
            const auto next = Blob<NetworkInfo>(blob);
            for (std::size_t i = 0; i < NodeCountMax; i++) {
                const bool before = network_info.ldn.nodes[i].is_connected != 0;
                const bool after = next.ldn.nodes[i].is_connected != 0;
                if (!before && after) {
                    latest_updates[i].state_change |= NodeStateChange::Connect;
                } else if (before && !after) {
                    latest_updates[i].state_change |= NodeStateChange::Disconnect;
                }
            }
            network_info = next;
        } else {
            // The lobby closed (host left) or the server connection broke.
            for (std::size_t i = 0; i < NodeCountMax; i++) {
                if (network_info.ldn.nodes[i].is_connected != 0) {
                    latest_updates[i].state_change |= NodeStateChange::Disconnect;
                }
            }
            if (state == State::AccessPointCreated) {
                disconnect_reason = DisconnectReason::DestroyedBySystem;
                SetStateLocked(State::AccessPointOpened);
            } else if (state == State::StationConnected) {
                disconnect_reason = DisconnectReason::DestroyedByUser;
                SetStateLocked(State::StationOpened);
            }
            LOG_INFO(Service_LDN, "ryuldn: left the lobby ({})",
                     backend->client().IsConnected() ? "closed by the host"
                                                     : backend->client().LastError());
        }
    }
    Signal();
}

State RyuLdnNetwork::GetState() const {
    std::scoped_lock lk{mutex};
    return state;
}

Result RyuLdnNetwork::GetNetworkInfo(NetworkInfo& out_network) const {
    std::scoped_lock lk{mutex};
    if (state == State::AccessPointCreated || state == State::StationConnected) {
        out_network = network_info;
        return ResultSuccess;
    }
    return ResultBadState;
}

Result RyuLdnNetwork::GetNetworkInfo(NetworkInfo& out_network,
                                     std::span<NodeLatestUpdate> out_updates) {
    if (out_updates.size() > NodeCountMax) {
        return ResultInvalidBufferCount;
    }
    std::scoped_lock lk{mutex};
    if (state != State::AccessPointCreated && state != State::StationConnected) {
        return ResultBadState;
    }
    out_network = network_info;
    for (std::size_t i = 0; i < out_updates.size(); i++) {
        out_updates[i].state_change = latest_updates[i].state_change;
        latest_updates[i].state_change = NodeStateChange::None;
    }
    return ResultSuccess;
}

DisconnectReason RyuLdnNetwork::GetDisconnectReason() const {
    std::scoped_lock lk{mutex};
    return disconnect_reason;
}

Result RyuLdnNetwork::Scan(std::span<NetworkInfo> out_networks, s16& out_count,
                           const ScanFilter& filter) {
    out_count = 0;
    ScanFilter request = filter;
    {
        std::scoped_lock lk{mutex};
        if (state == State::None) {
            return ResultBadState;
        }
        request.network_id.intent_id.local_communication_id =
            ResolveId(request.network_id.intent_id.local_communication_id);
    }
    const auto found = backend->Scan(Blob<RL::ScanFilterBlob>(request));
    if (!found) {
        // Server unreachable or slow: no lobbies, like Ryujinx (the game keeps scanning).
        return ResultSuccess;
    }
    for (const auto& info : *found) {
        if (out_count >= static_cast<s16>(out_networks.size())) {
            break;
        }
        out_networks[out_count++] = Blob<NetworkInfo>(info);
    }
    return ResultSuccess;
}

Result RyuLdnNetwork::SetAdvertiseData(std::span<const u8> data) {
    if (data.size() > AdvertiseDataSizeMax) {
        return ResultAdvertiseDataTooLarge;
    }
    bool created;
    {
        std::scoped_lock lk{mutex};
        advertise_data.assign(data.begin(), data.end());
        created = state == State::AccessPointCreated;
    }
    if (created) {
        backend->client().SetAdvertiseData(data);
    }
    return ResultSuccess;
}

Result RyuLdnNetwork::OpenAccessPoint() {
    State current;
    {
        std::scoped_lock lk{mutex};
        current = state;
    }
    if (current == State::None) {
        return ResultBadState;
    }
    if (current == State::StationConnected) {
        Disconnect();
    }
    {
        std::scoped_lock lk{mutex};
        disconnect_reason = DisconnectReason::None;
        advertise_data.clear();
        latest_updates = {};
        SetStateLocked(State::AccessPointOpened);
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::CloseAccessPoint() {
    State current;
    {
        std::scoped_lock lk{mutex};
        current = state;
    }
    if (current == State::None) {
        return ResultBadState;
    }
    if (current == State::AccessPointCreated) {
        DestroyNetwork();
    }
    {
        std::scoped_lock lk{mutex};
        SetStateLocked(State::Initialized);
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::OpenStation() {
    State current;
    {
        std::scoped_lock lk{mutex};
        current = state;
    }
    if (current == State::None) {
        return ResultBadState;
    }
    if (current == State::AccessPointCreated) {
        DestroyNetwork();
    }
    {
        std::scoped_lock lk{mutex};
        disconnect_reason = DisconnectReason::None;
        latest_updates = {};
        SetStateLocked(State::StationOpened);
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::CloseStation() {
    State current;
    {
        std::scoped_lock lk{mutex};
        current = state;
    }
    if (current == State::None) {
        return ResultBadState;
    }
    if (current == State::StationConnected) {
        Disconnect();
    }
    {
        std::scoped_lock lk{mutex};
        SetStateLocked(State::Initialized);
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::CreateNetwork(const SecurityConfig& security_config,
                                    const UserConfig& user_config,
                                    const NetworkConfig& network_config) {
    RL::CreateAccessPointRequest request{};
    std::vector<u8> advert;
    {
        std::scoped_lock lk{mutex};
        if (state != State::AccessPointOpened) {
            return ResultBadState;
        }
        NetworkConfig config = network_config;
        config.intent_id.local_communication_id =
            ResolveId(config.intent_id.local_communication_id);
        if (config.channel == WifiChannel::Default) {
            config.channel = WifiChannel::Wifi24_6; // as LANDiscovery picks
        }
        request.security_config = Blob<RL::SecurityConfigBlob>(security_config);
        request.user_config = Blob<RL::UserConfigBlob>(user_config);
        request.network_config = Blob<RL::NetworkConfigBlob>(config);
        advert = advertise_data;
        LOG_INFO(Service_LDN, "ryuldn: creating a lobby (id {:016X}, scene {}, {} players max)",
                 config.intent_id.local_communication_id, config.intent_id.scene_id,
                 config.node_count_max);
    }
    const RL::NetworkError error = backend->CreateNetwork(request, advert);
    if (error != RL::NetworkError::None) {
        LOG_WARNING(Service_LDN, "ryuldn: creating the lobby failed: {} {}",
                    RL::ErrorName(error), backend->client().LastError());
        return ResultAccessPointConnectionFailed;
    }
    {
        std::scoped_lock lk{mutex};
        disconnect_reason = DisconnectReason::None;
        SetStateLocked(State::AccessPointCreated);
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::DestroyNetwork() {
    backend->client().DisconnectNetwork();
    {
        std::scoped_lock lk{mutex};
        if (state == State::AccessPointCreated) {
            SetStateLocked(State::AccessPointOpened);
        }
        disconnect_reason = DisconnectReason::DestroyedByUser;
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::Connect(const NetworkInfo& network_info_, const UserConfig& user_config,
                              u16 local_communication_version) {
    RL::ConnectRequest request{};
    {
        std::scoped_lock lk{mutex};
        if (state != State::StationOpened) {
            return ResultBadState;
        }
        if (network_info_.ldn.node_count == 0) {
            return ResultInvalidNodeCount;
        }
        NetworkInfo target = network_info_;
        target.network_id.intent_id.local_communication_id =
            ResolveId(target.network_id.intent_id.local_communication_id);
        SecurityConfig security{};
        security.security_mode = target.ldn.security_mode;
        request.security_config = Blob<RL::SecurityConfigBlob>(security);
        request.user_config = Blob<RL::UserConfigBlob>(user_config);
        request.local_communication_version = local_communication_version;
        request.network_info = Blob<RL::NetworkInfoBlob>(target);
    }
    const RL::NetworkError error = backend->Connect(request);
    switch (error) {
    case RL::NetworkError::None:
        break;
    case RL::NetworkError::VersionTooLow:
        return ResultLocalCommunicationVersionTooLow;
    case RL::NetworkError::VersionTooHigh:
        return ResultLocalCommunicationVersionTooHigh;
    case RL::NetworkError::TooManyPlayers:
        return ResultMaximumNodeCount;
    default:
        LOG_WARNING(Service_LDN, "ryuldn: joining failed: {} {}", RL::ErrorName(error),
                    backend->client().LastError());
        return ResultConnectionFailed;
    }
    {
        std::scoped_lock lk{mutex};
        disconnect_reason = DisconnectReason::None;
        SetStateLocked(State::StationConnected);
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::Disconnect() {
    backend->client().DisconnectNetwork();
    {
        std::scoped_lock lk{mutex};
        if (state == State::StationConnected) {
            SetStateLocked(State::StationOpened);
        }
        disconnect_reason = DisconnectReason::DisconnectedByUser;
    }
    Signal();
    return ResultSuccess;
}

Result RyuLdnNetwork::SetStationAcceptPolicy(AcceptPolicy accept_policy) {
    bool created;
    {
        std::scoped_lock lk{mutex};
        created = state == State::AccessPointCreated;
    }
    if (created) {
        backend->client().SetAcceptPolicy(static_cast<u8>(accept_policy));
    }
    return ResultSuccess;
}

} // namespace Service::LDN
