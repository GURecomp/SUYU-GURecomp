// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <thread>

#include "common/logging.h"
#include "network/ryuldn/backend.h"

namespace Network::RyuLdn {

namespace {
std::mutex g_active_lock;
std::shared_ptr<Backend> g_active;

/// The server treats both as "everyone in the lobby" (its own subnet broadcast and the
/// 192.168.0.255 some games use).
constexpr u32 kAltBroadcast = 0xC0A800FF;
} // namespace

std::shared_ptr<Backend> GetActiveBackend() {
    std::scoped_lock lk{g_active_lock};
    return g_active;
}

void Register(std::shared_ptr<Backend> backend) {
    std::scoped_lock lk{g_active_lock};
    g_active = std::move(backend);
}

void Activate(std::shared_ptr<Backend> backend) {
    {
        std::scoped_lock lk{g_active_lock};
        g_active = backend;
    }
    SetLdnBackend(std::move(backend));
}

u32 Backend::ToU32(const IPv4Address& ip) {
    return (u32{ip[0]} << 24) | (u32{ip[1]} << 16) | (u32{ip[2]} << 8) | u32{ip[3]};
}

IPv4Address Backend::ToAddress(u32 ip) {
    return {static_cast<u8>(ip >> 24), static_cast<u8>(ip >> 16), static_cast<u8>(ip >> 8),
            static_cast<u8>(ip)};
}

Backend::Backend(std::vector<Client::Settings> servers) {
    current = MakeSet(std::move(servers), 0);
    retry_after.resize(current->clients.size());
}

Backend::~Backend() {
    CloseAll();
}

std::shared_ptr<Backend::ServerSet> Backend::MakeSet(std::vector<Client::Settings> servers,
                                                      u64 gen) {
    auto set = std::make_shared<ServerSet>();
    set->generation = gen;
    set->settings = std::move(servers);
    for (std::size_t i = 0; i < set->settings.size(); i++) {
        Client::Callbacks callbacks;
        callbacks.network_change = [this, gen, i](const NetworkInfoBlob& info, bool connected) {
            if (IsCurrent(gen, i)) {
                OnNetworkChange(info, connected);
            }
        };
        callbacks.proxy_config = [](const ProxyConfig& config) {
            LOG_INFO(Network, "ryuldn: lobby address {}.{}.{}.{}", config.proxy_ip >> 24,
                     (config.proxy_ip >> 16) & 0xFF, (config.proxy_ip >> 8) & 0xFF,
                     config.proxy_ip & 0xFF);
        };
        callbacks.proxy_data = [this, gen, i](const ProxyDataHeader& header,
                                              std::span<const u8> data) {
            if (IsCurrent(gen, i)) {
                OnProxyData(header, data);
            }
        };
        set->clients.push_back(std::make_shared<Client>(set->settings[i], std::move(callbacks)));
    }
    return set;
}

std::shared_ptr<Backend::ServerSet> Backend::Current() const {
    std::scoped_lock lk{set_lock};
    return current;
}

bool Backend::IsCurrent(u64 gen, std::size_t server) const {
    // Only the active connection of the current set can be in a lobby.
    return gen == generation.load() && server == active.load();
}

Client& Backend::Active() const {
    const auto set = Current();
    return *set->clients[std::min(active.load(), set->clients.size() - 1)];
}

Client& Backend::client() {
    return Active();
}

std::size_t Backend::ServerCount() const {
    return Current()->clients.size();
}

Client::Settings Backend::settings(std::size_t server) const {
    return Current()->settings.at(server);
}

bool Backend::SetServers(std::vector<Client::Settings> servers, std::string& error) {
    if (servers.empty()) {
        error = "no server";
        return false;
    }
    std::shared_ptr<ServerSet> old;
    {
        std::scoped_lock lk{set_lock};
        for (const auto& c : current->clients) {
            if (c->InNetwork()) {
                error = "leave the lobby first";
                return false;
            }
        }
        const u64 gen = generation.load() + 1;
        old = current;
        current = MakeSet(std::move(servers), gen);
        for (auto& c : current->clients) {
            c->SetGameVersion(game_version);
        }
        retired.push_back(old);
        active = 0;
        generation = gen;
    }
    const auto set = Current();
    {
        std::scoped_lock lk{scan_lock};
        lobby_server.clear();
        retry_after.assign(set->clients.size(), {});
    }
    for (auto& c : old->clients) {
        c->Close();
    }
    LOG_INFO(Network, "ryuldn: servers changed: {}:{} (+{} more)", set->settings[0].host,
             set->settings[0].port, set->settings.size() - 1);
    return true;
}

void Backend::CloseAll() {
    for (auto& c : Current()->clients) {
        c->Close();
    }
}

void Backend::SetGameVersion(const std::string& version) {
    std::scoped_lock lk{set_lock};
    game_version = version;
    for (auto& c : current->clients) {
        c->SetGameVersion(version);
    }
}

std::optional<std::vector<NetworkInfoBlob>> Backend::Scan(const ScanFilterBlob& filter) {
    const auto set = Current();
    const auto& clients = set->clients;
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::size_t> asked;
    {
        std::scoped_lock lk{scan_lock};
        retry_after.resize(clients.size());
        for (std::size_t i = 0; i < clients.size(); i++) {
            // The main server is always asked (as with one server); extras that were down
            // aren't retried on every scan (each try can take the connect timeout).
            if (i == 0 || now >= retry_after[i]) {
                asked.push_back(i);
            }
        }
    }

    std::vector<std::optional<std::vector<NetworkInfoBlob>>> found(clients.size());
    if (asked.size() == 1) {
        found[asked[0]] = clients[asked[0]]->Scan(filter);
    } else {
        std::vector<std::thread> threads;
        for (const std::size_t i : asked) {
            threads.emplace_back(
                [&clients, i, &filter, &found] { found[i] = clients[i]->Scan(filter); });
        }
        for (auto& t : threads) {
            t.join();
        }
    }

    std::scoped_lock lk{scan_lock};
    if (set->generation != generation.load()) {
        return std::nullopt; // the servers changed meanwhile
    }
    std::optional<std::vector<NetworkInfoBlob>> merged;
    lobby_server.clear();
    for (const std::size_t i : asked) {
        if (!found[i]) {
            if (i != 0 && !clients[i]->IsConnected()) {
                retry_after[i] = std::chrono::steady_clock::now() + UnreachableRetry;
            }
            continue;
        }
        if (!merged) {
            merged.emplace();
        }
        for (const auto& info : *found[i]) {
            NetworkKey key;
            std::memcpy(key.data(), info.data(), key.size());
            if (lobby_server.emplace(key, i).second) { // the same lobby twice: first server wins
                merged->push_back(info);
            }
        }
    }
    return merged;
}

NetworkError Backend::CreateNetwork(const CreateAccessPointRequest& request,
                                    std::span<const u8> advertise) {
    const auto set = Current();
    active = 0; // hosted lobbies live on the main server
    hosting = false;
    const NetworkError error = set->clients[0]->CreateNetwork(request, advertise);
    hosting = error == NetworkError::None;
    return error;
}

NetworkError Backend::Connect(const ConnectRequest& request) {
    NetworkKey key;
    std::memcpy(key.data(), request.network_info.data(), key.size());
    const auto set = Current();
    std::size_t server = 0;
    {
        std::scoped_lock lk{scan_lock};
        if (const auto it = lobby_server.find(key);
            it != lobby_server.end() && it->second < set->clients.size()) {
            server = it->second;
        }
    }
    if (server != 0) {
        LOG_INFO(Network, "ryuldn: joining a lobby of server {} ({}:{})", server + 1,
                 set->settings[server].host, set->settings[server].port);
    }
    active = server;
    hosting = false;
    return set->clients[server]->Connect(request);
}

void Backend::SetNetworkListener(NetworkListener listener, const void* owner) {
    std::scoped_lock lk{listener_lock};
    network_listener = std::move(listener);
    listener_owner = owner;
}

bool Backend::ClearNetworkListener(const void* owner) {
    std::scoped_lock lk{listener_lock};
    if (listener_owner != owner) {
        return false;
    }
    network_listener = {};
    listener_owner = nullptr;
    return true;
}

Backend::ObserverHandle Backend::AddNetworkObserver(NetworkListener observer) {
    auto handle = std::make_shared<NetworkListener>(std::move(observer));
    std::scoped_lock lk{listener_lock};
    observers.push_back(handle);
    return handle;
}

void Backend::RemoveNetworkObserver(const ObserverHandle& handle) {
    std::scoped_lock lk{listener_lock};
    std::erase(observers, handle);
}

bool Backend::IsHosting() const {
    return hosting.load() && Active().InNetwork();
}

NetworkInfoBlob Backend::LobbyInfo() const {
    std::scoped_lock lk{lobby_lock};
    return lobby_info;
}

void Backend::OnNetworkChange(const NetworkInfoBlob& info, bool connected) {
    {
        std::scoped_lock lk{lobby_lock};
        lobby_info = connected ? info : NetworkInfoBlob{};
    }
    if (!connected) {
        hosting = false;
    }
    std::vector<ObserverHandle> watchers;
    {
        std::scoped_lock lk{listener_lock};
        if (network_listener) {
            network_listener(info, connected);
        }
        watchers = observers;
    }
    for (const auto& watcher : watchers) {
        (*watcher)(info, connected);
    }
}

bool Backend::IsConnected() const {
    return Active().InNetwork() && Active().GetProxyConfig().has_value();
}

IPv4Address Backend::GetFakeIpAddress() const {
    if (const auto config = Active().GetProxyConfig()) {
        return ToAddress(config->proxy_ip);
    }
    return {};
}

std::optional<IPv4Address> Backend::GetFakeSubnetMask() const {
    if (const auto config = Active().GetProxyConfig()) {
        return ToAddress(config->proxy_subnet_mask);
    }
    return std::nullopt;
}

void Backend::SendProxyPacket(const ProxyPacket& packet) {
    const auto config = Active().GetProxyConfig();
    if (!config) {
        return;
    }
    ProxyInfo info{};
    info.source_ip = ToU32(packet.local_endpoint.ip);
    info.source_port = packet.local_endpoint.portno;
    info.dest_ip = packet.broadcast ? (config->proxy_ip | ~config->proxy_subnet_mask)
                                    : ToU32(packet.remote_endpoint.ip);
    info.dest_port = packet.remote_endpoint.portno;
    info.protocol = packet.protocol == Protocol::TCP ? ProtocolTcp : ProtocolUdp;
    Active().SendProxyData(info, packet.data);
}

void Backend::OnProxyData(const ProxyDataHeader& header, std::span<const u8> data) {
    const auto config = Active().GetProxyConfig();
    const u32 broadcast = config ? (config->proxy_ip | ~config->proxy_subnet_mask) : 0;

    ProxyPacket packet;
    packet.local_endpoint = {Domain::INET, ToAddress(header.info.source_ip), header.info.source_port};
    packet.remote_endpoint = {Domain::INET, ToAddress(header.info.dest_ip), header.info.dest_port};
    packet.protocol = header.info.protocol == ProtocolTcp ? Protocol::TCP : Protocol::UDP;
    packet.broadcast = header.info.dest_ip == broadcast || header.info.dest_ip == kAltBroadcast ||
                       header.info.dest_ip == 0xFFFFFFFF;
    packet.data.assign(data.begin(), data.end());

    std::vector<CallbackHandle<ProxyPacket>> targets;
    {
        std::scoped_lock lk{callbacks_lock};
        targets = proxy_callbacks;
    }
    for (const auto& callback : targets) {
        (*callback)(packet);
    }
}

void Backend::SendLdnPacket(const LDNPacket&) {
    // Lobby control goes through the master server (RyuLdnNetwork), not as LDN packets.
}

LdnBackend::CallbackHandle<ProxyPacket> Backend::BindOnProxyPacketReceived(
    std::function<void(const ProxyPacket&)> callback) {
    auto handle = std::make_shared<std::function<void(const ProxyPacket&)>>(std::move(callback));
    std::scoped_lock lk{callbacks_lock};
    proxy_callbacks.push_back(handle);
    return handle;
}

LdnBackend::CallbackHandle<LDNPacket> Backend::BindOnLdnPacketReceived(
    std::function<void(const LDNPacket&)>) {
    return nullptr;
}

void Backend::Unbind(CallbackHandle<ProxyPacket> handle) {
    std::scoped_lock lk{callbacks_lock};
    std::erase(proxy_callbacks, handle);
}

void Backend::Unbind(CallbackHandle<LDNPacket>) {}

} // namespace Network::RyuLdn
