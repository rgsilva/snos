#!/usr/bin/env python3
"""mcprobe: serve mkmc's baked bytes to a real Minecraft client (M13 step 1).

This is the desk-speed half of the milestone's validation: the ~100 lines a
real 1.16.5 Java client needs from a server, serving EXACTLY the blobs
mkmc.bake() produced — the same bytes step 2 will stream from ROM banks. If
the client spawns and walks around against this, the remaining risk on the
console is transport timing, not protocol bytes.

It deliberately mirrors what the `mc` app will be, and nothing more:
one connection at a time (uIP's listen refusal, rehearsed), a VarInt-length
framer that SKIPS every serverbound packet it does not care about, and a
handful of the ones it does:

    keepalive response   ignored (receiving it at all is the liveness)
    chat                 printed, echoed back into the client's chat
    player digging       answered with a Block Change restoring the block —
                         the SNES puts the world back (rule, not table)

Run:  python3 tools/mcprobe.py            (port 25565)
then Minecraft 1.16.5 -> Multiplayer -> Direct Connect -> localhost.
Ctrl-C to stop. Every packet id the client sends is logged with --verbose,
which is the instrument for discovering what a vanilla client insists on.
"""
import argparse
import json
import socket
import struct
import time

import mkmc

KEEPALIVE_EVERY = 10.0     # seconds; the client kicks a silent server at ~30


