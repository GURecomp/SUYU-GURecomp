// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "network/ryuldn/backend.h"
#include "network/room_member.h"

namespace Network::RyuLdn {

/**
 * Puts suyu room players into the RyuLdn lobby this game hosts, so players from both worlds share
 * one lobby: this process is the translation layer between the two.
 *
 * The host game's lobby lives on a RyuLdn server (Backend::CreateNetwork). While it is open and
 * this process is also in a suyu room (the room it hosts, usually), the bridge
 *  - answers the room's LDN scans with that lobby, as a suyu host would (LANDiscovery's wire
 *    protocol: Scan/ScanResp, Connect, SyncNetwork, Disconnect, DestroyNetwork),
 *  - joins every suyu player that connects to the lobby as a normal RyuLdn client of the same
 *    server (their own session, so the server, Ryujinx players and the host game see an ordinary
 *    player with an address in 10.114.0.0/16),
 *  - translates the game traffic between the two address spaces. Suyu players keep seeing room
 *    addresses (192.168.x.y): the host is the bridge's room address, other suyu players are
 *    themselves, and each Ryujinx player gets a stand-in address 192.168.250.<slot>, which the
 *    room hands to the host's member (Room::SetHostGateway) because no member owns it.
 * Traffic between two suyu players never goes through the bridge (the room delivers it itself).
 *
 * Costs nothing while no lobby is hosted or nobody in the room scans.
 */
class RoomBridge {
public:
    explicit RoomBridge(std::shared_ptr<Backend> ryuldn);
    ~RoomBridge();

    RoomBridge(const RoomBridge&) = delete;
    RoomBridge& operator=(const RoomBridge&) = delete;

    struct Status {
        bool active = false;           // a lobby is hosted and the room can see it
        std::size_t players = 0;       // suyu players in the lobby through the bridge
        std::vector<std::string> names; // their lobby names
    };
    Status GetStatus() const;

    /// Stand-in address of the RyuLdn player in lobby slot n (1..7) as suyu players see it.
    static IPv4Address AliasAddress(u8 slot);

private:
    struct Peer;
    using Clock = std::chrono::steady_clock;

    // Room side (room thread).
    void OnLdnPacket(const LDNPacket& packet);
    void OnRoomProxy(const ProxyPacket& packet);
    void OnRoomInformation();

    // RyuLdn side (reader threads of the peers' clients / the host's client).
    void OnLobby(const NetworkInfoBlob& info, bool connected);
    void OnPeerNetwork(Peer* peer, const NetworkInfoBlob& info, bool connected);
    void OnPeerProxy(Peer* peer, const ProxyDataHeader& header, std::span<const u8> data);

    // Worker.
    void Run();
    void Tick();
    void Post(std::function<void()> task);
    void BindToRoom();
    void DoConnect(IPv4Address room_ip, std::vector<u8> node);
    void DropPeer(const IPv4Address& room_ip, bool tell_player);
    void DropAll(bool tell_players);
    void Retire(std::shared_ptr<Peer> peer);

    // Helpers.
    std::shared_ptr<Peer> FindPeer(const IPv4Address& room_ip) const;
    void UpdateTable(const NetworkInfoBlob& info);
    NetworkInfoBlob RoomView(const NetworkInfoBlob& info) const;
    /// Rewrites the addresses (and MACs) a game writes into its own datagrams from one side's
    /// view to the other's; returns how many it changed.
    std::size_t Translate(std::vector<u8>& data, const Peer& peer, bool to_lobby) const;
    void SendLdn(LDNPacketType type, const IPv4Address& to, std::span<const u8> data);
    void PushSync(Peer& peer, const NetworkInfoBlob& info);
    Client::Settings PeerSettings() const;

    const std::shared_ptr<Backend> ryuldn;
    Backend::ObserverHandle observer;

    std::atomic<bool> running{true};
    std::atomic<bool> hosting{false}; // refreshed by the worker and the lobby observer
    std::thread worker;
    std::mutex task_lock;
    std::condition_variable task_cv;
    std::deque<std::function<void()>> tasks;

    mutable std::mutex lock; // peers, table, members
    std::vector<std::shared_ptr<Peer>> peers;
    std::vector<std::shared_ptr<Peer>> graveyard; // closed peers, freed by the worker
    std::array<u32, 8> node_ip{};                  // the lobby's slots as the host sees them
    std::array<bool, 8> node_live{};
    std::array<std::array<u8, 6>, 8> node_mac{};
    std::set<u32> room_members; // fake addresses in the room (host and bridge included)

    std::weak_ptr<RoomMember> bound_member;
    RoomMember::CallbackHandle<LDNPacket> ldn_handle;
    RoomMember::CallbackHandle<ProxyPacket> proxy_handle;
    RoomMember::CallbackHandle<RoomInformation> info_handle;
    Clock::time_point last_tick{};
};

} // namespace Network::RyuLdn
