// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/socket_types.h"

namespace Network {

struct ProxyPacket;

/**
 * Socket census for local wireless (LDN) play: which sockets the game opens (protocol, proxy or
 * real), what it does with the proxied ones (bind ports, any TCP connect/listen/accept), and every
 * proxied packet by flow (source -> destination, broadcast or not) with counts and sizes.
 * Answers "TCP or UDP only?" and "does a guest talk to other guests or only to the host?".
 * Written to ldn_census.txt in the user folder every 10 s while game_settings.ini
 * [Debug] diagnostics = true; does nothing otherwise.
 */
namespace LdnCensus {

bool Enabled();

/// A socket was created (guest BSD values: type 1 stream / 2 datagram, protocol 6 TCP / 17 UDP).
void OnSocket(s32 fd, u32 domain, u32 type, u32 protocol, bool proxy);

/// A call on a socket: "bind", "connect", "listen", "accept", "send", "recv", "close"...
void OnCall(s32 fd, const char* what, const SockAddrIn* addr = nullptr);

/// A proxied packet leaving this node, or arriving at one of its sockets.
void OnPacketOut(const ProxyPacket& packet);
void OnPacketIn(const ProxyPacket& packet);

/// The LDN state machine changed (host: access point created; guest: station connected...).
void OnLdnState(const char* state, IPv4Address own_ip, int node_count);

} // namespace LdnCensus

} // namespace Network
