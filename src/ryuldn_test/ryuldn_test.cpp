// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Checks of the RyuLdn wire layer (network/ryuldn), no emulator needed:
//   ryuldn-test [fixtures.json]
// fixtures.json = tools/ryuldn/fixtures/probe_session.json (frames of a passing probe run
// against a real LdnServer). Exit code 0 = all checks passed.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "network/ryuldn/backend.h"
#include "network/ryuldn/protocol.h"
#include "network/ryuldn/server.h"

using namespace Network::RyuLdn;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", what.c_str());
    } else if (std::getenv("RYULDN_TEST_VERBOSE")) {
        std::printf("  ok %s\n", what.c_str());
    }
}

template <typename T>
void RoundTrip(PacketId id, const T& message, std::span<const u8> extra = {}) {
    const auto bytes = Encode(id, message, extra);
    Check(bytes.size() == HeaderSize + sizeof(T) + extra.size(),
          std::string(PacketName(id)) + " encoded size");
    Framer framer;
    std::vector<Packet> packets;
    Check(framer.Feed(bytes, packets) && packets.size() == 1 && packets[0].type == id,
          std::string(PacketName(id)) + " framed");
    if (packets.size() != 1) {
        return;
    }
    const auto back = Read<T>(packets[0].data);
    Check(back && std::memcmp(&*back, &message, sizeof(T)) == 0,
          std::string(PacketName(id)) + " fields survive");
    const auto tail = Trailing<T>(packets[0].data);
    // memcmp only when non-empty: GCC 13 flags std::equal on an empty span's null data (-Wnonnull).
    Check(tail.size() == extra.size() &&
              (extra.empty() || std::memcmp(tail.data(), extra.data(), extra.size()) == 0),
          std::string(PacketName(id)) + " trailing data survives");
}

template <typename T>
T Filled(u8 seed) {
    T value;
    auto* p = reinterpret_cast<u8*>(&value);
    for (std::size_t i = 0; i < sizeof(T); i++) {
        p[i] = static_cast<u8>(seed + i * 7);
    }
    return value;
}

void TestRoundTrips() {
    std::printf("round trips\n");
    std::vector<u8> adv(0x180);
    for (std::size_t i = 0; i < adv.size(); i++) {
        adv[i] = static_cast<u8>(i);
    }
    RoundTrip(PacketId::Initialize, Filled<InitializeMessage>(1));
    RoundTrip(PacketId::Passphrase, MakePassphrase("Ryujinx-0123abcd"));
    RoundTrip(PacketId::Ping, PingMessage{0, 42});
    RoundTrip(PacketId::Disconnect, DisconnectMessage{0});
    RoundTrip(PacketId::NetworkError, NetworkErrorMessage{NetworkError::ConnectNotFound});
    RoundTrip(PacketId::Reject, RejectRequest{3, 5});
    RoundTrip(PacketId::SetAcceptPolicy, SetAcceptPolicyRequest{1});
    RoundTrip(PacketId::ProxyConfig, ProxyConfig{0x0A720001, 0xFFFF0000});
    RoundTrip(PacketId::ProxyConnect, Filled<ProxyConnectRequest>(2));
    RoundTrip(PacketId::ProxyConnectReply, Filled<ProxyConnectResponse>(3));
    RoundTrip(PacketId::ProxyDisconnect, Filled<ProxyDisconnectMessage>(4));
    const std::vector<u8> data{1, 2, 3, 4, 5};
    ProxyDataHeader pdh{ProxyInfo{0x0A720002, 50000, 0x0A720001, 12345, ProtocolUdp},
                        static_cast<u32>(data.size())};
    RoundTrip(PacketId::ProxyData, pdh, data);
    RoundTrip(PacketId::CreateAccessPoint, Filled<CreateAccessPointRequest>(5), adv);
    RoundTrip(PacketId::CreateAccessPointPrivate, Filled<CreateAccessPointPrivateRequest>(6), adv);
    RoundTrip(PacketId::Connect, Filled<ConnectRequest>(7));
    RoundTrip(PacketId::ConnectPrivate, Filled<ConnectPrivateRequest>(8));
    RoundTrip(PacketId::Scan, Filled<ScanFilterBlob>(9));
    RoundTrip(PacketId::SyncNetwork, Filled<NetworkInfoBlob>(10));
    RoundTrip(PacketId::ExternalProxy, Filled<ExternalProxyConfig>(11));
    RoundTrip(PacketId::ExternalProxyToken, Filled<ExternalProxyToken>(12));
    RoundTrip(PacketId::ExternalProxyState, Filled<ExternalProxyConnectionState>(13));

    const auto empty = Encode(PacketId::ScanReplyEnd);
    Check(empty.size() == HeaderSize, "empty message = header only");
    const u8 expect[12] = {'R', 'L', 'D', 'N', 12, 1, 0, 0, 0, 0, 0, 0};
    Check(std::memcmp(empty.data(), expect, 12) == 0, "header bytes: RLDN, type, version 1, size 0");

    // A passphrase never fills the last byte (stays zero-terminated).
    const auto longp = MakePassphrase(std::string(300, 'x'));
    Check(longp.passphrase[0x7F] == 0 && longp.passphrase[0x7E] == 'x', "passphrase cut at 0x7F");
}

