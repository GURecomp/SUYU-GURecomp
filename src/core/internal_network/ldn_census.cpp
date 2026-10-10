// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "core/arm/recomp/game_settings.h"
#include "core/internal_network/ldn_census.h"
#include "network/room_member.h"

namespace Network::LdnCensus {

namespace {

struct SocketInfo {
    double created;
    u32 domain, type, protocol;
    bool proxy;
    std::map<std::string, u64> calls; // "bind 0.0.0.0:12345" / "send" -> count
};

struct Flow {
    u64 packets = 0, bytes = 0;
    size_t min_size = SIZE_MAX, max_size = 0;
    double first = 0, last = 0;
};

// (protocol, source ip, source port, destination ip, destination port, broadcast flag)
using FlowKey = std::tuple<int, u32, u16, u32, u16, bool>;

std::mutex g_lock;
const auto g_start = std::chrono::steady_clock::now();
std::map<s32, std::vector<SocketInfo>> g_sockets; // fd -> every socket that used it
std::map<FlowKey, Flow> g_out, g_in;
std::vector<std::string> g_ldn_states;
bool g_writer_started = false;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

u32 IpToU32(const IPv4Address& ip) {
    return (u32{ip[0]} << 24) | (u32{ip[1]} << 16) | (u32{ip[2]} << 8) | ip[3];
}

std::string IpStr(u32 ip) {
    return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

const char* ProtoName(int p) {
    switch (p) {
    case static_cast<int>(Protocol::UDP):
        return "udp";
    case static_cast<int>(Protocol::TCP):
        return "tcp";
    default:
        return "?";
    }
}

void Record(std::map<FlowKey, Flow>& flows, const ProxyPacket& p) {
    const FlowKey key{static_cast<int>(p.protocol),     IpToU32(p.local_endpoint.ip),
                      p.local_endpoint.portno,          IpToU32(p.remote_endpoint.ip),
                      p.remote_endpoint.portno,         p.broadcast};
    Flow& f = flows[key];
    const double now = Now();
    if (f.packets == 0) {
        f.first = now;
    }
    f.last = now;
    f.packets++;
    f.bytes += p.data.size();
    f.min_size = std::min(f.min_size, p.data.size());
    f.max_size = std::max(f.max_size, p.data.size());
}

void WriteFlows(std::ofstream& out, const std::map<FlowKey, Flow>& flows) {
    for (const auto& [k, f] : flows) {
        const auto& [proto, sip, sport, dip, dport, bcast] = k;
        out << fmt::format("{}\t{}:{}\t{}:{}\t{}\t{}\t{}\t{}..{}\t{:.1f}..{:.1f}\n", ProtoName(proto),
                           IpStr(sip), sport, IpStr(dip), dport, bcast ? "bcast" : "-", f.packets,
                           f.bytes, f.min_size, f.max_size, f.first, f.last);
    }
}

void WriteLocked() {
    const auto path = Common::FS::GetSuyuPath(Common::FS::SuyuPath::SuyuDir) / "ldn_census.txt";
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# LDN socket census ([Debug] diagnostics): sockets the game opened, proxied packets "
           "by flow. Packet sizes are the game's payload (before the room's compression).\n";
    out << fmt::format("# uptime {:.1f} s\n", Now());

    // Summary: TCP over the LDN proxy? Unicast peers per source (star = guests only talk to
    // the host; mesh = guests also talk to each other).
    bool proxy_tcp = false;
    for (const auto& [fd, list] : g_sockets) {
        for (const SocketInfo& s : list) {
            if (s.proxy && (s.type == 1 || s.protocol == 6)) {
                proxy_tcp = true;
            }
        }
    }
    std::map<u32, std::set<u32>> peers; // source ip -> unicast destination ips (out and in)
    u64 bcast_out = 0;
    for (const auto* flows : {&g_out, &g_in}) {
        for (const auto& [k, f] : *flows) {
            const auto& [proto, sip, sport, dip, dport, bcast] = k;
            if (bcast || (dip & 0xFF) == 0xFF) {
                if (flows == &g_out) {
                    bcast_out += f.packets;
                }
                continue;
            }
            peers[sip].insert(dip);
        }
    }
    out << fmt::format("# summary: proxied TCP sockets: {}; broadcast packets sent: {}\n",
                       proxy_tcp ? "YES" : "none", bcast_out);
    for (const auto& [src, dsts] : peers) {
        std::string list;
        for (u32 d : dsts) {
            list += (list.empty() ? "" : ", ") + IpStr(d);
        }
        out << fmt::format("# unicast {} -> {}\n", IpStr(src), list);
    }

    out << "\n## LDN states: at_s\tstate\town_ip\tnodes\n";
    for (const std::string& s : g_ldn_states) {
        out << s << '\n';
    }
    out << "\n## sockets: fd\tcreated_s\tdomain/type/protocol\tproxy\tcalls (count)\n";
    for (const auto& [fd, list] : g_sockets) {
        for (const SocketInfo& s : list) {
            std::string calls;
            for (const auto& [what, n] : s.calls) {
                calls += fmt::format("{}{} x{}", calls.empty() ? "" : "; ", what, n);
            }
            out << fmt::format("{}\t{:.1f}\t{}/{}/{}\t{}\t{}\n", fd, s.created, s.domain, s.type,
                               s.protocol, s.proxy ? "proxy" : "real", calls);
        }
    }
    out << "\n## packets out: proto\tsource\tdestination\tbcast\tpackets\tbytes\tsize min..max\t"
           "seen_s\n";
    WriteFlows(out, g_out);
    out << "\n## packets in (accepted by a socket): same columns\n";
    WriteFlows(out, g_in);
}

void StartWriterLocked() {
    if (g_writer_started) {
        return;
    }
    g_writer_started = true;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::scoped_lock lk{g_lock};
            WriteLocked();
        }
    }).detach();
}

} // namespace

bool Enabled() {
    static const bool enabled = Core::GameSettings::DiagnosticsEnabled();
    return enabled;
}

void OnSocket(s32 fd, u32 domain, u32 type, u32 protocol, bool proxy) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lk{g_lock};
    StartWriterLocked();
    g_sockets[fd].push_back(SocketInfo{Now(), domain, type, protocol, proxy, {}});
}

void OnCall(s32 fd, const char* what, const SockAddrIn* addr) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lk{g_lock};
    auto it = g_sockets.find(fd);
    if (it == g_sockets.end() || it->second.empty()) {
        return;
    }
    std::string key = what;
    if (addr) {
        key += fmt::format(" {}:{}", IpStr(IpToU32(addr->ip)), addr->portno);
    }
    it->second.back().calls[key]++;
}

void OnPacketOut(const ProxyPacket& packet) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lk{g_lock};
    StartWriterLocked();
    Record(g_out, packet);
}

void OnPacketIn(const ProxyPacket& packet) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lk{g_lock};
    StartWriterLocked();
    Record(g_in, packet);
}

void OnLdnState(const char* state, IPv4Address own_ip, int node_count) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lk{g_lock};
    StartWriterLocked();
    g_ldn_states.push_back(
        fmt::format("{:.1f}\t{}\t{}\t{}", Now(), state, IpStr(IpToU32(own_ip)), node_count));
}

} // namespace Network::LdnCensus
