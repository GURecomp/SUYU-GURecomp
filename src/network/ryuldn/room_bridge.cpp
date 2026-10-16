// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "common/zstd_compression.h"
#include "network/network.h"
#include "network/ryuldn/room_bridge.h"

namespace Network::RyuLdn {

namespace {

// Offsets inside the Nintendo LDN structs the server and LANDiscovery share (see server.cpp).
namespace Info {
constexpr std::size_t SecurityMode = 0x60, AcceptPolicy = 0x62, NodeCountMax = 0x66,
                      NodeCount = 0x67, Nodes = 0x68;
}
namespace NodeOff {
constexpr std::size_t Size = 0x40, Ip = 0x00, Connected = 0x0B, Name = 0x0C, NameSize = 0x21,
                      Lcv = 0x2E;
}

constexpr std::size_t kMaxPeers = 7;
constexpr auto kIdlePeer = std::chrono::seconds(60);
constexpr auto kTickEvery = std::chrono::milliseconds(250);

u32 ToU32(const IPv4Address& ip) {
    return Backend::ToU32(ip);
}

std::string NodeName(std::span<const u8> node) {
    std::string name(reinterpret_cast<const char*>(node.data() + NodeOff::Name), NodeOff::NameSize);
    name.resize(std::strlen(name.c_str()));
    return name;
}

struct Subst {
    std::array<u8, 6> from{};
    std::array<u8, 6> to{};
    std::size_t length = 0;
};

/// An IPv4 address as the game may write it into its own data: network order or reversed.
void AddIp(std::vector<Subst>& subs, u32 from, u32 to) {
    if (from == 0 || to == 0 || from == to) {
        return;
    }
    Subst be;
    be.length = 4;
    for (int i = 0; i < 4; i++) {
        be.from[i] = static_cast<u8>(from >> (24 - 8 * i));
        be.to[i] = static_cast<u8>(to >> (24 - 8 * i));
    }
    Subst le = be;
    for (int i = 0; i < 4; i++) {
        le.from[i] = be.from[3 - i];
        le.to[i] = be.to[3 - i];
    }
    subs.push_back(be);
    subs.push_back(le);
}

void AddMac(std::vector<Subst>& subs, const std::array<u8, 6>& from, const std::array<u8, 6>& to) {
    if (from != to && from != std::array<u8, 6>{} && to != std::array<u8, 6>{}) {
        subs.push_back({from, to, 6});
    }
}

std::string Hex(std::span<const u8> data, std::size_t max = 72) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < data.size() && i < max; i++) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
        if (i % 4 == 3) {
            out += ' ';
        }
    }
    return out;
}

} // namespace

struct RoomBridge::Peer {
    IPv4Address room_ip{};
    std::string name;
    std::unique_ptr<Client> client;
    std::atomic<u32> ryu_ip{0}; // address in the lobby's network (from the server)
    std::atomic<u32> mask{0xFFFF0000};
    std::atomic<bool> joined{false};
    std::atomic<u32> from_room{0}; // datagrams relayed room -> lobby / lobby -> room (logging)
    std::atomic<u32> to_room{0};
    std::atomic<bool> raw{false}; // its proxy payloads aren't zstd-compressed (older/other builds)
    std::array<u8, 6> mac{};      // the MAC its own LDN code uses for itself
    NetworkInfoBlob last_info{}; // under RoomBridge::lock
    Clock::time_point created = Clock::now();
};

IPv4Address RoomBridge::AliasAddress(u8 slot) {
    return {192, 168, 250, slot};
}

RoomBridge::RoomBridge(std::shared_ptr<Backend> ryuldn_) : ryuldn{std::move(ryuldn_)} {
    observer = ryuldn->AddNetworkObserver(
        [this](const NetworkInfoBlob& info, bool connected) { OnLobby(info, connected); });
    worker = std::thread([this] { Run(); });
}