struct Frame {
    std::string dir, type, hex;
};

std::vector<u8> Unhex(const std::string& s) {
    std::vector<u8> out(s.size() / 2);
    for (std::size_t i = 0; i < out.size(); i++) {
        out[i] = static_cast<u8>(std::stoul(s.substr(i * 2, 2), nullptr, 16));
    }
    return out;
}

// The fixture file is the probe's own JSON dump (indent=1); read just the frame fields.
std::vector<Frame> LoadFrames(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::vector<Frame> frames;
    auto field = [&](std::size_t from, const std::string& key, std::string& value) {
        const std::size_t k = text.find("\"" + key + "\": \"", from);
        if (k == std::string::npos) {
            return std::string::npos;
        }
        const std::size_t start = k + key.size() + 5;
        const std::size_t end = text.find('"', start);
        value = text.substr(start, end - start);
        return end;
    };
    std::size_t pos = text.find("\"frames\"");
    while (pos != std::string::npos) {
        Frame f;
        std::size_t p = field(pos, "dir", f.dir);
        if (p == std::string::npos) {
            break;
        }
        p = field(p, "type", f.type);
        p = field(p, "hex", f.hex);
        if (p == std::string::npos) {
            break;
        }
        frames.push_back(f);
        pos = p;
    }
    return frames;
}

std::size_t ExpectedSize(PacketId id) {
    switch (id) {
    case PacketId::Initialize:
        return sizeof(InitializeMessage);
    case PacketId::Passphrase:
        return sizeof(PassphraseMessage);
    case PacketId::Ping:
        return sizeof(PingMessage);
    case PacketId::Disconnect:
        return sizeof(DisconnectMessage);
    case PacketId::NetworkError:
        return sizeof(NetworkErrorMessage);
    case PacketId::ProxyConfig:
        return sizeof(ProxyConfig);
    case PacketId::Connect:
        return sizeof(ConnectRequest);
    case PacketId::Scan:
        return sizeof(ScanFilterBlob);
    case PacketId::Connected:
    case PacketId::SyncNetwork:
    case PacketId::ScanReply:
        return sizeof(NetworkInfoBlob);
    case PacketId::ScanReplyEnd:
        return 0;
    default:
        return SIZE_MAX; // variable (advertise data, proxied bytes)
    }
}

void TestFixtures(const std::string& path) {
    std::printf("fixtures %s\n", path.c_str());
    const auto frames = LoadFrames(path);
    Check(frames.size() > 20, "fixture frames loaded (" + std::to_string(frames.size()) + ")");

    std::vector<u8> stream;
    std::vector<PacketId> expected_types;
    for (const Frame& f : frames) {
        const auto bytes = Unhex(f.hex);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        Framer framer;
        std::vector<Packet> packets;
        const bool ok = framer.Feed(bytes, packets);
        Check(ok && packets.size() == 1 && framer.Pending() == 0, f.dir + " " + f.type + " frames");
        if (packets.size() != 1) {
            continue;
        }
        const Packet& p = packets[0];
        expected_types.push_back(p.type);
        Check(f.type == PacketName(p.type), f.type + " type id matches its name");
        const std::size_t want = ExpectedSize(p.type);
        if (want != SIZE_MAX) {
            Check(p.data.size() == want, f.type + " payload size " + std::to_string(p.data.size()));
        }
        // Our encoder reproduces every frame byte for byte.
        Check(Encode(p.type, p.data) == bytes, f.dir + " " + f.type + " re-encodes identically");
        // Struct views: decode + encode through the struct gives the same bytes.
        if (p.type == PacketId::ProxyData) {
            const auto h = Read<ProxyDataHeader>(p.data);
            const auto tail = Trailing<ProxyDataHeader>(p.data);
            Check(h && h->data_length == tail.size() && h->info.protocol == ProtocolUdp,
                  "ProxyData length field = trailing bytes");
            if (h) {
                Check(Encode(PacketId::ProxyData, *h, tail) == bytes, "ProxyData via struct");
            }
        }
        if (p.type == PacketId::Connect) {
            const auto c = Read<ConnectRequest>(p.data);
            Check(c && Encode(PacketId::Connect, *c) == bytes, "Connect via struct (0x500)");
        }
        if (p.type == PacketId::CreateAccessPoint) {
            const auto c = Read<CreateAccessPointRequest>(p.data);
            Check(c && Encode(PacketId::CreateAccessPoint, *c,
                              Trailing<CreateAccessPointRequest>(p.data)) == bytes,
                  "CreateAccessPoint via struct + advertise data");
        }
        if (p.type == PacketId::ProxyConfig) {
            const auto c = Read<ProxyConfig>(p.data);
            Check(c && (c->proxy_ip & 0xFFFF0000) == 0x0A720000 && c->proxy_subnet_mask == 0xFFFF0000,
                  "ProxyConfig = 10.114.x.x / 255.255.0.0");
        }
    }

    // The whole conversation fed one byte at a time and in odd chunks gives the same packets.
    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
        Framer framer;
        std::vector<Packet> packets;
        bool ok = true;
        for (std::size_t i = 0; i < stream.size(); i += chunk) {
            const std::size_t n = std::min(chunk, stream.size() - i);
            ok = ok && framer.Feed(std::span<const u8>(stream).subspan(i, n), packets);
        }
        bool same = ok && packets.size() == expected_types.size();
        for (std::size_t i = 0; same && i < packets.size(); i++) {
            same = packets[i].type == expected_types[i];
        }
        Check(same && framer.Pending() == 0,
              "stream in chunks of " + std::to_string(chunk) + " gives the same packets");
    }
}

