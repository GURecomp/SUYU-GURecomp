// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <mutex>

#include "common/zstd_compression.h"
#include "network/ldn_backend.h"
#include "network/network.h"

namespace Network {

namespace {
std::mutex g_backend_lock;
std::shared_ptr<LdnBackend> g_backend;
} // namespace

std::shared_ptr<LdnBackend> GetLdnBackend() {
    std::scoped_lock lk{g_backend_lock};
    if (!g_backend) {
        g_backend = std::make_shared<RoomLdnBackend>();
    }
    return g_backend;
}

void SetLdnBackend(std::shared_ptr<LdnBackend> backend) {
    std::scoped_lock lk{g_backend_lock};
    g_backend = std::move(backend);
}

bool RoomLdnBackend::IsConnected() const {
    const auto room_member = GetRoomMember().lock();
    return room_member && room_member->IsConnected();
}

IPv4Address RoomLdnBackend::GetFakeIpAddress() const {
    if (const auto room_member = GetRoomMember().lock()) {
        return room_member->GetFakeIpAddress();
    }
    return {};
}

void RoomLdnBackend::SendProxyPacket(const ProxyPacket& packet) {
    if (const auto room_member = GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            ProxyPacket compressed = packet;
            compressed.data =
                Common::Compression::CompressDataZSTDDefault(packet.data.data(), packet.data.size());
            room_member->SendProxyPacket(compressed);
        }
    }
}

void RoomLdnBackend::SendLdnPacket(const LDNPacket& packet) {
    if (const auto room_member = GetRoomMember().lock()) {
        if (room_member->IsConnected()) {
            room_member->SendLdnPacket(packet);
        }
    }
}

LdnBackend::CallbackHandle<ProxyPacket> RoomLdnBackend::BindOnProxyPacketReceived(
    std::function<void(const ProxyPacket&)> callback) {
    if (const auto room_member = GetRoomMember().lock()) {
        return room_member->BindOnProxyPacketReceived(
            [callback = std::move(callback)](const ProxyPacket& packet) {
                ProxyPacket decompressed = packet;
                decompressed.data = Common::Compression::DecompressDataZSTD(packet.data);
                callback(decompressed);
            });
    }
    return nullptr;
}

LdnBackend::CallbackHandle<LDNPacket> RoomLdnBackend::BindOnLdnPacketReceived(
    std::function<void(const LDNPacket&)> callback) {
    if (const auto room_member = GetRoomMember().lock()) {
        return room_member->BindOnLdnPacketReceived(std::move(callback));
    }
    return nullptr;
}

void RoomLdnBackend::Unbind(CallbackHandle<ProxyPacket> handle) {
    if (const auto room_member = GetRoomMember().lock()) {
        room_member->Unbind(std::move(handle));
    }
}

void RoomLdnBackend::Unbind(CallbackHandle<LDNPacket> handle) {
    if (const auto room_member = GetRoomMember().lock()) {
        room_member->Unbind(std::move(handle));
    }
}

} // namespace Network