RoomBridge::~RoomBridge() {
    ryuldn->RemoveNetworkObserver(observer);
    running = false;
    task_cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    if (const auto member = bound_member.lock()) {
        member->Unbind(ldn_handle);
        member->Unbind(proxy_handle);
        member->Unbind(info_handle);
    }
    DropAll(false);
    std::vector<std::shared_ptr<Peer>> dead;
    {
        std::scoped_lock lk{lock};
        dead.swap(graveyard);
    }
}

RoomBridge::Status RoomBridge::GetStatus() const {
    Status status;
    status.active = hosting.load();
    std::scoped_lock lk{lock};
    for (const auto& peer : peers) {
        if (peer->joined.load()) {
            status.players++;
            status.names.push_back(peer->name);
        }
    }
    return status;
}

// ── worker ──

void RoomBridge::Post(std::function<void()> task) {
    {
        std::scoped_lock lk{task_lock};
        tasks.push_back(std::move(task));
    }
    task_cv.notify_one();
}

void RoomBridge::Run() {
    while (running.load()) {
        std::function<void()> task;
        {
            std::unique_lock lk{task_lock};
            task_cv.wait_for(lk, kTickEvery, [&] { return !tasks.empty() || !running.load(); });
            if (!running.load()) {
                return;
            }
            if (!tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop_front();
            }
        }
        if (task) {
            task();
        }
        if (Clock::now() - last_tick >= kTickEvery) {
            last_tick = Clock::now();
            Tick();
        }
    }
}

void RoomBridge::BindToRoom() {
    const auto member = GetRoomMember().lock();
    if (!member || member == bound_member.lock()) {
        return;
    }
    if (const auto old = bound_member.lock()) {
        old->Unbind(ldn_handle);
        old->Unbind(proxy_handle);
        old->Unbind(info_handle);
    }
    ldn_handle = member->BindOnLdnPacketReceived(
        [this](const LDNPacket& packet) { OnLdnPacket(packet); });
    proxy_handle = member->BindOnProxyPacketReceived(
        [this](const ProxyPacket& packet) { OnRoomProxy(packet); });
    info_handle = member->BindOnRoomInformationChanged(
        [this](const RoomInformation&) { OnRoomInformation(); });
    bound_member = member;
    OnRoomInformation(); // the members known so far
}

void RoomBridge::Tick() {
    BindToRoom();

    const bool now = ryuldn->IsHosting();
    if (hosting.load() && !now) {
        DropAll(true);
    }
    hosting = now;

    const auto member = GetRoomMember().lock();
    const bool in_room = member && member->IsConnected();

    std::vector<IPv4Address> gone;
    std::vector<std::shared_ptr<Peer>> dead;
    {
        std::scoped_lock lk{lock};
        dead.swap(graveyard); // freed below, outside the lock (closed already)
        for (const auto& peer : peers) {
            const bool left =
                !in_room || (!room_members.empty() && !room_members.contains(ToU32(peer->room_ip)));
            const bool stale =
                !peer->joined.load() && Clock::now() - peer->created > kIdlePeer;
            if (left || stale) {
                gone.push_back(peer->room_ip);
            }
        }
    }
    for (const auto& ip : gone) {
        LOG_INFO(Network, "ryuldn bridge: {}.{}.{}.{} left the room", ip[0], ip[1], ip[2], ip[3]);
        DropPeer(ip, false);
    }
}

// ── room side ──

void RoomBridge::OnRoomInformation() {
    const auto member = GetRoomMember().lock();
    if (!member) {
        return;
    }
    std::set<u32> ips;
    for (const auto& m : member->GetMemberInformation()) {
        ips.insert(ToU32(m.fake_ip));
    }
    std::scoped_lock lk{lock};
    room_members = std::move(ips);
}