void TestMalformed() {
    std::printf("malformed input\n");
    auto expect_error = [](std::vector<u8> bytes, const std::string& what) {
        Framer framer;
        std::vector<Packet> packets;
        const bool ok = framer.Feed(bytes, packets);
        Check(!ok && packets.empty() && !framer.error().empty(), what + " rejected");
        Check(!framer.Feed(Encode(PacketId::Ping, PingMessage{}), packets),
              what + ": stream stays closed");
    };
    auto bytes = Encode(PacketId::Ping, PingMessage{0, 1});
    auto bad = bytes;
    bad[0] = 'X';
    expect_error(bad, "bad magic");
    bad = bytes;
    bad[5] = 2;
    expect_error(bad, "version 2");
    bad = bytes;
    bad[8] = 0xFF, bad[9] = 0xFF, bad[10] = 0xFF, bad[11] = 0xFF; // -1
    expect_error(bad, "negative size");
    bad = bytes;
    const s32 huge = static_cast<s32>(MaxPacketSize - HeaderSize);
    std::memcpy(bad.data() + 8, &huge, 4);
    expect_error(bad, "size at the server's limit");

    {
        Framer framer;
        std::vector<Packet> packets;
        const s32 max_ok = static_cast<s32>(MaxPayloadSize);
        auto big = Encode(PacketId::ProxyData, std::vector<u8>(MaxPayloadSize, 0xAB));
        std::memcpy(big.data() + 8, &max_ok, 4);
        Check(framer.Feed(big, packets) && packets.size() == 1 &&
                  packets[0].data.size() == MaxPayloadSize,
              "largest allowed payload accepted");
    }
    {
        Framer framer;
        std::vector<Packet> packets;
        Check(framer.Feed(std::span<const u8>(bytes).first(5), packets) && packets.empty() &&
                  framer.Pending() == 5,
              "partial header waits");
        Check(!Read<ConnectRequest>(std::vector<u8>(0x4FC)), "short Connect payload refused");
    }

    // Fuzz: valid headers with random types/sizes and random garbage; never crash, and a stream
    // either keeps parsing or reports an error.
    std::mt19937 rng(1234);
    int errors = 0, parsed = 0;
    for (int round = 0; round < 20000; round++) {
        Framer framer;
        std::vector<Packet> packets;
        std::vector<u8> buf;
        const int pieces = 1 + static_cast<int>(rng() % 4);
        for (int i = 0; i < pieces; i++) {
            if (rng() % 3 == 0) {
                std::vector<u8> junk(rng() % 64);
                for (auto& b : junk) {
                    b = static_cast<u8>(rng());
                }
                buf.insert(buf.end(), junk.begin(), junk.end());
            } else {
                std::vector<u8> payload(rng() % 300);
                for (auto& b : payload) {
                    b = static_cast<u8>(rng());
                }
                auto m = Encode(static_cast<PacketId>(rng() % 256), payload);
                if (rng() % 4 == 0) {
                    const s32 lie = static_cast<s32>(rng());
                    std::memcpy(m.data() + 8, &lie, 4);
                }
                buf.insert(buf.end(), m.begin(), m.end());
            }
        }
        std::size_t i = 0;
        bool ok = true;
        while (ok && i < buf.size()) {
            const std::size_t n = std::min<std::size_t>(1 + rng() % 50, buf.size() - i);
            ok = framer.Feed(std::span<const u8>(buf).subspan(i, n), packets);
            i += n;
        }
        errors += ok ? 0 : 1;
        parsed += static_cast<int>(packets.size());
        for (const Packet& p : packets) {
            if (p.data.size() > MaxPayloadSize) {
                Check(false, "fuzz: oversized packet passed");
            }
        }
    }
    Check(errors > 0 && parsed > 0, "fuzz: 20000 streams, " + std::to_string(parsed) +
                                        " packets, " + std::to_string(errors) + " rejected");
}

