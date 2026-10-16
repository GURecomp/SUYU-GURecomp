// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ldn/ldn_results.h"
#include "core/hle/service/ldn/ldn_types.h"
#include "core/hle/service/ldn/user_local_communication_service.h"
#include "core/hle/service/server_manager.h"
#include "core/internal_network/network.h"
#include "core/internal_network/network_interface.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/hle/service/ldn/hybrid_network.h"
#include "network/ryuldn/backend.h"
#include "network/network.h"

// This is defined by synchapi.h and conflicts with ServiceContext::CreateEvent
#undef CreateEvent

namespace Service::LDN {

IUserLocalCommunicationService::IUserLocalCommunicationService(Core::System& system_)
    : ServiceFramework{system_, "IUserLocalCommunicationService"},
      service_context{system, "IUserLocalCommunicationService"},
      ldn_backend{Network::GetLdnBackend()} {
    // Which LDN state machine serves this session follows the mode the menu set last (it can
    // change while the game runs; the next local-play session picks it up). Without the hybrid
    // backend (other frontends) a RyuLdn backend means RyuLdn only, none means the room.
    const auto hybrid_backend = Network::GetHybridLdnBackend();
    const auto ryu = Network::RyuLdn::GetActiveBackend();
    Network::LdnMode mode = ryu ? Network::LdnMode::RyuLdn : Network::LdnMode::Room;
    if (hybrid_backend) {
        mode = ryu ? hybrid_backend->GetMode() : Network::LdnMode::Room;
    }
    if (mode == Network::LdnMode::Auto && hybrid_backend) {
        auto hybrid = std::make_unique<HybridLdnNetwork>(hybrid_backend, ryu,
                                                         [this] { return ReadAppInfo(); });
        hybrid_network = hybrid.get();
        network = std::move(hybrid);
    } else if (mode == Network::LdnMode::RyuLdn) {
        network = std::make_unique<RyuLdnNetwork>(ryu, [this] { return ReadAppInfo(); });
    } else {
        auto lan = std::make_unique<LANDiscovery>();
        lan_discovery = lan.get();
        network = std::move(lan);
    }

    // clang-format off
        static const FunctionInfo functions[] = {
            {0, D<&IUserLocalCommunicationService::GetState>, "GetState"},
            {1, D<&IUserLocalCommunicationService::GetNetworkInfo>, "GetNetworkInfo"},
            {2, D<&IUserLocalCommunicationService::GetIpv4Address>, "GetIpv4Address"},
            {3, D<&IUserLocalCommunicationService::GetDisconnectReason>, "GetDisconnectReason"},
            {4, D<&IUserLocalCommunicationService::GetSecurityParameter>, "GetSecurityParameter"},
            {5, D<&IUserLocalCommunicationService::GetNetworkConfig>, "GetNetworkConfig"},
            {100, D<&IUserLocalCommunicationService::AttachStateChangeEvent>, "AttachStateChangeEvent"},
            {101, D<&IUserLocalCommunicationService::GetNetworkInfoLatestUpdate>, "GetNetworkInfoLatestUpdate"},
            {102, D<&IUserLocalCommunicationService::Scan>, "Scan"},
            {103, D<&IUserLocalCommunicationService::ScanPrivate>, "ScanPrivate"},
            {104, D<&IUserLocalCommunicationService::SetWirelessControllerRestriction>, "SetWirelessControllerRestriction"},
            { 106, D<&IUserLocalCommunicationService::SetProtocol>, "SetProtocol" },
            {200, D<&IUserLocalCommunicationService::OpenAccessPoint>, "OpenAccessPoint"},
            {201, D<&IUserLocalCommunicationService::CloseAccessPoint>, "CloseAccessPoint"},
            {202, D<&IUserLocalCommunicationService::CreateNetwork>, "CreateNetwork"},
            {203, D<&IUserLocalCommunicationService::CreateNetworkPrivate>, "CreateNetworkPrivate"},
            {204, D<&IUserLocalCommunicationService::DestroyNetwork>, "DestroyNetwork"},
            {205, nullptr, "Reject"},
            {206, D<&IUserLocalCommunicationService::SetAdvertiseData>, "SetAdvertiseData"},
            {207, D<&IUserLocalCommunicationService::SetStationAcceptPolicy>, "SetStationAcceptPolicy"},
            {208, D<&IUserLocalCommunicationService::AddAcceptFilterEntry>, "AddAcceptFilterEntry"},
            {209, nullptr, "ClearAcceptFilter"},
            {300, D<&IUserLocalCommunicationService::OpenStation>, "OpenStation"},
            {301, D<&IUserLocalCommunicationService::CloseStation>, "CloseStation"},
            {302, D<&IUserLocalCommunicationService::Connect>, "Connect"},
            {303, nullptr, "ConnectPrivate"},
            {304, D<&IUserLocalCommunicationService::Disconnect>, "Disconnect"},
            {400, D<&IUserLocalCommunicationService::Initialize>, "Initialize"},
            {401, D<&IUserLocalCommunicationService::Finalize>, "Finalize"},
            {402, D<&IUserLocalCommunicationService::Initialize2>, "Initialize2"},
        };
    // clang-format on

    RegisterHandlers(functions);

    state_change_event =
        service_context.CreateEvent("IUserLocalCommunicationService:StateChangeEvent");
}

IUserLocalCommunicationService::~IUserLocalCommunicationService() {
    StopHostingHook();
    if (is_initialized) {
        ldn_backend->Unbind(ldn_packet_received);
    }

    service_context.CloseEvent(state_change_event);
}

Result IUserLocalCommunicationService::GetState(Out<State> out_state) {
    *out_state = State::Error;

    if (is_initialized) {
        *out_state = network->GetState();
    }

    LOG_INFO(Service_LDN, "called, state={}", *out_state);

    R_SUCCEED();
}

Result IUserLocalCommunicationService::GetNetworkInfo(
    OutLargeData<NetworkInfo, BufferAttr_HipcPointer> out_network_info) {
    LOG_INFO(Service_LDN, "called");

    R_RETURN(network->GetNetworkInfo(*out_network_info));
}

Result IUserLocalCommunicationService::GetIpv4Address(Out<Ipv4Address> out_current_address,
                                                      Out<Ipv4Address> out_subnet_mask) {
    LOG_INFO(Service_LDN, "called");
    const auto network_interface = Network::GetSelectedNetworkInterface();

    R_UNLESS(network_interface.has_value(), ResultNoIpAddress);

    *out_current_address = {Network::TranslateIPv4(network_interface->ip_address)};
    *out_subnet_mask = {Network::TranslateIPv4(network_interface->subnet_mask)};

    // When we're connected to a room, spoof the hosts IP address
    if (ldn_backend->IsConnected()) {
        *out_current_address = ldn_backend->GetFakeIpAddress();
        if (const auto mask = ldn_backend->GetFakeSubnetMask()) {
            *out_subnet_mask = *mask;
        }
    }

    std::reverse(std::begin(*out_current_address), std::end(*out_current_address)); // ntohl
    std::reverse(std::begin(*out_subnet_mask), std::end(*out_subnet_mask));         // ntohl
    R_SUCCEED();
}

Result IUserLocalCommunicationService::GetDisconnectReason(
    Out<DisconnectReason> out_disconnect_reason) {
    LOG_INFO(Service_LDN, "called");

    *out_disconnect_reason = network->GetDisconnectReason();
    R_SUCCEED();
}

Result IUserLocalCommunicationService::GetSecurityParameter(
    Out<SecurityParameter> out_security_parameter) {
    LOG_INFO(Service_LDN, "called");

    NetworkInfo info{};
    R_TRY(network->GetNetworkInfo(info));

    out_security_parameter->session_id = info.network_id.session_id;
    std::memcpy(out_security_parameter->data.data(), info.ldn.security_parameter.data(),
                sizeof(SecurityParameter::data));
    R_SUCCEED();
}

Result IUserLocalCommunicationService::GetNetworkConfig(Out<NetworkConfig> out_network_config) {
    LOG_INFO(Service_LDN, "called");

    NetworkInfo info{};
    R_TRY(network->GetNetworkInfo(info));

    out_network_config->intent_id = info.network_id.intent_id;
    out_network_config->channel = info.common.channel;
    out_network_config->node_count_max = info.ldn.node_count_max;
    out_network_config->local_communication_version = info.ldn.nodes[0].local_communication_version;
    R_SUCCEED();
}

Result IUserLocalCommunicationService::AttachStateChangeEvent(
    OutCopyHandle<Kernel::KReadableEvent> out_event) {
    LOG_INFO(Service_LDN, "called");

    *out_event = &state_change_event->GetReadableEvent();
    R_SUCCEED();
}

Result IUserLocalCommunicationService::GetNetworkInfoLatestUpdate(
    OutLargeData<NetworkInfo, BufferAttr_HipcPointer> out_network_info,
    OutArray<NodeLatestUpdate, BufferAttr_HipcPointer> out_node_latest_update) {
    LOG_INFO(Service_LDN, "called");

    R_UNLESS(!out_node_latest_update.empty(), ResultBadInput);

    R_RETURN(network->GetNetworkInfo(*out_network_info, out_node_latest_update));
}

Result IUserLocalCommunicationService::Scan(
    Out<s16> network_count, WifiChannel channel, const ScanFilter& scan_filter,
    OutArray<NetworkInfo, BufferAttr_HipcAutoSelect> out_network_info) {
    LOG_INFO(Service_LDN, "called, channel={}, filter_scan_flag={}, filter_network_type={}",
             channel, scan_filter.flag, scan_filter.network_type);

    R_UNLESS(!out_network_info.empty(), ResultBadInput);
    R_RETURN(network->Scan(out_network_info, *network_count, scan_filter));
}

Result IUserLocalCommunicationService::ScanPrivate(
    Out<s16> network_count, WifiChannel channel, const ScanFilter& scan_filter,
    OutArray<NetworkInfo, BufferAttr_HipcAutoSelect> out_network_info) {
    LOG_INFO(Service_LDN, "called, channel={}, filter_scan_flag={}, filter_network_type={}",
             channel, scan_filter.flag, scan_filter.network_type);

    R_UNLESS(out_network_info.empty(), ResultBadInput);
    R_RETURN(network->Scan(out_network_info, *network_count, scan_filter));
}

Result IUserLocalCommunicationService::SetProtocol(u32 protocol) {
    LOG_WARNING(Service_LDN, "(STUBBED) called, protocol={}", protocol);
    R_SUCCEED();
}

Result IUserLocalCommunicationService::SetWirelessControllerRestriction(
    WirelessControllerRestriction wireless_restriction) {
    LOG_WARNING(Service_LDN, "(STUBBED) called");
    R_SUCCEED();
}

Result IUserLocalCommunicationService::OpenAccessPoint() {
    LOG_INFO(Service_LDN, "called");

    R_RETURN(network->OpenAccessPoint());
}

Result IUserLocalCommunicationService::CloseAccessPoint() {
    LOG_INFO(Service_LDN, "called");

    const Result rc = network->CloseAccessPoint();
    StopHostingHook();
    R_RETURN(rc);
}

Result IUserLocalCommunicationService::CreateNetwork(const CreateNetworkConfig& create_config) {
    LOG_INFO(Service_LDN, "called");

    StartHostingHook();
    const Result rc = network->CreateNetwork(create_config.security_config,
                                             create_config.user_config,
                                             create_config.network_config);
    if (rc.IsError()) {
        StopHostingHook();
    }
    R_RETURN(rc);
}

Result IUserLocalCommunicationService::CreateNetworkPrivate(
    const CreateNetworkConfigPrivate& create_config,
    InArray<AddressEntry, BufferAttr_HipcPointer> address_list) {
    LOG_INFO(Service_LDN, "called");

    StartHostingHook();
    const Result rc = network->CreateNetwork(create_config.security_config,
                                             create_config.user_config,
                                             create_config.network_config);
    if (rc.IsError()) {
        StopHostingHook();
    }
    R_RETURN(rc);
}

Result IUserLocalCommunicationService::DestroyNetwork() {
    LOG_INFO(Service_LDN, "called");

    const Result rc = network->DestroyNetwork();
    StopHostingHook();
    R_RETURN(rc);
}

Result IUserLocalCommunicationService::SetAdvertiseData(
    InBuffer<BufferAttr_HipcAutoSelect> buffer_data) {
    LOG_INFO(Service_LDN, "called");

    R_RETURN(network->SetAdvertiseData(buffer_data));
}

Result IUserLocalCommunicationService::SetStationAcceptPolicy(AcceptPolicy accept_policy) {
    LOG_WARNING(Service_LDN, "(STUBBED) called");
    R_RETURN(network->SetStationAcceptPolicy(accept_policy));
}

Result IUserLocalCommunicationService::AddAcceptFilterEntry(MacAddress mac_address) {
    LOG_WARNING(Service_LDN, "(STUBBED) called");
    R_SUCCEED();
}

Result IUserLocalCommunicationService::OpenStation() {
    LOG_INFO(Service_LDN, "called");

    R_RETURN(network->OpenStation());
}

Result IUserLocalCommunicationService::CloseStation() {
    LOG_INFO(Service_LDN, "called");

    R_RETURN(network->CloseStation());
}

Result IUserLocalCommunicationService::Connect(
    const ConnectNetworkData& connect_data,
    InLargeData<NetworkInfo, BufferAttr_HipcPointer> network_info) {
    LOG_INFO(Service_LDN,
             "called, passphrase_size={}, security_mode={}, "
             "local_communication_version={}",
             connect_data.security_config.passphrase_size,
             connect_data.security_config.security_mode, connect_data.local_communication_version);

    R_RETURN(network->Connect(*network_info, connect_data.user_config,
                                   static_cast<u16>(connect_data.local_communication_version)));
}

Result IUserLocalCommunicationService::Disconnect() {
    LOG_INFO(Service_LDN, "called");

    R_RETURN(network->Disconnect());
}

Result IUserLocalCommunicationService::Initialize(ClientProcessId aruid) {
    LOG_INFO(Service_LDN, "called, process_id={}", aruid.pid);

    const auto network_interface = Network::GetSelectedNetworkInterface();
    R_UNLESS(network_interface, ResultAirplaneModeEnabled);

    if (lan_discovery || hybrid_network) {
        ldn_packet_received = ldn_backend->BindOnLdnPacketReceived(
            [this](const Network::LDNPacket& packet) { OnLDNPacketReceived(packet); });
        if (!ldn_packet_received) {
            LOG_ERROR(Service_LDN, "Couldn't bind callback!");
            R_RETURN(ResultAirplaneModeEnabled);
        }
    }

    network->Initialize([&]() { OnEventFired(); });
    is_initialized = true;
    R_SUCCEED();
}

Result IUserLocalCommunicationService::Finalize() {
    LOG_INFO(Service_LDN, "called");
    ldn_backend->Unbind(ldn_packet_received);

    is_initialized = false;

    const Result rc = network->Finalize();
    StopHostingHook();
    R_RETURN(rc);
}

Result IUserLocalCommunicationService::Initialize2(u32 version, ClientProcessId process_id) {
    LOG_INFO(Service_LDN, "called, version={}, process_id={}", version, process_id.pid);
    R_RETURN(Initialize(process_id));
}

void IUserLocalCommunicationService::OnLDNPacketReceived(const Network::LDNPacket& packet) {
    if (hybrid_network) {
        hybrid_network->ReceivePacket(packet);
    } else if (lan_discovery) {
        lan_discovery->ReceivePacket(packet);
    }
}

void IUserLocalCommunicationService::StartHostingHook() {
    // The hybrid network runs the hook itself (it decides between server and room first).
    if (!hybrid_network && !hosting_hooked) {
        hosting_hooked = true;
        Network::RunHostingHook(true);
    }
}

void IUserLocalCommunicationService::StopHostingHook() {
    if (hosting_hooked) {
        hosting_hooked = false;
        Network::RunHostingHook(false);
    }
}

RyuLdnNetwork::AppInfo IUserLocalCommunicationService::ReadAppInfo() const {
    RyuLdnNetwork::AppInfo info{system.GetApplicationProcessProgramID(), ""};
    FileSys::NACP nacp;
    bool have_nacp = system.GetAppLoader().ReadControlData(nacp) == Loader::ResultStatus::Success;
    if (!have_nacp) {
        const FileSys::PatchManager pm{system.GetApplicationProcessProgramID(),
                                       system.GetFileSystemController(),
                                       system.GetContentProvider()};
        if (const auto control = pm.GetControlMetadata().first) {
            nacp = *control;
            have_nacp = true;
        }
    }
    if (have_nacp) {
        if (const u64 id = nacp.GetLocalCommunicationId(); id != 0) {
            info.local_communication_id = id;
        }
        info.display_version = nacp.GetVersionString();
    } else {
        LOG_WARNING(Service_LDN, "ryuldn: no control data; local communication id = program id");
    }
    return info;
}

void IUserLocalCommunicationService::OnEventFired() {
    state_change_event->Signal(system.Kernel());
}

} // namespace Service::LDN