void RoomBridge::OnLdnPacket(const LDNPacket& packet) {
    switch (packet.type) {
    case LDNPacketType::Scan: {
        if (!ryuldn->IsHosting()) {
            return;
        }
        const NetworkInfoBlob info = ryuldn->LobbyInfo();
        if (info[Info::NodeCount] == 0 || info[Info::AcceptPolicy] == 1) {
            return; // not open to new players
        }
        const NetworkInfoBlob view = RoomView(info);
        SendLdn(LDNPacketType::ScanResp, packet.local_ip, view);
        break;
    }
    case LDNPacketType::Connect:
        if (hosting.load() && packet.data.size() >= NodeOff::Size) {
            Post([this, ip = packet.local_ip, node = packet.data] { DoConnect(ip, node); });
        }
        break;
    case LDNPacketType::Disconnect:
        Post([this, ip = packet.local_ip] { DropPeer(ip, false); });
        break;
    default:
        break;
    }
}

void RoomBridge::OnRoomProxy(const ProxyPacket& packet) {
    if (!hosting.load()) {
        return;
    }
    const auto peer = FindPeer(packet.local_endpoint.ip);
    if (!peer || !peer->joined.load()) {
        static std::atomic<u32> unknown{0};
        if (!peer && unknown.fetch_add(1) < 5 && packet.remote_endpoint.ip[0] != 0) {
            LOG_INFO(Network, "ryuldn bridge: datagram from {}.{}.{}.{} isn't a bridged player's",
                     packet.local_endpoint.ip[0], packet.local_endpoint.ip[1],
                     packet.local_endpoint.ip[2], packet.local_endpoint.ip[3]);
        }
        return; // not one of the lobby's suyu players (their traffic among themselves is the
                // room's business)
    }
    const u32 own = peer->ryu_ip.load();
    if (own == 0) {
        return;
    }
    ProxyInfo info{};
    info.source_ip = own;
    info.source_port = packet.local_endpoint.portno;
    info.dest_port = packet.remote_endpoint.portno;
    info.protocol = packet.protocol == Protocol::TCP ? ProtocolTcp : ProtocolUdp;
    if (packet.broadcast) {
        info.dest_ip = own | ~peer->mask.load();
    } else {
        const auto member = GetRoomMember().lock();
        if (!member) {
            return;
        }
        const IPv4Address& to = packet.remote_endpoint.ip;
        u32 dest = 0;
        {
            std::scoped_lock lk{lock};
            if (to == member->GetFakeIpAddress()) {
                dest = node_live[0] ? node_ip[0] : 0; // the host game
            } else if (to[0] == 192 && to[1] == 168 && to[2] == 250 && to[3] >= 1 && to[3] <= 7) {
                dest = node_live[to[3]] ? node_ip[to[3]] : 0; // a RyuLdn player
            }
        }
        if (dest == 0) {
            LOG_WARNING(Network, "ryuldn bridge: {} sent to {}.{}.{}.{}, which isn't in the lobby",
                        peer->name, to[0], to[1], to[2], to[3]);
            return;
        }
        info.dest_ip = dest;
    }
    // Room builds compress the payload with zstd; tolerate one that doesn't (and answer it alike).
    const bool zstd = packet.data.size() >= 4 && packet.data[0] == 0x28 && packet.data[1] == 0xB5 &&
                      packet.data[2] == 0x2F && packet.data[3] == 0xFD;
    peer->raw = !zstd;
    if (peer->from_room.fetch_add(1) < 6) {
        LOG_INFO(Network, "ryuldn bridge: {} -> lobby: {} bytes, {}:{} -> {}.{}.{}.{}:{}{}{}",
                 peer->name, packet.data.size(), packet.local_endpoint.ip[3],
                 packet.local_endpoint.portno, packet.remote_endpoint.ip[0],
                 packet.remote_endpoint.ip[1], packet.remote_endpoint.ip[2],
                 packet.remote_endpoint.ip[3], packet.remote_endpoint.portno,
                 packet.broadcast ? " (broadcast)" : "", zstd ? "" : " (not compressed)");
    }
    std::vector<u8> data =
        zstd ? Common::Compression::DecompressDataZSTD(packet.data) : packet.data;
    const std::size_t changed = Translate(data, *peer, true);
    if (peer->from_room.load() <= 6) {
        LOG_INFO(Network, "ryuldn bridge: payload {} ({} address{} rewritten): {}", data.size(),
                 changed, changed == 1 ? "" : "es", Hex(data));
    }
    peer->client->SendProxyData(info, data);
}