// ---- live checks against a running LdnServer: two in-process nodes (client + backend) ----

constexpr u64 kLcid = 0x0100770008DD8000; // MHGU

UserConfigBlob User(const char* name) {
    UserConfigBlob u{};
    std::memcpy(u.data(), name, std::strlen(name));
    return u;
}

CreateAccessPointRequest CreateRequest(const char* name, u8 node_max = 4, u16 lcv = 0) {
    CreateAccessPointRequest r{};
    r.security_config[0] = 1; // SecurityMode Retail
    r.user_config = User(name);
    std::memcpy(r.network_config.data(), &kLcid, 8);
    const u16 channel = 6;
    std::memcpy(r.network_config.data() + 0x10, &channel, 2);
    r.network_config[0x12] = node_max;
    std::memcpy(r.network_config.data() + 0x14, &lcv, 2);
    return r;
}

ScanFilterBlob Filter(u64 lcid) {
    ScanFilterBlob f{};
    std::memcpy(f.data(), &lcid, 8);
    f[0x5C] = 1; // LocalCommunicationId
    return f;
}

ConnectRequest JoinRequest(const char* name, const NetworkInfoBlob& info, u32 lcv = 0) {
    ConnectRequest r{};
    r.security_config[0] = 1;
    r.user_config = User(name);
    r.local_communication_version = lcv;
    r.network_info = info;
    return r;
}

int NodeCount(const NetworkInfoBlob& info) {
    return info[0x67];
}

std::string NodeName(const NetworkInfoBlob& info, int node) {
    return reinterpret_cast<const char*>(info.data() + 0x68 + node * 0x40 + 0xC);
}

std::vector<u8> Advert(const NetworkInfoBlob& info) {
    u16 size;
    std::memcpy(&size, info.data() + 0x26A, 2);
    return {info.begin() + 0x26C, info.begin() + 0x26C + size};
}

/// A node: backend + what its listeners saw.
struct Node {
    explicit Node(Client::Settings s) : Node(std::vector<Client::Settings>{s}) {}
    explicit Node(std::vector<Client::Settings> s)
        : backend(std::make_shared<Network::RyuLdn::Backend>(std::move(s))) {
        backend->SetNetworkListener([this](const NetworkInfoBlob& info, bool connected) {
            std::scoped_lock lk{m};
            last_info = info;
            last_connected = connected;
            changes++;
            cv.notify_all();
        });
        handle = backend->BindOnProxyPacketReceived([this](const Network::ProxyPacket& p) {
            std::scoped_lock lk{m};
            packets.push_back(p);
            cv.notify_all();
        });
    }
    ~Node() {
        backend->Unbind(handle);
        backend->SetNetworkListener({});
    }
    template <typename Pred>
    bool WaitFor(Pred pred, int ms = 3000) {
        std::unique_lock lk{m};
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return pred(); });
    }

    std::shared_ptr<Network::RyuLdn::Backend> backend;
    Network::LdnBackend::CallbackHandle<Network::ProxyPacket> handle;
    std::mutex m;
    std::condition_variable cv;
    NetworkInfoBlob last_info{};
    bool last_connected = false;
    int changes = 0;
    std::vector<Network::ProxyPacket> packets;
};

Network::ProxyPacket Udp(const Network::IPv4Address& src, u16 sport, const Network::IPv4Address& dst,
                         u16 dport, std::vector<u8> data, bool broadcast = false) {
    Network::ProxyPacket p;
    p.local_endpoint = {Network::Domain::INET, src, sport};
    p.remote_endpoint = {Network::Domain::INET, dst, dport};
    p.protocol = Network::Protocol::UDP;
    p.broadcast = broadcast;
    p.data = std::move(data);
    return p;
}

