#!/usr/bin/env python3
"""RyuLdn protocol probe: drives a LdnServer (Ryujinx LDN master server) with two
or three fake clients and checks every step of the lobby + relay flow.

Wire format (measured from the server's C# structs with Unsafe.SizeOf / Marshal.OffsetOf):
  header 12 bytes: u32 magic 'RLDN' (LE), u8 type, u8 version (1), 2 pad, i32 data size
  ConnectRequest is 0x500 (NetworkInfo at +0x80), ConnectPrivateRequest 0xC0,
  not the 0x4FC / 0xBC the StructLayout attributes declare.

Usage: ryuldn_probe.py [--host 127.0.0.1] [--port 30456] [--fixtures out.json]
Exit code 0 = every check passed.
"""
import argparse
import json
import queue
import socket
import struct
import sys
import threading
import time

MAGIC = 0x4E444C52  # 'R' | 'L'<<8 | 'D'<<16 | 'N'<<24
VERSION = 1
HDR = struct.Struct("<IBBxxi")  # 12 bytes

# PacketId (server Network/Types/PacketId.cs, implicit values from 0)
P = dict(Initialize=0, Passphrase=1, CreateAccessPoint=2, CreateAccessPointPrivate=3,
         ExternalProxy=4, ExternalProxyToken=5, ExternalProxyState=6, SyncNetwork=7,
         Reject=8, RejectReply=9, Scan=10, ScanReply=11, ScanReplyEnd=12, Connect=13,
         ConnectPrivate=14, Connected=15, Disconnect=16, ProxyConfig=17, ProxyConnect=18,
         ProxyConnectReply=19, ProxyData=20, ProxyDisconnect=21, SetAcceptPolicy=22,
         SetAdvertiseData=23, Ping=254, NetworkError=255)
PNAME = {v: k for k, v in P.items()}
NETERR = {0: "None", 1: "PortUnreachable", 2: "TooManyPlayers", 3: "VersionTooLow",
          4: "VersionTooHigh", 5: "ConnectFailure", 6: "ConnectNotFound", 7: "ConnectTimeout",
          8: "ConnectRejected", 9: "RejectFailed", 127: "BannedByServer", -1: "Unknown"}

# Game constants. LocalCommunicationId = NACP LocalCommunicationId[0]; for MHGU assumed to be
# the title id (UNVERIFIED until read from the game's control data).
MHGU_LCID = 0x0100770008DD8000
NETINFO_SIZE = 0x480
UDP = 17
BROADCAST = 0x0A72FFFF  # 10.114.255.255 (server VirtualDhcp: 10.114.0.0/16)

SIZES = dict(InitializeMessage=0x16, PassphraseMessage=0x80, PingMessage=2, DisconnectMessage=4,
             NetworkErrorMessage=4, RejectRequest=8, SetAcceptPolicyRequest=1, ProxyInfo=0x10,
             ProxyDataHeader=0x14, ProxyConfig=8, RyuNetworkConfig=0x28,
             CreateAccessPointRequest=0xBC, CreateAccessPointPrivateRequest=0x13C,
             ConnectRequest=0x500, ConnectPrivateRequest=0xC0, ScanFilter=0x60, NetworkInfo=0x480)


def frame(ptype, payload=b""):
    return HDR.pack(MAGIC, ptype, VERSION, len(payload)) + payload


# ---- struct builders (offsets measured from the server types) ----
def security_config(mode=1, passphrase=b""):            # 0x44
    return struct.pack("<HH", mode, len(passphrase)) + passphrase.ljust(0x40, b"\0")


def user_config(name):                                  # 0x30
    return name.encode()[:32].ljust(33, b"\0") + bytes(15)


def intent_id(lcid, scene=0):                           # 0x10
    return struct.pack("<qHHI", lcid, 0, scene, 0)


def network_config(lcid, channel=1, node_max=4, lcv=0):  # 0x20
    return intent_id(lcid) + struct.pack("<HBBH", channel, node_max, 0, lcv) + bytes(10)


def ryu_network_config(game_version=b"1.4.0"):          # 0x28, no p2p proxy
    return game_version.ljust(16, b"\0") + bytes(16) + struct.pack("<iHH", 0, 0, 0)


def create_access_point(name, lcid, node_max=4, lcv=0, adv=b""):
    body = security_config() + user_config(name) + network_config(lcid, node_max=node_max, lcv=lcv) + ryu_network_config()
    assert len(body) == SIZES["CreateAccessPointRequest"]
    return body + adv


def scan_filter(lcid):                                  # 0x60, flag LocalCommunicationId
    nid = intent_id(lcid) + bytes(16)
    body = nid + struct.pack("<I", 0) + bytes(6) + bytes(0x22) + bytes(16) + bytes([1]) + bytes(3)
    assert len(body) == SIZES["ScanFilter"], hex(len(body))
    return body