class Conn:
    """Blocking socket with a VarInt framer and a short poll timeout, so the
    read loop can interleave keepalives the way the console's frame timer
    will."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""
        sock.settimeout(0.05)

    def _fill(self):
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return True
        if not data:
            return False
        self.buf += data
        return True

    def read_packet(self):
        """One framed packet as (id, payload), None on no-data-yet, or
        raises ConnectionError on EOF."""
        while True:
            pid_len = self._varint_at(0)
            if pid_len is None:
                if not self._fill():
                    raise ConnectionError("client closed")
                if self._varint_at(0) is None:
                    return None
                continue
            length, consumed = pid_len
            if len(self.buf) < consumed + length:
                if not self._fill():
                    raise ConnectionError("client closed")
                if len(self.buf) < consumed + length:
                    return None
                continue
            body = self.buf[consumed:consumed + length]
            self.buf = self.buf[consumed + length:]
            pid, off = self._varint(body, 0)
            return pid, body[off:]

    def _varint_at(self, off):
        try:
            return self._varint(self.buf, off)[0], \
                self._varint(self.buf, off)[1]
        except IndexError:
            return None

    @staticmethod
    def _varint(data, off):
        v = 0
        for i in range(5):
            b = data[off + i]
            v |= (b & 0x7F) << (7 * i)
            if not (b & 0x80):
                return v, off + i + 1
        raise ValueError("varint too long")

    def send(self, data):
        self.sock.sendall(data)


def read_string(payload, off):
    n, off = Conn._varint(payload, off)
    return payload[off:off + n].decode("utf-8", "replace"), off + n


def unpack_position(v):
    x = v >> 38
    z = (v >> 12) & 0x3FFFFFF
    y = v & 0xFFF
    if x >= 1 << 25:
        x -= 1 << 26
    if z >= 1 << 25:
        z -= 1 << 26
    if y >= 1 << 11:
        y -= 1 << 12
    return x, y, z


def chat_packet(text, uuid):
    body = mkmc.mcstring('{"text":%s}' % json.dumps(text))
    return mkmc.packet(mkmc.P_CHAT, body + b"\x00" + uuid)


def serve_connection(sock, addr, blobs, manifest, verbose):
    conn = Conn(sock)

    # ---- handshake
    pkt = None
    while pkt is None:
        pkt = conn.read_packet()
    pid, payload = pkt
    if pid != 0x00:
        print(f"  unexpected first packet 0x{pid:02x}")
        return
    proto, off = Conn._varint(payload, 0)
    _, off = read_string(payload, off)
    off += 2  # port
    state, _ = Conn._varint(payload, off)

    if state == 1:
        # ---- status: respond, echo the ping, done.
        print(f"  status ping (client protocol {proto})")
        while True:
            pkt = conn.read_packet()
            if pkt is None:
                continue
            pid, payload = pkt
            if pid == 0x00:
                conn.send(blobs["status_response"])
            elif pid == 0x01:
                pong = bytearray(blobs["pong"])
                o = manifest["pong_payload_offset"]
                pong[o:o + 8] = payload[:8]
                conn.send(bytes(pong))
                return
        return

    if state != 2:
        return

    # ---- login
    if proto != mkmc.PROTOCOL:
        print(f"  client protocol {proto}, we speak {mkmc.PROTOCOL} "
              f"(1.16.5) - it will refuse")
    pkt = None
    while pkt is None:
        pkt = conn.read_packet()
    pid, payload = pkt
    name, _ = read_string(payload, 0)
    uuid = mkmc.offline_uuid(name)
    print(f"  login: {name}")

    # Login Success is the one runtime-framed packet: its length depends on
    # the name. Everything after it is baked bytes, verbatim.
    body = blobs["login_success_id"] + uuid + mkmc.mcstring(name)
    conn.send(mkmc.varint(len(body)) + body)

    # ---- play: the baked burst
    t0 = time.time()
    conn.send(blobs["play_pre"])
    for cname in manifest["chunks"]:
        conn.send(blobs[cname])
    conn.send(blobs["play_post"])
    sent = len(blobs["play_pre"]) + len(blobs["play_post"]) + \
        sum(len(blobs[c]) for c in manifest["chunks"])
    print(f"  join burst: {sent} bytes in {time.time() - t0:.2f}s")
    conn.send(chat_packet(
        "Welcome to a Super Nintendo. The world is read-only.", uuid))

    # ---- steady state
    last_ka = time.time()
    ka_id = 0
    while True:
        now = time.time()
        if now - last_ka >= KEEPALIVE_EVERY:
            last_ka = now
            ka_id += 1
            ka = bytearray(blobs["keepalive"])
            o = manifest["keepalive_id_offset"]
            ka[o:o + 8] = struct.pack(">q", ka_id)
            conn.send(bytes(ka))
        pkt = conn.read_packet()
        if pkt is None:
            continue
        pid, payload = pkt
        if verbose:
            print(f"    <- 0x{pid:02x} ({len(payload)}B)")
        if pid == mkmc.S_CHAT:
            text, _ = read_string(payload, 0)
            print(f"  <{name}> {text}")
            conn.send(chat_packet(f"<{name}> {text}", uuid))
        elif pid == mkmc.S_PLAYER_DIGGING:
            status, off = Conn._varint(payload, 0)
            pos = struct.unpack(">Q", payload[off:off + 8])[0]
            x, y, z = unpack_position(pos)
            if status in (0, 2):
                original = mkmc.block_at(x, y, z)
                conn.send(mkmc.packet(
                    mkmc.P_BLOCK_CHANGE,
                    mkmc.position(x, y, z) + mkmc.varint(original)))
                print(f"  dig at {x},{y},{z} - put back state {original}")
        # Everything else - teleport confirms, settings, position spam,
        # keepalive responses - is drained by the framer and ignored,
        # exactly as the console will.


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=25565)
    ap.add_argument("--verbose", action="store_true",
                    help="log every serverbound packet id")
    args = ap.parse_args()

    blobs, manifest = mkmc.bake()
    total = sum(len(v) for v in blobs.values())
    print(f"mcprobe: {len(manifest['chunks'])} chunks baked, "
          f"{total} bytes of blobs")
    print(f"mcprobe: listening on port {args.port} - Minecraft "
          f"{mkmc.VERSION_NAME}, Direct Connect to localhost")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)   # one at a time: uIP's listen, rehearsed
    try:
        while True:
            sock, addr = srv.accept()
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"connection from {addr[0]}:{addr[1]}")
            try:
                serve_connection(sock, addr, blobs, manifest, args.verbose)
            except (ConnectionError, OSError) as e:
                print(f"  gone: {e}")
            finally:
                try:
                    sock.close()
                except OSError:
                    pass
    except KeyboardInterrupt:
        print("\nmcprobe: stopped")


if __name__ == "__main__":
    main()