void TestLive(const std::string& host, u16 port) {
    std::printf("live server %s:%u\n", host.c_str(), port);
    Client::Settings settings{host, port, "", "1.4.0"};
    Node a{settings}, b{settings};
    Client& ca = a.backend->client();
    Client& cb = b.backend->client();

    Check(ca.EnsureConnected() && cb.EnsureConnected(), "both nodes connect");
    const std::vector<u8> adv1{'m', 'h', 'g', 'u', 1};
    Check(ca.CreateNetwork(CreateRequest("HostA"), adv1) == NetworkError::None, "A creates a lobby");
    Check(ca.InNetwork() && a.backend->IsConnected(), "A in the lobby with a proxy address");
    const auto a_ip = a.backend->GetFakeIpAddress();
    Check(a_ip[0] == 10 && a_ip[1] == 114, "A's address is 10.114.x.x");
    const auto mask = a.backend->GetFakeSubnetMask();
    Check(mask && (*mask)[0] == 255 && (*mask)[1] == 255 && (*mask)[2] == 0,
          "subnet mask 255.255.0.0");
    Check(a.WaitFor([&] { return a.last_connected && NodeCount(a.last_info) == 1; }),
          "A's listener: 1 node");
    Check(Advert(a.last_info) == adv1, "advertise data in the lobby info");
    Check(!b.backend->IsConnected(), "B not in a lobby yet: no proxy address");

    const auto found = cb.Scan(Filter(kLcid));
    Check(found && found->size() == 1, "B finds 1 MHGU lobby");
    const auto other = cb.Scan(Filter(0x0100000000001000));
    Check(other && other->empty(), "B finds none for another title");
    if (!found || found->empty()) {
        return;
    }
    const NetworkInfoBlob lobby = (*found)[0];

    Check(cb.Connect(JoinRequest("GuestB", lobby, 1)) == NetworkError::VersionTooHigh,
          "newer local communication version refused (VersionTooHigh)");
    Check(cb.Connect(JoinRequest("GuestB", lobby)) == NetworkError::None, "B joins");
    Check(b.WaitFor([&] { return b.last_connected && NodeCount(b.last_info) == 2; }),
          "B's listener: 2 nodes");
    Check(a.WaitFor([&] { return NodeCount(a.last_info) == 2; }) &&
              NodeName(a.last_info, 1) == "GuestB",
          "A's listener: GuestB is node 1");
    const auto b_ip = b.backend->GetFakeIpAddress();
    Check(b_ip != a_ip && b_ip[0] == 10, "B has its own lobby address");

    const std::vector<u8> adv2{'n', 'e', 'w'};
    ca.SetAdvertiseData(adv2);
    Check(b.WaitFor([&] { return Advert(b.last_info) == adv2; }), "B sees A's new advertise data");

    // Data plane: what bsd/ProxySocket would send.
    b.backend->SendProxyPacket(Udp(b_ip, 50000, a_ip, 12345, {1, 2, 3}));
    Check(a.WaitFor([&] { return !a.packets.empty(); }), "A receives B's datagram");
    if (!a.packets.empty()) {
        const auto& p = a.packets.back();
        Check(p.local_endpoint.ip == b_ip && p.local_endpoint.portno == 50000 &&
                  p.remote_endpoint.ip == a_ip && p.remote_endpoint.portno == 12345 &&
                  p.protocol == Network::Protocol::UDP && !p.broadcast &&
                  p.data == std::vector<u8>{1, 2, 3},
              "datagram endpoints, protocol and bytes intact");
    }
    {
        std::scoped_lock lk{a.m};
        a.packets.clear();
    }
    // The game may compute a /24 broadcast; the backend sends to the lobby's broadcast.
    a.backend->SendProxyPacket(Udp(a_ip, 12345, {10, 114, 0, 255}, 12345, {9, 9}, true));
    Check(b.WaitFor([&] { return !b.packets.empty() && b.packets.back().broadcast; }),
          "B receives A's broadcast (flagged broadcast)");
    Check(a.WaitFor([&] { return !a.packets.empty(); }), "A gets its own broadcast back (server)");
    std::vector<u8> big(8000, 0x5A);
    b.backend->SendProxyPacket(Udp(b_ip, 50000, a_ip, 12345, big));
    Check(a.WaitFor([&] { return !a.packets.empty() && a.packets.back().data == big; }),
          "8000-byte datagram intact");

    Check(ca.Reject(1, 5), "A rejects node 1 (server accepts)");
    Check(a.WaitFor([&] { return NodeCount(a.last_info) == 1; }), "A back to 1 node after reject");
    Check(!cb.Reject(0, 5), "a non-member can't reject");

    cb.DisconnectNetwork(); // the server dropped B already; clears B's own state
    const auto again = cb.Scan(Filter(kLcid));
    Check(again && again->size() == 1, "B scans again");
    if (again && !again->empty()) {
        Check(cb.Connect(JoinRequest("GuestB", (*again)[0])) == NetworkError::None, "B rejoins");
        {
            std::scoped_lock lk{b.m};
            b.changes = 0;
        }
        ca.DisconnectNetwork(); // host leaves = lobby closes
        Check(b.WaitFor([&] { return !b.last_connected && b.changes > 0; }, 5000),
              "B told the lobby closed (listener connected=false)");
        Check(!b.backend->IsConnected() && !cb.InNetwork(), "B no longer in a lobby");
    }
    const auto none = cb.Scan(Filter(kLcid));
    Check(none && none->empty(), "no lobby left");
    NetworkInfoBlob bogus = lobby;
    bogus[0x10] ^= 0xFF;
    Check(cb.Connect(JoinRequest("GuestB", bogus)) == NetworkError::ConnectNotFound,
          "joining a closed lobby: ConnectNotFound");
}