def connect_request(name, netinfo, lcv=0):
    body = security_config() + user_config(name) + struct.pack("<II", lcv, 0) + bytes(4) + netinfo
    assert len(body) == SIZES["ConnectRequest"], hex(len(body))
    return body


def proxy_info(src_ip, src_port, dst_ip, dst_port, proto=UDP):
    return struct.pack("<IHIHi", src_ip, src_port, dst_ip, dst_port, proto)


def proxy_data(src_ip, src_port, dst_ip, dst_port, data, proto=UDP):
    return proxy_info(src_ip, src_port, dst_ip, dst_port, proto) + struct.pack("<I", len(data)) + data


# ---- parsers ----
def parse_netinfo(b):
    assert len(b) >= NETINFO_SIZE, len(b)
    lcid, _, scene, _ = struct.unpack_from("<qHHI", b, 0)
    session = b[0x10:0x20]
    common_mac = b[0x20:0x26]
    ssid_len = b[0x26]
    channel, link, ntype = struct.unpack_from("<HBB", b, 0x48)
    ldn = 0x50
    sec_mode, accept, _, _, node_max, node_count = struct.unpack_from("<HBBHBB", b, ldn + 0x10)
    nodes = []
    for i in range(8):
        o = ldn + 0x18 + i * 0x40
        ip, = struct.unpack_from("<I", b, o)
        node = dict(ip=ip, mac=b[o + 4:o + 10].hex(), id=b[o + 10], connected=b[o + 11],
                    name=b[o + 12:o + 45].split(b"\0")[0].decode(errors="replace"),
                    lcv=struct.unpack_from("<H", b, o + 0x2E)[0])
        nodes.append(node)
    adv_size, = struct.unpack_from("<H", b, ldn + 0x21A)
    adv = b[ldn + 0x21C:ldn + 0x21C + adv_size]
    return dict(lcid=lcid, scene=scene, session=session.hex(), mac=common_mac.hex(), ssid_len=ssid_len,
                channel=channel, link=link, net_type=ntype, sec_mode=sec_mode, accept=accept,
                node_max=node_max, node_count=node_count, nodes=nodes, adv=adv)


def ipstr(ip):
    return ".".join(str((ip >> s) & 0xFF) for s in (24, 16, 8, 0))


