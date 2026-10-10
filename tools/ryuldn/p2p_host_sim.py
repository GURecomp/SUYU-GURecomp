#!/usr/bin/env python3
"""Stand-in for a Ryujinx host that runs its own P2P proxy (what Ryujinx does when UPnP works),
to test joining such a lobby without Ryujinx.

It hosts an MHGU lobby on a LdnServer with RyuNetworkConfig.ExternalProxyPort set, serves the
P2P proxy on 127.0.0.1:<proxy-port> like Ryujinx's P2pProxyServer (token check, ProxyConfig with
the virtual IP the master assigned, ProxyData routing incl. broadcast), and echoes every UDP
datagram sent to the host's own address back to its sender with b"echo:" prepended.

Usage: p2p_host_sim.py [--host 127.0.0.1] [--port 30456] [--proxy-port 39990] [--seconds 120]
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ryuldn_probe import (HDR, MAGIC, VERSION, P, PNAME, Client, frame, security_config,  # noqa: E402
                          user_config, network_config, MHGU_LCID, proxy_data, ipstr)


def read_frames(sock):
    buf = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return
        buf += chunk
        while len(buf) >= HDR.size:
            magic, ptype, ver, size = HDR.unpack_from(buf)
            if magic != MAGIC or ver != VERSION:
                raise RuntimeError("bad frame")
            if len(buf) < HDR.size + size:
                break
            yield ptype, buf[HDR.size:HDR.size + size]
            buf = buf[HDR.size + size:]


class ProxyServer:
    """Ryujinx P2pProxyServer behaviour: authenticate by token, assign the virtual IP, route."""

    def __init__(self, port):
        self.tokens = []  # (virtual_ip, token16)
        self.sessions = {}  # virtual ip -> socket
        self.lock = threading.Lock()
        self.broadcast = None
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(8)
        threading.Thread(target=self.accept_loop, daemon=True).start()

    def accept_loop(self):
        while True:
            conn, _ = self.srv.accept()
            threading.Thread(target=self.session, args=(conn,), daemon=True).start()

    def session(self, conn):
        vip = None
        try:
            for ptype, body in read_frames(conn):
                if ptype == P["ExternalProxy"] and vip is None:
                    token = body[0x16:0x26]
                    deadline = time.time() + 1.0
                    while vip is None and time.time() < deadline:
                        with self.lock:
                            for t in self.tokens:
                                if t[1] == token:
                                    vip = t[0]
                                    self.tokens.remove(t)
                                    break
                        if vip is None:
                            time.sleep(0.02)
                    if vip is None:
                        print("  proxy: unknown token, closing")
                        return
                    with self.lock:
                        self.sessions[vip] = conn
                        if self.broadcast is None:
                            self.broadcast = vip | 0x0000FFFF
                    conn.sendall(frame(P["ProxyConfig"], struct.pack("<II", vip, 0xFFFF0000)))
                    print(f"  proxy: {ipstr(vip)} authenticated")
                elif ptype == P["ProxyData"] and vip is not None:
                    src, sport, dst, dport, proto, n = struct.unpack_from("<IHIHiI", body)
                    if src == 0:
                        src = vip
                    elif src != vip:
                        continue
                    data = body[20:]
                    out = frame(P["ProxyData"], proxy_data(src, sport, dst, dport, data, proto))
                    with self.lock:
                        targets = list(self.sessions.items())
                    for ip, s in targets:
                        if dst in (self.broadcast, 0xC0A800FF) or ip == dst:
                            s.sendall(out)
        except OSError:
            pass
        finally:
            with self.lock:
                if vip is not None:
                    self.sessions.pop(vip, None)
            conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=30456)
    ap.add_argument("--proxy-port", type=int, default=39990)
    ap.add_argument("--seconds", type=int, default=120)
    args = ap.parse_args()

    proxy = ProxyServer(args.proxy_port)
    log = []
    master = Client("P2PHost", args.host, args.port, log)
    master.handshake()

    # RyuNetworkConfig: version, private ip (127.0.0.1), family 2, external + internal port.
    ryu = b"1.4.0".ljust(16, b"\0") + bytes([127, 0, 0, 1]).ljust(16, b"\0") + struct.pack(
        "<iHH", 2, args.proxy_port, args.proxy_port)
    request = security_config() + user_config("RyuP2PHost") + network_config(MHGU_LCID) + ryu
    master.send(P["CreateAccessPoint"], request + b"p2p-advert")

    own = None  # the host's own link to its proxy, like Ryujinx (it joins its own lobby)
    joined = False
    end = time.time() + args.seconds
    while time.time() < end:
        try:
            ptype, body = master.q.get(timeout=0.5)
        except Exception:
            continue
        if ptype is None:
            print("master connection closed")
            return 1
        name = PNAME.get(ptype, ptype)
        if ptype == P["ExternalProxyToken"]:
            vip, token = struct.unpack_from("<I", body)[0], body[4:20]
            with proxy.lock:
                proxy.tokens.append((vip, token))
            print(f"token for {ipstr(vip)}")
        elif ptype == P["ExternalProxy"] and own is None:
            # Our own join: connect to our proxy with the token, then echo datagrams.
            own = socket.create_connection(("127.0.0.1", args.proxy_port))
            own.sendall(frame(P["ExternalProxy"], body))

            def own_reader(sock=own):
                my_ip = None
                for t, b in read_frames(sock):
                    if t == P["ProxyConfig"]:
                        my_ip = struct.unpack_from("<I", b)[0]
                        print(f"host address {ipstr(my_ip)}")
                    elif t == P["ProxyData"]:
                        src, sport, dst, dport, proto, n = struct.unpack_from("<IHIHiI", b)
                        if src == my_ip:
                            continue
                        print(f"host got {n} bytes from {ipstr(src)}:{sport} -> {ipstr(dst)}:{dport}")
                        reply = proxy_data(my_ip, dport, src, sport, b"echo:" + b[20:], proto)
                        sock.sendall(frame(P["ProxyData"], reply))
            threading.Thread(target=own_reader, daemon=True).start()
        elif ptype == P["Connected"] and not joined:
            joined = True
            print("lobby up (P2P), waiting for joiners")
        elif ptype == P["SyncNetwork"]:
            print(f"lobby now has {body[0x67]} nodes")
        elif ptype == P["NetworkError"]:
            print("server error", struct.unpack("<i", body)[0])
        elif ptype not in (P["Ping"],):
            print("master:", name)
    master.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