/// Host a lobby, then the server goes away (killed by the caller): no hang, clean errors.
void TestServerLoss(const std::string& host, u16 port) {
    std::printf("server loss: kill the server within 60 s\n");
    Node a{Client::Settings{host, port, "", "1.4.0"}};
    Client& ca = a.backend->client();
    Check(ca.CreateNetwork(CreateRequest("HostA"), {}) == NetworkError::None, "lobby created");
    Check(a.WaitFor([&] { return a.last_connected; }), "in the lobby");
    Check(a.WaitFor([&] { return !a.last_connected; }, 60000), "listener: connected=false");
    Check(!ca.IsConnected() && !a.backend->IsConnected(), "client and data plane report it");
    const auto t0 = std::chrono::steady_clock::now();
    Check(!ca.Scan(Filter(kLcid)), "scan fails");
    Check(ca.CreateNetwork(CreateRequest("HostA"), {}) == NetworkError::ConnectFailure,
          "create fails with ConnectFailure");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    Check(ms < 2 * Client::ConnectTimeoutMs + 1000,
          "failures return within the connect timeouts (" + std::to_string(ms) + " ms)");
    std::printf("  last error: %s\n", ca.LastError().c_str());
}

/// Join a lobby hosted by tools/ryuldn/p2p_host_sim.py (a Ryujinx-style host with its own P2P
/// proxy): the address comes from the host's proxy and datagrams go through it.
void TestP2pJoin(const std::string& host, u16 port) {
    std::printf("p2p join (run tools/ryuldn/p2p_host_sim.py first)\n");
    Node b{Client::Settings{host, port, "", "1.4.0"}};
    Client& cb = b.backend->client();
    const auto found = cb.Scan(Filter(kLcid));
    Check(found && found->size() == 1, "finds the P2P host's lobby");
    if (!found || found->empty()) {
        return;
    }
    const NetworkInfoBlob lobby = (*found)[0];
    u32 host_ip;
    std::memcpy(&host_ip, lobby.data() + 0x68, 4);
    Check(cb.Connect(JoinRequest("GuestB", lobby)) == NetworkError::None, "joins");
    Check(b.backend->IsConnected(), "has an address (from the host's proxy)");
    const auto my_ip = b.backend->GetFakeIpAddress();
    b.backend->SendProxyPacket(Udp(my_ip, 50000, Backend::ToAddress(host_ip), 7777, {'h', 'i'}));
    Check(b.WaitFor([&] {
              return !b.packets.empty() &&
                     b.packets.back().data == std::vector<u8>{'e', 'c', 'h', 'o', ':', 'h', 'i'};
          }),
          "the host echoed our datagram through its proxy");
    if (!b.packets.empty()) {
        const auto& p = b.packets.back();
        Check(Backend::ToU32(p.local_endpoint.ip) == host_ip && p.local_endpoint.portno == 7777 &&
                  p.remote_endpoint.portno == 50000,
              "echo comes from the host's address and port");
    }
    cb.DisconnectNetwork();
    Check(!b.backend->IsConnected(), "left the lobby");
}