// ── RyuLdn side ──

void RoomBridge::UpdateTable(const NetworkInfoBlob& info) {
    std::scoped_lock lk{lock};
    for (std::size_t i = 0; i < 8; i++) {
        const u8* node = info.data() + Info::Nodes + i * NodeOff::Size;
        node_live[i] = node[NodeOff::Connected] != 0;
        std::memcpy(&node_ip[i], node + NodeOff::Ip, 4);
        std::memcpy(node_mac[i].data(), node + 4, 6);
    }
}

void RoomBridge::OnLobby(const NetworkInfoBlob& info, bool connected) {
    if (connected) {
        UpdateTable(info);
        return;
    }
    {
        std::scoped_lock lk{lock};
        node_live.fill(false);
    }
    hosting = false;
    Post([this] { DropAll(true); });
}

void RoomBridge::OnPeerNetwork(Peer* peer, const NetworkInfoBlob& info, bool connected) {
    if (!connected) {
        Post([this, peer] {
            std::shared_ptr<Peer> found;
            {
                std::scoped_lock lk{lock};
                for (const auto& p : peers) {
                    if (p.get() == peer) {
                        found = p;
                    }
                }
            }
            if (found) {
                SendLdn(LDNPacketType::DestroyNetwork, found->room_ip, {});
                Retire(found);
            }
        });
        return;
    }
    UpdateTable(info);
    {
        std::scoped_lock lk{lock};
        peer->last_info = info;
    }
    if (peer->joined.load()) {
        PushSync(*peer, info);
    }
}

void RoomBridge::OnPeerProxy(Peer* peer, const ProxyDataHeader& header, std::span<const u8> data) {
    const u32 src = header.info.source_ip;
    if (src == peer->ryu_ip.load()) {
        return; // the server echoes broadcasts to the sender
    }
    const auto member = GetRoomMember().lock();
    if (!member || !member->IsConnected()) {
        return;
    }
    IPv4Address from{};
    {
        std::scoped_lock lk{lock};
        for (const auto& p : peers) {
            if (p->ryu_ip.load() == src) {
                return; // another suyu player: the room already delivers it
            }
        }
        if (node_live[0] && node_ip[0] == src) {
            from = member->GetFakeIpAddress();
        } else {
            bool found = false;
            for (std::size_t slot = 1; slot < 8 && !found; slot++) {
                if (node_live[slot] && node_ip[slot] == src) {
                    from = AliasAddress(static_cast<u8>(slot));
                    found = true;
                }
            }
            if (!found) {
                return;
            }
        }
    }
    std::vector<u8> payload(data.begin(), data.end());
    const std::size_t changed = Translate(payload, *peer, false);
    if (peer->to_room.fetch_add(1) < 6) {
        LOG_INFO(Network, "ryuldn bridge: lobby -> {}: {} bytes from {}.{}.{}.{} port {} ({} "
                          "address{} rewritten): {}",
                 peer->name, payload.size(), from[0], from[1], from[2], from[3],
                 header.info.source_port, changed, changed == 1 ? "" : "es", Hex(payload));
    }
    ProxyPacket out;
    out.local_endpoint = {Domain::INET, from, header.info.source_port};
    out.remote_endpoint = {Domain::INET, peer->room_ip, header.info.dest_port};
    out.protocol = header.info.protocol == ProtocolTcp ? Protocol::TCP : Protocol::UDP;
    out.broadcast = false; // addressed to this player alone, so its sockets accept it
    out.data = peer->raw.load() ? payload
                                : Common::Compression::CompressDataZSTDDefault(payload.data(),
                                                                               payload.size());
    member->SendProxyPacket(out);
}

// ── lobby membership ──

std::shared_ptr<RoomBridge::Peer> RoomBridge::FindPeer(const IPv4Address& room_ip) const {
    std::scoped_lock lk{lock};
    for (const auto& peer : peers) {
        if (peer->room_ip == room_ip) {
            return peer;
        }
    }
    return nullptr;
}