class Client:
    def __init__(self, name, host, port, log):
        self.name = name
        self.log = log
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(None)  # the reader blocks; idle periods are normal
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.q = queue.Queue()
        self.alive = True
        self.ip = 0
        self.mask = 0
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        try:
            while self.alive:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while len(buf) >= HDR.size:
                    magic, ptype, ver, size = HDR.unpack_from(buf)
                    if magic != MAGIC or ver != VERSION or size < 0 or size > 131072:
                        raise RuntimeError(f"bad header {buf[:12].hex()}")
                    if len(buf) < HDR.size + size:
                        break
                    raw = buf[:HDR.size + size]
                    body = raw[HDR.size:]
                    buf = buf[HDR.size + size:]
                    self.log.append(dict(dir="in", client=self.name, type=PNAME.get(ptype, ptype), hex=raw.hex()))
                    if ptype == P["Ping"] and body[0] == 0:
                        self.send(P["Ping"], body)  # echo server pings (requester 0)
                        continue
                    self.q.put((ptype, body))
        except OSError:
            pass
        self.q.put((None, b""))

    def send(self, ptype, payload=b""):
        raw = frame(ptype, payload)
        self.log.append(dict(dir="out", client=self.name, type=PNAME.get(ptype, ptype), hex=raw.hex()))
        self.sock.sendall(raw)

    def expect(self, *types, timeout=3.0):
        """Next packet must be one of types; returns (type, body)."""
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError(f"{self.name}: waiting for {[PNAME[t] for t in types]}")
            try:
                ptype, body = self.q.get(timeout=left)
            except queue.Empty:
                continue
            if ptype in types:
                return ptype, body
            raise AssertionError(f"{self.name}: got {PNAME.get(ptype, ptype)} while waiting for {[PNAME[t] for t in types]}"
                                 + (f" ({NETERR.get(struct.unpack('<i', body)[0])})" if ptype == P["NetworkError"] else ""))

    def quiet(self, wait=0.4):
        """Nothing else may arrive within wait seconds."""
        try:
            ptype, body = self.q.get(timeout=wait)
        except queue.Empty:
            return
        raise AssertionError(f"{self.name}: unexpected {PNAME.get(ptype, ptype)}")

    def handshake(self):
        self.send(P["Initialize"], bytes(SIZES["InitializeMessage"]))
        _, body = self.expect(P["Initialize"])
        assert len(body) == 0x16
        self.id, self.mac = body[:16].hex(), body[16:22].hex()
        self.send(P["Passphrase"], bytes(0x80))  # empty passphrase = public lobbies

    def take_proxy_config(self):
        _, body = self.expect(P["ProxyConfig"])
        self.ip, self.mask = struct.unpack("<II", body)

    def close(self):
        self.alive = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        raise AssertionError(what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=30456)
    ap.add_argument("--fixtures", help="write every frame (hex) to this JSON file")
    args = ap.parse_args()
    log = []
    clients = []
    try:
        print("1. handshake (Initialize + Passphrase)")
        a = Client("A", args.host, args.port, log); clients.append(a)
        b = Client("B", args.host, args.port, log); clients.append(b)
        a.handshake(); b.handshake()
        check(a.id != "00" * 16 and a.mac != "00" * 6, f"A got id {a.id[:8]}.. and mac {a.mac}")
        check(a.mac != b.mac, "A and B have different macs")

        print("2. A creates an access point (MHGU, 4 players, advertise data)")
        adv1 = b"MHGU-probe-adv-1"
        a.send(P["CreateAccessPoint"], create_access_point("HostA", MHGU_LCID, 4, 0, adv1))
        a.take_proxy_config()
        check((a.ip & 0xFFFF0000) == 0x0A720000 and a.mask == 0xFFFF0000, f"A ProxyConfig ip {ipstr(a.ip)} mask {ipstr(a.mask)}")
        _, body = a.expect(P["Connected"])
        info = parse_netinfo(body)
        check(len(body) == NETINFO_SIZE, "Connected carries a 0x480 NetworkInfo")
        check(info["lcid"] == MHGU_LCID and info["node_count"] == 1 and info["node_max"] == 4, "lcid / 1 node / max 4")
        check(info["nodes"][0]["ip"] == a.ip and info["nodes"][0]["name"] == "HostA" and info["nodes"][0]["connected"] == 1, "node 0 = A")
        check(info["adv"] == adv1, "advertise data stored")
        check(info["mac"] == a.mac, "network mac = host mac")
        a.quiet()

        print("3. B scans (filter LocalCommunicationId); a wrong id finds nothing")
        b.send(P["Scan"], scan_filter(MHGU_LCID))
        _, body = b.expect(P["ScanReply"])
        found = parse_netinfo(body)
        b.expect(P["ScanReplyEnd"])
        check(found["session"] == info["session"] and found["node_count"] == 1, f"found session {found['session'][:8]}..")
        b.send(P["Scan"], scan_filter(0x0100000000001000))
        b.expect(P["ScanReplyEnd"])
        check(True, "other title: only ScanReplyEnd")

        print("4. B connects with a too-high local communication version -> VersionTooHigh")
        b.send(P["Connect"], connect_request("GuestB", body, lcv=1))
        _, err = b.expect(P["NetworkError"])
        check(struct.unpack("<i", err)[0] == 4, "NetworkError VersionTooHigh")

        print("5. B connects to an unknown session -> ConnectNotFound")
        bogus = bytearray(body); bogus[0x10:0x20] = bytes(range(16))
        b.send(P["Connect"], connect_request("GuestB", bytes(bogus)))
        _, err = b.expect(P["NetworkError"])
        check(struct.unpack("<i", err)[0] == 6, "NetworkError ConnectNotFound")

        print("6. B connects")
        b.send(P["Connect"], connect_request("GuestB", body))
        b.take_proxy_config()
        check(b.ip != a.ip and (b.ip & 0xFFFF0000) == 0x0A720000, f"B ip {ipstr(b.ip)}")
        _, sbody = a.expect(P["SyncNetwork"])
        s = parse_netinfo(sbody)
        check(s["node_count"] == 2 and s["nodes"][1]["ip"] == b.ip and s["nodes"][1]["name"] == "GuestB", "A sees 2 nodes (B = node 1)")
        _, cbody = b.expect(P["Connected"])
        c = parse_netinfo(cbody)
        check(c["node_count"] == 2 and c["nodes"][0]["name"] == "HostA", "B Connected with 2 nodes")

        print("7. A changes advertise data -> both get SyncNetwork")
        adv2 = b"second-advert"
        a.send(P["SetAdvertiseData"], adv2)
        for cl in (a, b):
            _, sb = cl.expect(P["SyncNetwork"])
            check(parse_netinfo(sb)["adv"] == adv2, f"{cl.name} sees new advertise data")
        b.send(P["SetAdvertiseData"], b"not-owner")
        b.quiet(); a.quiet()
        check(True, "non-owner SetAdvertiseData ignored")

        print("8. relay: unicast B -> A, src 0 filled in, broadcast reaches everyone (sender too)")
        b.send(P["ProxyData"], proxy_data(0, 50000, a.ip, 12345, b"hello A"))
        _, d = a.expect(P["ProxyData"])
        src, sport, dst, dport, proto, n = struct.unpack_from("<IHIHiI", d)
        check(src == b.ip and sport == 50000 and dst == a.ip and dport == 12345 and proto == UDP and d[20:] == b"hello A" and n == 7,
              f"A got {n} bytes from {ipstr(src)}:{sport}")
        b.quiet(0.2)
        a.send(P["ProxyData"], proxy_data(a.ip, 12345, BROADCAST, 12345, b"bcast"))
        for cl in (a, b):
            _, d = cl.expect(P["ProxyData"])
            check(d[20:] == b"bcast" and struct.unpack_from("<I", d, 6)[0] == BROADCAST, f"{cl.name} got the broadcast")
        a.send(P["ProxyData"], proxy_data(a.ip, 12345, 0xC0A800FF, 12345, b"b2"))
        for cl in (a, b):
            _, d = cl.expect(P["ProxyData"])
            check(d[20:] == b"b2", f"{cl.name} got 192.168.0.255 as broadcast (dest kept as sent: {ipstr(struct.unpack_from('<I', d, 6)[0])})")
        b.send(P["ProxyData"], proxy_data(a.ip, 1, b.ip, 2, b"spoof"))
        b.quiet(0.3); a.quiet(0.1)
        check(True, "spoofed source dropped")
        big = bytes(range(256)) * 32  # 8 KB datagram
        b.send(P["ProxyData"], proxy_data(0, 50000, a.ip, 12345, big))
        _, d = a.expect(P["ProxyData"])
        check(d[20:] == big, "8 KB payload intact")

        print("9. third client C joins and leaves; A and B see it")
        c3 = Client("C", args.host, args.port, log); clients.append(c3)
        c3.handshake()
        c3.send(P["Connect"], connect_request("GuestC", body))
        c3.take_proxy_config()
        for cl in (a, b):
            _, sb = cl.expect(P["SyncNetwork"])
            check(parse_netinfo(sb)["node_count"] == 3, f"{cl.name} sees 3 nodes")
        c3.expect(P["Connected"])
        c3.send(P["Disconnect"], bytes(4))
        for cl in (a, b):
            _, sb = cl.expect(P["SyncNetwork"])
            si = parse_netinfo(sb)
            check(si["node_count"] == 2 and si["nodes"][2]["connected"] == 0, f"{cl.name}: C gone, node 2 marked disconnected")
        c3.quiet()

        print("10. B drops TCP without Disconnect -> A sees it leave")
        b.close()
        _, sb = a.expect(P["SyncNetwork"])
        check(parse_netinfo(sb)["node_count"] == 1, "A back to 1 node")

        print("11. B2 reconnects, then host A disconnects -> B2 gets Disconnect, lobby gone")
        b2 = Client("B2", args.host, args.port, log); clients.append(b2)
        b2.handshake()
        b2.send(P["Connect"], connect_request("GuestB2", body))
        b2.take_proxy_config()
        a.expect(P["SyncNetwork"]); b2.expect(P["Connected"])
        a.send(P["Disconnect"], bytes(4))
        # the server may first send B2 a SyncNetwork without A (host removal runs on its own task)
        while b2.expect(P["Disconnect"], P["SyncNetwork"], timeout=5)[0] != P["Disconnect"]:
            pass
        check(True, "B2 received Disconnect (lobby closed)")
        b2.send(P["Scan"], scan_filter(MHGU_LCID))
        b2.expect(P["ScanReplyEnd"])
        check(True, "scan after close finds nothing")

        print("12. idle ping: server pings after ~10-20 s idle; we echo and stay connected")
        t0 = time.time()
        while time.time() - t0 < 25:
            if any(e["type"] == "Ping" and e["dir"] == "in" and e["client"] == "B2" for e in log):
                break
            time.sleep(0.5)
        check(any(e["type"] == "Ping" and e["dir"] == "out" and e["client"] == "B2" for e in log), "B2 got and answered a Ping")
        b2.send(P["Scan"], scan_filter(MHGU_LCID)); b2.expect(P["ScanReplyEnd"])
        check(True, "still connected after the ping")
        print("ALL CHECKS PASSED")
        rc = 0
    except (AssertionError, TimeoutError, OSError, RuntimeError) as e:
        print("FAILED:", e)
        rc = 1
    finally:
        for cl in clients:
            cl.close()
        if args.fixtures:
            with open(args.fixtures, "w") as f:
                json.dump(dict(header_size=HDR.size, sizes=SIZES, frames=log), f, indent=1)
    sys.exit(rc)


if __name__ == "__main__":
    main()