/// Two servers: a lobby hosted on server 2 shows up in a scan of a node whose main server is
/// server 1 (with server 2 as an extra), the join goes through server 2, and an extra server
/// that is down doesn't slow scans after the first.
void TestMulti(const std::string& host1, u16 port1, const std::string& host2, u16 port2) {
    std::printf("multi server %s:%u + %s:%u\n", host1.c_str(), port1, host2.c_str(), port2);
    const Client::Settings s1{host1, port1, "", "1.4.0"};
    const Client::Settings s2{host2, port2, "", "1.4.0"};
    const Client::Settings dead{"127.0.0.1", 1, "", "1.4.0"};
    Node host{s2};
    Node local{s1}; // a lobby on the main server too
    Node b{std::vector<Client::Settings>{s1, s2, dead}};

    Check(host.backend->CreateNetwork(CreateRequest("HostOn2"), {}) == NetworkError::None,
          "host creates a lobby on server 2");
    Check(local.backend->CreateNetwork(CreateRequest("HostOn1"), {}) == NetworkError::None,
          "another host creates one on server 1");

    auto t0 = std::chrono::steady_clock::now();
    const auto found = b.backend->Scan(Filter(kLcid));
    const auto first_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
    Check(found && found->size() == 2, "B's scan merges both servers' lobbies");
    t0 = std::chrono::steady_clock::now();
    const auto again = b.backend->Scan(Filter(kLcid));
    const auto second_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    Check(again && again->size() == 2, "second scan: still both");
    Check(second_ms < Client::ScanTimeoutMs + 500,
          "a dead extra server doesn't slow later scans (" + std::to_string(first_ms) + " / " +
              std::to_string(second_ms) + " ms)");
    if (!again) {
        return;
    }
    const NetworkInfoBlob* on2 = nullptr;
    for (const auto& info : *again) {
        if (NodeName(info, 0) == "HostOn2") {
            on2 = &info;
        }
    }
    Check(on2 != nullptr, "the server 2 lobby is listed by its host's name");
    if (!on2) {
        return;
    }
    Check(b.backend->Connect(JoinRequest("GuestB", *on2)) == NetworkError::None,
          "B joins the server 2 lobby");
    Check(b.WaitFor([&] { return b.last_connected && NodeCount(b.last_info) == 2; }),
          "B's listener: 2 nodes");
    Check(b.backend->IsConnected(),
          "B has an address in that lobby");
    const auto h_ip = host.backend->GetFakeIpAddress();
    const auto b_ip = b.backend->GetFakeIpAddress();
    b.backend->SendProxyPacket(Udp(b_ip, 50000, h_ip, 12345, {4, 5, 6}));
    Check(host.WaitFor([&] { return !host.packets.empty(); }),
          "the server 2 host receives B's datagram");
    host.backend->SendProxyPacket(Udp(h_ip, 12345, b_ip, 50000, {7}));
    Check(b.WaitFor([&] { return !b.packets.empty(); }), "B receives the host's reply");
    b.backend->client().DisconnectNetwork();
    Check(!b.backend->IsConnected(), "B left");
    Check(b.backend->CreateNetwork(CreateRequest("HostB"), {}) == NetworkError::None,
          "B hosts: on its main server");
    const auto on1 = local.backend->client().Scan(Filter(kLcid));
    bool b_on_1 = false;
    for (const auto& info : on1.value_or(std::vector<NetworkInfoBlob>{})) {
        b_on_1 |= NodeName(info, 0) == "HostB";
    }
    Check(b_on_1, "B's lobby is on server 1");
    b.backend->CloseAll();
}

/// The servers change while the game runs (the menu's host/join): scans follow, a join works,
/// and switching is refused while in a lobby.
void TestSwitch(const std::string& host, u16 port1, u16 port2) {
    std::printf("switch servers live %u -> %u\n", port1, port2);
    const Client::Settings s1{host, port1, "", "1.4.0"};
    const Client::Settings s2{host, port2, "", "1.4.0"};
    Node lobby_host{s2};
    Check(lobby_host.backend->CreateNetwork(CreateRequest("HostOn2"), {}) == NetworkError::None,
          "a lobby on server 2");
    Node b{s1};
    const auto before = b.backend->Scan(Filter(kLcid));
    Check(before && before->empty(), "B on server 1 sees no lobby");
    std::string error;
    Check(b.backend->SetServers({s2}, error), "B switches to server 2 while running");
    Check(b.backend->settings().port == port2, "B's main server is now server 2");
    const auto after = b.backend->Scan(Filter(kLcid));
    Check(after && after->size() == 1, "B now sees the lobby");
    if (!after || after->empty()) {
        return;
    }
    Check(b.backend->Connect(JoinRequest("GuestB", (*after)[0])) == NetworkError::None,
          "B joins it");
    Check(!b.backend->SetServers({s1}, error) && error == "leave the lobby first",
          "switching is refused in a lobby");
    const auto h_ip = lobby_host.backend->GetFakeIpAddress();
    const auto b_ip = b.backend->GetFakeIpAddress();
    lobby_host.backend->SendProxyPacket(Udp(h_ip, 1, b_ip, 2, {42}));
    Check(b.WaitFor([&] { return !b.packets.empty(); }), "B gets the host's datagram");
    b.backend->client().DisconnectNetwork();
    Check(b.backend->SetServers({s1}, error), "after leaving, switching back works");
    const auto back = b.backend->Scan(Filter(kLcid));
    Check(back && back->empty(), "back on server 1: no lobby again");
}

/// The in-game server goes away under a lobby (host switches it off): clean loss, restartable.
void TestOwnServerStop(const std::string& host, u16 port) {
    std::printf("own server stop/restart on %u\n", port);
    Server server;
    std::string error;
    Check(server.Start(host, port, error), "server starts");
    Node a{Client::Settings{host, port, "", "1.4.0"}};
    Check(a.backend->CreateNetwork(CreateRequest("HostA"), {}) == NetworkError::None,
          "lobby created on it");
    const auto st = server.GetStatus();
    Check(st.running && st.lobbies == 1 && st.players == 1 && st.connections == 1,
          "status: 1 connection, 1 lobby, 1 player");
    server.Stop();
    Check(a.WaitFor([&] { return !a.last_connected; }, 5000), "lobby lost after Stop");
    Check(!server.GetStatus().running, "status: stopped");
    Check(server.Start(host, port, error), "starts again on the same port");
    Server second;
    std::string busy;
    Check(!second.Start(host, port, busy) && !busy.empty(),
          "a second server on that port fails with a message");
    Check(a.backend->CreateNetwork(CreateRequest("HostA"), {}) == NetworkError::None,
          "a new lobby after the restart");
    server.Stop();
}