Client::Settings RoomBridge::PeerSettings() const {
    Client::Settings settings = ryuldn->settings(0);
    settings.keep_identity = false; // a player of its own, not this process's connection
    return settings;
}

void RoomBridge::DoConnect(IPv4Address room_ip, std::vector<u8> node) {
    if (!ryuldn->IsHosting()) {
        return;
    }
    const NetworkInfoBlob lobby = ryuldn->LobbyInfo();
    if (lobby[Info::NodeCount] >= lobby[Info::NodeCountMax]) {
        SendLdn(LDNPacketType::DestroyNetwork, room_ip, {});
        return;
    }
    std::shared_ptr<Peer> peer = FindPeer(room_ip);
    if (peer && peer->joined.load()) {
        PushSync(*peer, lobby); // joined already (a repeated request)
        return;
    }
    if (!peer) {
        peer = std::make_shared<Peer>();
        peer->room_ip = room_ip;
        peer->name = NodeName(node);
        std::memcpy(peer->mac.data(), node.data() + 4, 6);
        Peer* raw = peer.get();
        Client::Callbacks callbacks;
        callbacks.network_change = [this, raw](const NetworkInfoBlob& info, bool connected) {
            OnPeerNetwork(raw, info, connected);
        };
        callbacks.proxy_config = [raw](const ProxyConfig& config) {
            raw->ryu_ip = config.proxy_ip;
            raw->mask = config.proxy_subnet_mask;
        };
        callbacks.proxy_data = [this, raw](const ProxyDataHeader& header, std::span<const u8> data) {
            OnPeerProxy(raw, header, data);
        };
        peer->client = std::make_unique<Client>(PeerSettings(), std::move(callbacks));
        {
            std::scoped_lock lk{lock};
            if (peers.size() >= kMaxPeers) {
                return;
            }
            peers.push_back(peer);
        }
    }

    ConnectRequest request{};
    std::memcpy(request.security_config.data(), lobby.data() + Info::SecurityMode, 2);
    std::memcpy(request.user_config.data(), node.data() + NodeOff::Name, NodeOff::NameSize);
    u16 version = 0;
    std::memcpy(&version, node.data() + NodeOff::Lcv, 2);
    request.local_communication_version = version;
    request.network_info = lobby;

    const NetworkError error = peer->client->Connect(request);
    if (error != NetworkError::None) {
        LOG_WARNING(Network, "ryuldn bridge: {} couldn't join the lobby: {}", peer->name,
                    ErrorName(error));
        SendLdn(LDNPacketType::DestroyNetwork, room_ip, {});
        Retire(peer);
        return;
    }
    peer->joined = true;
    LOG_INFO(Network, "ryuldn bridge: {} joined the lobby from the room as {}.{}.{}.{}", peer->name,
             peer->ryu_ip.load() >> 24, (peer->ryu_ip.load() >> 16) & 0xFF,
             (peer->ryu_ip.load() >> 8) & 0xFF, peer->ryu_ip.load() & 0xFF);
    NetworkInfoBlob info;
    {
        std::scoped_lock lk{lock};
        info = peer->last_info;
    }
    PushSync(*peer, info);
}

void RoomBridge::Retire(std::shared_ptr<Peer> peer) {
    {
        std::scoped_lock lk{lock};
        std::erase(peers, peer);
        graveyard.push_back(peer);
    }
    // Both end the client's reader (so no callback runs afterwards); never from that reader.
    peer->client->DisconnectNetwork();
    peer->client->Close();
}

void RoomBridge::DropPeer(const IPv4Address& room_ip, bool tell_player) {
    if (const auto peer = FindPeer(room_ip)) {
        if (tell_player) {
            SendLdn(LDNPacketType::DestroyNetwork, room_ip, {});
        }
        Retire(peer);
    }
}

void RoomBridge::DropAll(bool tell_players) {
    std::vector<std::shared_ptr<Peer>> all;
    {
        std::scoped_lock lk{lock};
        all = peers;
    }
    for (const auto& peer : all) {
        if (tell_players) {
            SendLdn(LDNPacketType::DestroyNetwork, peer->room_ip, {});
        }
        Retire(peer);
    }
}

