// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

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

/// Which transports local play may use. Room: suyu rooms only; RyuLdn: RyuLdn servers only; Auto:
/// both at once (lobbies of both are listed, a hosted lobby is open to both, the bridge links
/// suyu room players and RyuLdn players in one lobby).
enum class LdnMode : u8 { Room, RyuLdn, Auto };

/// The transport carrying the lobby the game is in.
enum class LdnTransport : u8 { None, Room, RyuLdn };

/**
 * The process's LDN backend once the exported game's multiplayer is set up: the room adapter and
 * a RyuLdn backend side by side, and a mode that can change while the game runs. The data plane
 * (proxied sockets) follows Resolve(): the transport of the lobby the LDN state machine joined or
 * created, else whichever is connected. Packets from the transport that isn't in use are not
 * handed to the game (the bridge reads the room's packets itself).
 */
class HybridLdnBackend final : public LdnBackend {
public:
    HybridLdnBackend(std::shared_ptr<LdnBackend> room, std::shared_ptr<LdnBackend> ryuldn);

    void SetMode(LdnMode new_mode);
    LdnMode GetMode() const;

    /// Set by the LDN state machine when it hosts/joins a lobby, None when it leaves.
    void SetActive(LdnTransport transport);
    LdnTransport GetActive() const;

    /// The transport the data plane uses right now.
    LdnTransport Resolve() const;

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

private:
    LdnBackend& Backend(LdnTransport transport) const;

    struct Binding {
        CallbackHandle<ProxyPacket> outer;
        CallbackHandle<ProxyPacket> room;
        CallbackHandle<ProxyPacket> ryuldn;
    };

    const std::shared_ptr<LdnBackend> room;
    const std::shared_ptr<LdnBackend> ryuldn;
    std::atomic<LdnMode> mode{LdnMode::Room};
    std::atomic<LdnTransport> active{LdnTransport::None};
    std::mutex bindings_lock;
    std::vector<Binding> bindings;
};

/// Called by the LDN state machine when the game starts hosting a lobby (true, before the lobby is
/// created: open the room and the server for friends) and when it stops (false). Set by the
/// frontend; none = nothing to do.
void SetHostingHook(std::function<void(bool hosting)> hook);
void RunHostingHook(bool hosting);

/// The hybrid backend when SetHybridLdnBackend installed one (the exported game), else null.
std::shared_ptr<HybridLdnBackend> GetHybridLdnBackend();

/// Installs hybrid as the process's LDN backend; call once at start, before the game opens sockets.
void SetHybridLdnBackend(std::shared_ptr<HybridLdnBackend> hybrid);

} // namespace Network