/// A host offering a P2P proxy is told PortUnreachable and relayed by the server instead.
void TestOwnServerP2pRequest(const std::string& host, u16 port) {
    std::printf("p2p request on own server %u\n", port);
    Server server;
    std::string error;
    server.Start(host, port, error);
    Client::Settings s{host, port, "", "1.4.0"};
    Node a{s}, b{s};
    auto req = CreateRequest("P2pHost");
    req.ryu_network_config.external_proxy_port = 39990;
    Check(a.backend->CreateNetwork(req, {}) == NetworkError::None, "P2P-style create accepted");
    const auto found = b.backend->Scan(Filter(kLcid));
    Check(found && found->size() == 1, "listed");
    if (found && !found->empty()) {
        Check(b.backend->Connect(JoinRequest("GuestB", (*found)[0])) == NetworkError::None,
              "join goes through the server relay");
        const auto a_ip = a.backend->GetFakeIpAddress();
        const auto b_ip = b.backend->GetFakeIpAddress();
        b.backend->SendProxyPacket(Udp(b_ip, 5, a_ip, 6, {1}));
        Check(a.WaitFor([&] { return !a.packets.empty(); }), "relayed datagram arrives");
    }
    server.Stop();
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress shows even if a check hangs
    std::string fixtures, server, multi;
    bool server_loss = false;
    bool p2p_join = false;
    bool own_server = false;
    int serve_port = 0, serve_seconds = 0;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            server = argv[++i];
        } else if (arg == "--server-loss") {
            server_loss = true;
        } else if (arg == "--multi" && i + 1 < argc) {
            multi = argv[++i]; // host:port,host:port
        } else if (arg == "--own-server") {
            own_server = true; // the in-game server, in this process
        } else if (arg == "--serve" && i + 2 < argc) {
            serve_port = std::stoi(argv[++i]); // run the in-game server for a while (probe)
            serve_seconds = std::stoi(argv[++i]);
        } else if (arg == "--p2p-join") {
            p2p_join = true;
        } else {
            fixtures = arg;
        }
    }
    std::string host = "127.0.0.1";
    u16 port = DefaultPort;
    if (!server.empty()) {
        const auto colon = server.rfind(':');
        host = server.substr(0, colon);
        if (colon != std::string::npos) {
            port = static_cast<u16>(std::stoi(server.substr(colon + 1)));
        }
    }
    const auto split = [](const std::string& hp) {
        const auto colon = hp.rfind(':');
        return std::pair<std::string, u16>{hp.substr(0, colon),
                                           static_cast<u16>(std::stoi(hp.substr(colon + 1)))};
    };
    if (serve_port != 0) {
        Server served;
        std::string error;
        if (!served.Start("127.0.0.1", static_cast<u16>(serve_port), error)) {
            std::printf("can't serve: %s\n", error.c_str());
            return 1;
        }
        std::printf("serving on %d for %d s\n", serve_port, serve_seconds);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(serve_seconds));
        served.Stop();
        return 0;
    }
    if (own_server) {
        constexpr u16 kBase = 30470;
        Server one, two;
        std::string error;
        Check(one.Start("127.0.0.1", kBase, error) && two.Start("127.0.0.1", kBase + 1, error),
              "two in-game servers start");
        TestLive("127.0.0.1", kBase);
        TestMulti("127.0.0.1", kBase, "127.0.0.1", kBase + 1);
        TestSwitch("127.0.0.1", kBase, kBase + 1);
        one.Stop();
        two.Stop();
        TestOwnServerStop("127.0.0.1", kBase + 2);
        TestOwnServerP2pRequest("127.0.0.1", kBase + 3);
    } else if (!multi.empty()) {
        const auto comma = multi.find(',');
        const auto [h1, p1] = split(multi.substr(0, comma));
        const auto [h2, p2] = split(multi.substr(comma + 1));
        TestMulti(h1, p1, h2, p2);
    } else if (server_loss) {
        TestServerLoss(host, port);
    } else if (p2p_join) {
        TestP2pJoin(host, port);
    } else {
        TestRoundTrips();
        if (!fixtures.empty()) {
            TestFixtures(fixtures);
        } else {
            std::printf("(no fixtures file given: skipping the recorded-session checks)\n");
        }
        TestMalformed();
        if (!server.empty()) {
            TestLive(host, port);
        }
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
