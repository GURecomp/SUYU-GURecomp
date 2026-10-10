// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "network/ryuldn/client.h"
#include "network/ryuldn/socket_util.h"

namespace Network::RyuLdn {

namespace {

/// The id and MAC each server gave this process: sent again on reconnects so the server keeps
/// the same MAC for us (Ryujinx keeps it for the emulator's lifetime the same way). Per server:
/// one server's id means nothing to another.
std::mutex g_identity_lock;
std::map<std::string, InitializeMessage> g_identities;

std::string IdentityKey(const std::string& host, u16 port) {
    return host + ":" + std::to_string(port);
}

} // namespace

/// TCP link to a lobby host's own P2P proxy: authenticated with the master's token, then it
/// assigns our address (ProxyConfig) and carries the game traffic (ProxyData).
class P2pLink {
public:
    P2pLink(std::uintptr_t s, std::function<void(const Packet&)> handle,
            std::function<void(const std::string&)> lost)
        : sock{s} {
        reader = std::thread([this, handle = std::move(handle), lost = std::move(lost)] {
            const std::string why = ReadMessages(sock, handle);
            if (!closing.load()) {
                lost(why);
            }
        });
    }
    ~P2pLink() {
        closing = true;
        ShutdownSocket(sock);
        if (reader.joinable()) {
            reader.join();
        }
        CloseSocket(sock);
    }
    bool Send(const std::vector<u8>& bytes) {
        std::scoped_lock lk{send_lock};
        return SendAll(sock, bytes);
    }

private:
    std::uintptr_t sock;
    std::atomic<bool> closing{false};
    std::mutex send_lock;
    std::thread reader;
};

Client::Client(Settings settings_, Callbacks callbacks_)
    : settings{std::move(settings_)}, callbacks{std::move(callbacks_)}, sock{kInvalidSocket},
      game_version{settings.game_version} {
    EnsureSockets();
}

Client::~Client() {
    Close();
}

bool Client::EnsureConnected() {
    std::scoped_lock connect_guard{connect_lock};
    if (connected.load()) {
        return true;
    }

    // A connection that broke earlier: its reader has finished or is finishing.
    if (reader.joinable()) {
        reader.join();
    }
    if (sock != kInvalidSocket) {
        std::scoped_lock send_guard{send_lock};
        CloseSocket(sock);
        sock = kInvalidSocket;
    }

    std::string error;
    const std::uintptr_t s = ConnectTcp(settings.host, settings.port, ConnectTimeoutMs, error);
    if (s == kInvalidSocket) {
        std::scoped_lock lk{lock};
        last_error = error;
        LOG_WARNING(Network, "ryuldn: {}", error);
        return false;
    }

    {
        std::scoped_lock lk{lock};
        sock = s;
        generation++;
        proxy.reset();
        scan_done = join_done = reject_done = false;
        join_error = last_network_error = NetworkError::None;
        last_error.clear();
    }
    in_network = false;
    connected = true;
    reader = std::thread([this, s] { ReaderLoop(s); });
    LOG_INFO(Network, "ryuldn: connected to {}:{}", settings.host, settings.port);

    InitializeMessage hello{};
    {
        std::scoped_lock id_guard{g_identity_lock};
        if (settings.keep_identity) {
            hello = g_identities[IdentityKey(settings.host, settings.port)];
        }
    }
    Send(Encode(PacketId::Initialize, hello));
    Send(Encode(PacketId::Passphrase, MakePassphrase(settings.passphrase)));
    return true;
}

void Client::Close() {
    std::scoped_lock connect_guard{connect_lock};
    {
        std::scoped_lock lk{lock};
        connected = false; // the reader's OnConnectionLost then stays quiet
        proxy.reset();
    }
    in_network = false;
    changed.notify_all();
    ResetP2p();
    if (sock != kInvalidSocket) {
        ShutdownSocket(sock); // wakes the reader
    }
    if (reader.joinable()) {
        reader.join();
    }
    if (sock != kInvalidSocket) {
        std::scoped_lock send_guard{send_lock};
        CloseSocket(sock);
        sock = kInvalidSocket;
    }
}

bool Client::Send(const std::vector<u8>& bytes) {
    std::scoped_lock send_guard{send_lock};
    if (sock == kInvalidSocket || !connected.load()) {
        return false;
    }
    return SendAll(sock, bytes); // a failure: the reader notices the broken connection
}

void Client::ReaderLoop(std::uintptr_t s) {
    const std::string why = ReadMessages(s, [this](const Packet& packet) { Handle(packet); });
    OnConnectionLost("server connection lost: " + why);
}

void Client::OnConnectionLost(const std::string& why) {
    {
        std::scoped_lock lk{lock};
        if (!connected.load()) {
            return; // Close() did it
        }
        connected = false;
        last_error = why;
        proxy.reset();
        generation++;
    }
    const bool was_in_network = in_network.exchange(false);
    changed.notify_all();
    ResetP2p();
    LOG_WARNING(Network, "ryuldn: {}", why);
    if (was_in_network && callbacks.network_change) {
        callbacks.network_change(NetworkInfoBlob{}, false);
    }
}

void Client::Handle(const Packet& packet) {
    const std::span<const u8> data{packet.data};
    switch (packet.type) {
    case PacketId::Initialize:
        if (const auto msg = Read<InitializeMessage>(data)) {
            std::scoped_lock id_guard{g_identity_lock};
            if (settings.keep_identity) {
                g_identities[IdentityKey(settings.host, settings.port)] = *msg;
            }
        }
        break;
    case PacketId::Ping:
        if (const auto ping = Read<PingMessage>(data); ping && ping->requester == 0) {
            Send(Encode(PacketId::Ping, *ping)); // the server checks that we're alive
        }
        break;
    case PacketId::NetworkError:
        if (const auto msg = Read<NetworkErrorMessage>(data)) {
            if (msg->error == NetworkError::PortUnreachable) {
                break; // about a P2P proxy we don't host
            }
            LOG_WARNING(Network, "ryuldn: server error {}", ErrorName(msg->error));
            {
                std::scoped_lock lk{lock};
                last_network_error = msg->error;
                join_error = msg->error;
            }
            changed.notify_all();
        }
        break;
    case PacketId::ProxyConfig:
        if (const auto cfg = Read<ProxyConfig>(data)) {
            {
                std::scoped_lock lk{lock};
                proxy = *cfg;
            }
            if (callbacks.proxy_config) {
                callbacks.proxy_config(*cfg);
            }
        }
        break;
    case PacketId::Connected:
        if (const auto info = Read<NetworkInfoBlob>(data)) {
            in_network = true;
            if (callbacks.network_change) {
                callbacks.network_change(*info, true);
            }
            {
                std::scoped_lock lk{lock};
                join_done = true;
            }
            changed.notify_all();
        }
        break;
    case PacketId::SyncNetwork:
        if (const auto info = Read<NetworkInfoBlob>(data); info && in_network.load()) {
            if (callbacks.network_change) {
                callbacks.network_change(*info, true);
            }
        }
        break;
    case PacketId::Disconnect:
        if (in_network.exchange(false)) {
            {
                std::scoped_lock lk{lock};
                proxy.reset();
            }
            LOG_INFO(Network, "ryuldn: the lobby was closed");
            if (callbacks.network_change) {
                callbacks.network_change(NetworkInfoBlob{}, false);
            }
        }
        break;
    case PacketId::ScanReply:
        if (const auto info = Read<NetworkInfoBlob>(data)) {
            std::scoped_lock lk{lock};
            scan_results.push_back(*info);
        }
        break;
    case PacketId::ScanReplyEnd:
        {
            std::scoped_lock lk{lock};
            scan_done = true;
        }
        changed.notify_all();
        break;
    case PacketId::RejectReply:
        {
            std::scoped_lock lk{lock};
            reject_done = true;
        }
        changed.notify_all();
        break;
    case PacketId::ProxyData:
        if (const auto header = Read<ProxyDataHeader>(data); header && callbacks.proxy_data) {
            const auto payload = Trailing<ProxyDataHeader>(data);
            if (payload.size() == header->data_length) {
                callbacks.proxy_data(*header, payload);
            }
        }
        break;
    case PacketId::ExternalProxy:
        if (const auto config = Read<ExternalProxyConfig>(data)) {
            StartP2p(*config);
        }
        break;
    default:
        break; // proxy TCP messages, external proxy tokens: not used by this client
    }
}

std::optional<ProxyConfig> Client::GetProxyConfig() const {
    std::scoped_lock lk{lock};
    return proxy;
}

std::string Client::LastError() const {
    std::scoped_lock lk{lock};
    return last_error;
}

void Client::SetGameVersion(std::string version) {
    std::scoped_lock lk{lock};
    game_version = std::move(version);
}

std::optional<std::vector<NetworkInfoBlob>> Client::Scan(const ScanFilterBlob& filter) {
    if (!EnsureConnected()) {
        return std::nullopt;
    }
    {
        std::scoped_lock lk{lock};
        scan_results.clear();
        scan_done = false;
    }
    if (!Send(Encode(PacketId::Scan, filter))) {
        return std::nullopt;
    }
    std::unique_lock lk{lock};
    const bool done = changed.wait_for(lk, std::chrono::milliseconds(ScanTimeoutMs),
                                       [&] { return scan_done || !connected.load(); });
    if (!done || !scan_done) {
        return std::nullopt;
    }
    return scan_results;
}

void Client::BeginJoin() {
    ResetP2p();
    std::scoped_lock lk{lock};
    join_done = false;
    join_error = NetworkError::None;
    p2p_expected = p2p_ready = false;
}

NetworkError Client::WaitForJoin() {
    NetworkError result;
    {
        std::unique_lock lk{lock};
        // A P2P host's proxy answers after the server's Connected; Ryujinx waits 4 s for each.
        const auto ready = [&] {
            return (join_done && (!p2p_expected || p2p_ready)) ||
                   join_error != NetworkError::None || !connected.load();
        };
        if (!changed.wait_for(lk, std::chrono::milliseconds(ConnectTimeoutMs), ready) &&
            join_done) {
            changed.wait_for(lk, std::chrono::milliseconds(ConnectTimeoutMs), ready);
        }
        if (join_error != NetworkError::None) {
            result = join_error;
        } else if (!connected.load()) {
            result = NetworkError::ConnectFailure;
        } else if (!join_done) {
            result = NetworkError::ConnectTimeout;
        } else if (p2p_expected && !p2p_ready) {
            last_error = "the host's P2P proxy didn't answer";
            result = NetworkError::ConnectFailure;
        } else {
            return NetworkError::None;
        }
    }
    if (in_network.load()) {
        DisconnectNetwork(); // the server let us in, but we can't use the lobby
    }
    return result;
}

void Client::StartP2p(const ExternalProxyConfig& config) {
    // .NET AddressFamily: InterNetwork = 2 (InterNetworkV6 = 23 isn't supported here).
    std::string error;
    std::uintptr_t s = kInvalidSocket;
    if (config.address_family == 2) {
        const auto& ip = config.proxy_ip;
        s = ConnectTcp(fmt::format("{}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3]), config.proxy_port,
                       ConnectTimeoutMs, error);
    } else {
        error = fmt::format("address family {} not supported", config.address_family);
    }
    if (s == kInvalidSocket) {
        LOG_WARNING(Network, "ryuldn: can't reach the host's P2P proxy: {}", error);
        {
            std::scoped_lock lk{lock};
            last_error = "can't reach the host's P2P proxy (" + error + ")";
            join_error = NetworkError::ConnectFailure;
        }
        changed.notify_all();
        return;
    }
    LOG_INFO(Network, "ryuldn: the host runs a P2P proxy, connecting to it");
    auto link = std::make_unique<P2pLink>(
        s, [this](const Packet& packet) { HandleP2p(packet); },
        [this](const std::string& why) { OnP2pLost(why); });
    {
        std::scoped_lock lk{lock};
        p2p_expected = true;
    }
    link->Send(Encode(PacketId::ExternalProxy, config)); // the token authenticates us
    std::scoped_lock lk{p2p_lock};
    p2p = std::move(link);
}

void Client::HandleP2p(const Packet& packet) {
    const std::span<const u8> data{packet.data};
    switch (packet.type) {
    case PacketId::ProxyConfig:
        if (const auto cfg = Read<ProxyConfig>(data)) {
            {
                std::scoped_lock lk{lock};
                proxy = *cfg;
                p2p_ready = true;
            }
            changed.notify_all();
            if (callbacks.proxy_config) {
                callbacks.proxy_config(*cfg);
            }
        }
        break;
    case PacketId::ProxyData:
        if (const auto header = Read<ProxyDataHeader>(data); header && callbacks.proxy_data) {
            const auto payload = Trailing<ProxyDataHeader>(data);
            if (payload.size() == header->data_length) {
                callbacks.proxy_data(*header, payload);
            }
        }
        break;
    default:
        break;
    }
}

void Client::OnP2pLost(const std::string& why) {
    LOG_WARNING(Network, "ryuldn: the host's P2P proxy connection ended: {}", why);
    {
        std::scoped_lock lk{lock};
        last_error = "P2P proxy: " + why;
        proxy.reset();
    }
    changed.notify_all();
    if (in_network.exchange(false)) {
        Send(Encode(PacketId::Disconnect, DisconnectMessage{})); // tell the server we left
        if (callbacks.network_change) {
            callbacks.network_change(NetworkInfoBlob{}, false);
        }
    }
}

void Client::ResetP2p() {
    std::unique_ptr<P2pLink> old;
    {
        std::scoped_lock lk{p2p_lock};
        old = std::move(p2p);
    }
    // old's destructor stops its reader (never called from that reader itself)
}

NetworkError Client::CreateNetwork(CreateAccessPointRequest request,
                                   std::span<const u8> advertise) {
    if (!EnsureConnected()) {
        return NetworkError::ConnectFailure;
    }
    request.ryu_network_config = {};
    BeginJoin();
    {
        std::scoped_lock lk{lock};
        std::memcpy(request.ryu_network_config.game_version.data(), game_version.data(),
                    std::min<std::size_t>(game_version.size(), 15));
    }
    Send(Encode(PacketId::CreateAccessPoint, request, advertise));
    return WaitForJoin();
}

NetworkError Client::CreateNetworkPrivate(CreateAccessPointPrivateRequest request,
                                          std::span<const u8> advertise) {
    if (!EnsureConnected()) {
        return NetworkError::ConnectFailure;
    }
    request.ryu_network_config = {};
    BeginJoin();
    {
        std::scoped_lock lk{lock};
        std::memcpy(request.ryu_network_config.game_version.data(), game_version.data(),
                    std::min<std::size_t>(game_version.size(), 15));
    }
    Send(Encode(PacketId::CreateAccessPointPrivate, request, advertise));
    return WaitForJoin();
}

NetworkError Client::Connect(const ConnectRequest& request) {
    if (!EnsureConnected()) {
        return NetworkError::ConnectFailure;
    }
    BeginJoin();
    Send(Encode(PacketId::Connect, request));
    return WaitForJoin();
}

NetworkError Client::ConnectPrivate(const ConnectPrivateRequest& request) {
    if (!EnsureConnected()) {
        return NetworkError::ConnectFailure;
    }
    BeginJoin();
    Send(Encode(PacketId::ConnectPrivate, request));
    return WaitForJoin();
}

void Client::DisconnectNetwork() {
    if (!in_network.exchange(false)) {
        return;
    }
    {
        std::scoped_lock lk{lock};
        proxy.reset();
    }
    ResetP2p();
    Send(Encode(PacketId::Disconnect, DisconnectMessage{}));
}

bool Client::Reject(u32 node_id, u32 disconnect_reason) {
    if (!in_network.load()) {
        return false;
    }
    {
        std::scoped_lock lk{lock};
        reject_done = false;
        last_network_error = NetworkError::None;
    }
    Send(Encode(PacketId::Reject, RejectRequest{node_id, disconnect_reason}));
    std::unique_lock lk{lock};
    changed.wait_for(lk, std::chrono::milliseconds(RejectTimeoutMs),
                     [&] { return reject_done || !connected.load(); });
    return reject_done && last_network_error == NetworkError::None;
}

void Client::SetAdvertiseData(std::span<const u8> data) {
    if (in_network.load()) {
        Send(Encode(PacketId::SetAdvertiseData, data));
    }
}

void Client::SetAcceptPolicy(u8 policy) {
    if (in_network.load()) {
        Send(Encode(PacketId::SetAcceptPolicy, SetAcceptPolicyRequest{policy}));
    }
}

void Client::SendProxyData(const ProxyInfo& info, std::span<const u8> data) {
    if (!in_network.load()) {
        return;
    }
    if (sizeof(ProxyDataHeader) + data.size() > MaxPayloadSize) {
        LOG_WARNING(Network, "ryuldn: dropped a {} byte packet (too large to relay)", data.size());
        return;
    }
    const auto bytes =
        Encode(PacketId::ProxyData, ProxyDataHeader{info, static_cast<u32>(data.size())}, data);
    {
        std::scoped_lock lk{p2p_lock};
        if (p2p) {
            p2p->Send(bytes); // the lobby's host relays (its own P2P proxy)
            return;
        }
    }
    Send(bytes);
}

} // namespace Network::RyuLdn