// ── translation ──

NetworkInfoBlob RoomBridge::RoomView(const NetworkInfoBlob& info) const {
    NetworkInfoBlob view = info;
    const auto member = GetRoomMember().lock();
    const IPv4Address host = member ? member->GetFakeIpAddress() : IPv4Address{};
    std::scoped_lock lk{lock};
    for (std::size_t slot = 0; slot < 8; slot++) {
        u8* node = view.data() + Info::Nodes + slot * NodeOff::Size;
        if (node[NodeOff::Connected] == 0) {
            continue;
        }
        u32 ip;
        std::memcpy(&ip, node + NodeOff::Ip, 4);
        u32 mapped;
        if (slot == 0) {
            mapped = ToU32(host);
            // LANDiscovery's own MAC for a host: 02:00 + its address
            node[4] = 0x02;
            node[5] = 0x00;
            std::memcpy(node + 6, host.data(), 4);
        } else {
            mapped = ToU32(AliasAddress(static_cast<u8>(slot)));
            for (const auto& peer : peers) {
                if (peer->ryu_ip.load() == ip) {
                    mapped = ToU32(peer->room_ip);
                    // The player finds its own node by the MAC it announced.
                    std::memcpy(node + 4, peer->mac.data(), 6);
                }
            }
        }
        std::memcpy(node + NodeOff::Ip, &mapped, 4);
    }
    return view;
}

std::size_t RoomBridge::Translate(std::vector<u8>& data, const Peer& peer, bool to_lobby) const {
    const auto member = GetRoomMember().lock();
    if (!member) {
        return 0;
    }
    const IPv4Address host = member->GetFakeIpAddress();
    std::array<u8, 6> host_mac{0x02, 0x00, host[0], host[1], host[2], host[3]};
    std::vector<Subst> subs; // room view -> lobby view
    {
        std::scoped_lock lk{lock};
        AddIp(subs, ToU32(host), node_live[0] ? node_ip[0] : 0);
        AddMac(subs, host_mac, node_mac[0]);
        for (const auto& p : peers) {
            const u32 ryu = p->ryu_ip.load();
            AddIp(subs, ToU32(p->room_ip), ryu);
            for (std::size_t slot = 1; slot < 8; slot++) {
                if (node_live[slot] && node_ip[slot] == ryu) {
                    AddMac(subs, p->mac, node_mac[slot]);
                }
            }
        }
        for (std::size_t slot = 1; slot < 8; slot++) {
            if (node_live[slot]) {
                AddIp(subs, ToU32(AliasAddress(static_cast<u8>(slot))), node_ip[slot]);
            }
        }
    }
    std::size_t changed = 0;
    for (const auto& sub : subs) {
        const u8* from = to_lobby ? sub.from.data() : sub.to.data();
        const u8* to = to_lobby ? sub.to.data() : sub.from.data();
        for (std::size_t i = 0; i + sub.length <= data.size(); i++) {
            if (std::memcmp(data.data() + i, from, sub.length) == 0) {
                std::memcpy(data.data() + i, to, sub.length);
                changed++;
                i += sub.length - 1;
            }
        }
    }
    return changed;
}

void RoomBridge::SendLdn(LDNPacketType type, const IPv4Address& to, std::span<const u8> data) {
    const auto member = GetRoomMember().lock();
    if (!member || !member->IsConnected()) {
        return;
    }
    LDNPacket packet;
    packet.type = type;
    packet.local_ip = member->GetFakeIpAddress();
    packet.remote_ip = to;
    packet.broadcast = false;
    packet.data.assign(data.begin(), data.end());
    member->SendLdnPacket(packet);
}

void RoomBridge::PushSync(Peer& peer, const NetworkInfoBlob& info) {
    const NetworkInfoBlob view = RoomView(info);
    SendLdn(LDNPacketType::SyncNetwork, peer.room_ip, view);
}

} // namespace Network::RyuLdn
