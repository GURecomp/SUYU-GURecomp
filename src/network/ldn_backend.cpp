// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>

#include "common/zstd_compression.h"
#include "network/ldn_backend.h"
#include "network/network.h"

namespace Network {

namespace {
std::mutex g_backend_lock;
std::shared_ptr<LdnBackend> g_backend;
std::shared_ptr<HybridLdnBackend> g_hybrid;
std::function<void(bool)> g_hosting_hook;
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
    g_hybrid.reset();
    g_backend = std::move(backend);
}

void SetHostingHook(std::function<void(bool)> hook) {
    std::scoped_lock lk{g_backend_lock};
    g_hosting_hook = std::move(hook);
}

void RunHostingHook(bool hosting) {
    std::function<void(bool)> hook;
    {
        std::scoped_lock lk{g_backend_lock};
        hook = g_hosting_hook;
    }
    if (hook) {
        hook(hosting);
    }
}

std::shared_ptr<HybridLdnBackend> GetHybridLdnBackend() {
    std::scoped_lock lk{g_backend_lock};
    return g_hybrid;
}

void SetHybridLdnBackend(std::shared_ptr<HybridLdnBackend> hybrid) {
    std::scoped_lock lk{g_backend_lock};
    g_hybrid = hybrid;
    g_backend = std::move(hybrid);
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

HybridLdnBackend::HybridLdnBackend(std::shared_ptr<LdnBackend> room_,
                                  std::shared_ptr<LdnBackend> ryuldn_)
    : room{std::move(room_)}, ryuldn{std::move(ryuldn_)} {}

void HybridLdnBackend::SetMode(LdnMode new_mode) {
    mode = new_mode;
}

LdnMode HybridLdnBackend::GetMode() const {
    return mode.load();
}

void HybridLdnBackend::SetActive(LdnTransport transport) {
    active = transport;
}

LdnTransport HybridLdnBackend::GetActive() const {
    return active.load();
}

LdnTransport HybridLdnBackend::Resolve() const {
    switch (mode.load()) {
    case LdnMode::Room:
        return LdnTransport::Room;
    case LdnMode::RyuLdn:
        return ryuldn ? LdnTransport::RyuLdn : LdnTransport::Room;
    case LdnMode::Auto:
        break;
    }
    const LdnTransport chosen = active.load();
    if (chosen == LdnTransport::RyuLdn && ryuldn) {
        return LdnTransport::RyuLdn;
    }
    if (chosen == LdnTransport::Room) {
        return LdnTransport::Room;
    }
    return ryuldn && ryuldn->IsConnected() ? LdnTransport::RyuLdn : LdnTransport::Room;
}

LdnBackend& HybridLdnBackend::Backend(LdnTransport transport) const {
    return transport == LdnTransport::RyuLdn && ryuldn ? *ryuldn : *room;
}

bool HybridLdnBackend::IsConnected() const {
    return Backend(Resolve()).IsConnected();
}

IPv4Address HybridLdnBackend::GetFakeIpAddress() const {
    return Backend(Resolve()).GetFakeIpAddress();
}

std::optional<IPv4Address> HybridLdnBackend::GetFakeSubnetMask() const {
    return Backend(Resolve()).GetFakeSubnetMask();
}

void HybridLdnBackend::SendProxyPacket(const ProxyPacket& packet) {
    Backend(Resolve()).SendProxyPacket(packet);
}

void HybridLdnBackend::SendLdnPacket(const LDNPacket& packet) {
    room->SendLdnPacket(packet); // the RyuLdn side has no LDN packets
}

LdnBackend::CallbackHandle<ProxyPacket> HybridLdnBackend::BindOnProxyPacketReceived(
    std::function<void(const ProxyPacket&)> callback) {
    Binding binding;
    binding.outer =
        std::make_shared<std::function<void(const ProxyPacket&)>>(std::move(callback));
    const auto outer = binding.outer;
    binding.room = room->BindOnProxyPacketReceived([this, outer](const ProxyPacket& packet) {
        if (Resolve() == LdnTransport::Room) {
            (*outer)(packet);
        }
    });
    if (ryuldn) {
        binding.ryuldn = ryuldn->BindOnProxyPacketReceived([this, outer](const ProxyPacket& packet) {
            if (Resolve() == LdnTransport::RyuLdn) {
                (*outer)(packet);
            }
        });
    }
    std::scoped_lock lk{bindings_lock};
    bindings.push_back(binding);
    return outer;
}

LdnBackend::CallbackHandle<LDNPacket> HybridLdnBackend::BindOnLdnPacketReceived(
    std::function<void(const LDNPacket&)> callback) {
    return room->BindOnLdnPacketReceived(std::move(callback));
}

void HybridLdnBackend::Unbind(CallbackHandle<ProxyPacket> handle) {
    Binding found;
    {
        std::scoped_lock lk{bindings_lock};
        const auto it = std::find_if(bindings.begin(), bindings.end(),
                                     [&](const Binding& b) { return b.outer == handle; });
        if (it == bindings.end()) {
            return;
        }
        found = *it;
        bindings.erase(it);
    }
    if (found.room) {
        room->Unbind(found.room);
    }
    if (found.ryuldn && ryuldn) {
        ryuldn->Unbind(found.ryuldn);
    }
}

void HybridLdnBackend::Unbind(CallbackHandle<LDNPacket> handle) {
    room->Unbind(std::move(handle));
}

} // namespace Network
