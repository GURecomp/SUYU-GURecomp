// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "common/socket_types.h"
#include "network/room_member.h"

namespace Network {

/**
 * The transport under local wireless (LDN) play: the guest's proxied sockets (data plane) and the
 * node's address in the emulated network. The room backend (default) goes through a suyu room;
 * another backend (RyuLdn) can replace it for a whole run. Packet payloads are the game's own
 * bytes here; a backend that compresses on the wire does so itself.
 */
class LdnBackend {
public:
    template <typename T>
    using CallbackHandle = std::shared_ptr<std::function<void(const T&)>>;

    virtual ~LdnBackend() = default;

    /// The data plane can carry packets (room joined / in a network).
    virtual bool IsConnected() const = 0;

    /// This node's address in the emulated network (valid while IsConnected()).
    virtual IPv4Address GetFakeIpAddress() const = 0;

    /// The emulated network's mask when the backend defines one (none: keep the host's).
    virtual std::optional<IPv4Address> GetFakeSubnetMask() const {
        return std::nullopt;
    }

    virtual void SendProxyPacket(const ProxyPacket& packet) = 0;

    /// The room's own LDN discovery packets (room backend only; others ignore them).
    virtual void SendLdnPacket(const LDNPacket& packet) = 0;

    /// Null when the backend can't deliver packets of that kind (callers treat it as unbound).
    virtual CallbackHandle<ProxyPacket> BindOnProxyPacketReceived(
        std::function<void(const ProxyPacket&)> callback) = 0;
    virtual CallbackHandle<LDNPacket> BindOnLdnPacketReceived(
        std::function<void(const LDNPacket&)> callback) = 0;

    virtual void Unbind(CallbackHandle<ProxyPacket> handle) = 0;
    virtual void Unbind(CallbackHandle<LDNPacket> handle) = 0;
};

/// The process's LDN backend: the room adapter unless SetLdnBackend chose another one.
std::shared_ptr<LdnBackend> GetLdnBackend();

/// Replaces the backend; call once at start, before the game opens sockets.
void SetLdnBackend(std::shared_ptr<LdnBackend> backend);

/// Room adapter: forwards to Network::GetRoomMember(); proxy payloads are zstd-compressed on the
/// wire as before.
class RoomLdnBackend final : public LdnBackend {
public:
    bool IsConnected() const override;
    IPv4Address GetFakeIpAddress() const override;
    void SendProxyPacket(const ProxyPacket& packet) override;
    void SendLdnPacket(const LDNPacket& packet) override;
    CallbackHandle<ProxyPacket> BindOnProxyPacketReceived(
        std::function<void(const ProxyPacket&)> callback) override;
    CallbackHandle<LDNPacket> BindOnLdnPacketReceived(
        std::function<void(const LDNPacket&)> callback) override;
    void Unbind(CallbackHandle<ProxyPacket> handle) override;
    void Unbind(CallbackHandle<LDNPacket> handle) override;
};

} // namespace Network
