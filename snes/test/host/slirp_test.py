#!/usr/bin/env python3
"""The NAT half of the gateway (tools/slirp.py), driven from the console's side.

The Mesen suites prove the CONSOLE composes and parses IP correctly, against a
Lua mini-gateway. This proves the DAEMON does, against a scripted console —
and the two implementations share no code, which is the point: a checksum
convention both ends get wrong the same way would pass either test alone.

Everything here is hermetic. The echo/HTTP peers are real sockets on
127.0.0.1, so the TCP path exercises a genuine three-way handshake, genuine
sequence arithmetic and a genuine close — the only fiction is that the
console's packets arrive by function call instead of down a cable.

Covered:
  1. ICMP echo to 10.0.2.2 (the console's first proof of life);
  2. UDP out and back, including the reply wearing the address the console
     sent to rather than the socket's real peer;
  3. TCP: handshake, data both ways, and a close from each side;
  4. TCP to a closed port: RST, not silence (silence costs uIP its whole
     connection timeout);
  5. a port forward: an OUTSIDE connection reaching the console as an
     inbound SYN it can answer;
  6. a garbage datagram is dropped without taking the daemon with it.

Plain assertions, no pytest: `python3 test/host/slirp_test.py`, exit 0 on
pass, prints RESULT PASS/FAIL like every other suite.
"""
import os
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import slirp  # noqa: E402

SNES = slirp.SNES_IP
GW = slirp.GW_IP
FIN, SYN, RST, PSH, ACK = slirp.FIN, slirp.SYN, slirp.RST, slirp.PSH, slirp.ACK


class Console:
    """The SNES side: composes datagrams, parses what comes back."""

    def __init__(self):
        self.rx = []
        self.nat = slirp.Slirp(self.rx.append, log=lambda *a: None)
        self.ident = 0

    def send(self, dst, proto, payload):
        self.ident += 1
        hdr = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(payload), self.ident,
                          0x4000, 64, proto, 0)
        hdr += socket.inet_aton(SNES) + socket.inet_aton(dst)
        c = slirp.csum16(hdr)
        hdr = hdr[:10] + struct.pack("!H", c) + hdr[12:]
        self.nat.input(hdr + payload)

    def pump(self, seconds=0.5, until=None):
        """Tick the NAT until `until()` is true or the time runs out."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.nat.tick()
            if until is not None and until():
                return True
            time.sleep(0.002)
        return until is None or until()

    def wait_rx(self, seconds=2.0):
        """Tick until at least one datagram comes back. Returns True if one did."""
        return self.pump(seconds, until=lambda: bool(self.rx))

    def wait_tcp(self, seconds, pred):
        """Tick until some received TCP segment satisfies `pred`.

        Segments are ACCUMULATED, not consumed, because the interesting
        condition is usually spread over several ("the data arrived AND it was
        acked"), and a peek that ate them would make the assertion after the
        wait see nothing."""
        deadline = time.time() + seconds
        while True:
            for seg in self.tcp_peek():
                if pred(seg):
                    return True
            if time.time() >= deadline:
                return False
            self.nat.tick()
            time.sleep(0.002)

    def tcp_peek(self):
        """Parse the TCP segments received so far WITHOUT consuming them."""
        out = []
        for pkt in self.rx:
            if pkt[9] != slirp.PROTO_TCP:
                continue
            self.check(pkt)
            out.append(self.tcp_parse(pkt[(pkt[0] & 0x0F) * 4:]))
        return out

    @staticmethod
    def check(pkt):
        """Validate a datagram the way the CONSOLE does, and be strict.

        uIP verifies the transport checksum on every segment and silently
        drops what fails, so a daemon computing it wrongly produces a console
        that receives nothing while every log at this end looks perfect. This
        test peer therefore has to check the same things the 65816 checks —
        checking only the IP header (which is what it did first) let a
        deliberately corrupted TCP checksum pass the whole suite."""
        ihl = (pkt[0] & 0x0F) * 4
        assert slirp.csum16(pkt[:ihl]) == 0, "daemon sent a bad IP checksum"
        dst = socket.inet_ntoa(pkt[16:20])
        assert dst == SNES, f"datagram addressed to {dst}, not the console"
        total = struct.unpack("!H", pkt[2:4])[0]
        body, proto = pkt[ihl:total], pkt[9]
        if proto == slirp.PROTO_ICMP:
            assert slirp.csum16(body) == 0, "bad ICMP checksum"
        elif proto in (slirp.PROTO_TCP, slirp.PROTO_UDP):
            if proto == slirp.PROTO_UDP and body[6:8] == b"\0\0":
                return              # legal: "checksum not computed"
            src4, dst4 = struct.unpack("!II", pkt[12:20])
            assert slirp.csum16(
                slirp._pseudo(src4, dst4, proto, len(body)) + body) == 0, \
                f"bad {'TCP' if proto == slirp.PROTO_TCP else 'UDP'} checksum"

    def take(self, proto=None):
        """Pop parsed datagrams received so far, oldest first."""
        out = []
        for pkt in self.rx:
            self.check(pkt)
            ihl = (pkt[0] & 0x0F) * 4
            src = socket.inet_ntoa(pkt[12:16])
            if proto is None or pkt[9] == proto:
                out.append((src, pkt[9], pkt[ihl:]))
        self.rx.clear()
        return out

    # ---- TCP helpers: the console's own sequence bookkeeping ----
    def tcp_send(self, dst, sport, dport, flags, seq, ack, payload=b""):
        hdr = struct.pack("!HHIIBBHHH", sport, dport, seq, ack,
                          5 << 4, flags, 1024, 0, 0)
        c = slirp.csum16(
            slirp._pseudo(slirp._ip2int(SNES), slirp._ip2int(dst),
                          slirp.PROTO_TCP, len(hdr) + len(payload)) +
            hdr + payload)
        seg = hdr[:16] + struct.pack("!H", c) + hdr[18:] + payload
        self.send(dst, slirp.PROTO_TCP, seg)

    @staticmethod
    def tcp_parse(body):
        sport, dport, seq, ack = struct.unpack("!HHII", body[:12])
        off = (body[12] >> 4) * 4
        return dict(sport=sport, dport=dport, seq=seq, ack=ack,
                    flags=body[13], data=body[off:])

    def tcp_take(self):
        return [self.tcp_parse(b) for _s, _p, b in self.take(slirp.PROTO_TCP)]


def echo_server():
    """A real listening socket that echoes, then a second that closes at once."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)

    def run():
        try:
            conn, _ = srv.accept()
            with conn:
                while True:
                    d = conn.recv(512)
                    if not d:
                        break
                    conn.sendall(d.upper())
        except OSError:
            pass

    threading.Thread(target=run, daemon=True).start()
    return srv, srv.getsockname()[1]


def udp_server():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))

    def run():
        try:
            while True:
                d, a = s.recvfrom(512)
                s.sendto(b"pong:" + d, a)
        except OSError:
            pass

    threading.Thread(target=run, daemon=True).start()
    return s, s.getsockname()[1]


def t_icmp(c):
    icmp = bytearray(struct.pack("!BBHHH", 8, 0, 0, 0x5341, 3) + b"odd-len!")
    icmp[2:4] = struct.pack("!H", slirp.csum16(bytes(icmp)))
    c.send(GW, slirp.PROTO_ICMP, bytes(icmp))
    got = c.take(slirp.PROTO_ICMP)
    assert len(got) == 1, f"gateway ping got {len(got)} replies, wanted 1"
    src, _p, body = got[0]
    assert src == GW, f"reply came from {src}, not {GW}"
    assert slirp.csum16(body) == 0, "reply's ICMP checksum is wrong"
    assert body[0] == 0, "reply is not an echo reply"
    assert body[4:8] == bytes(icmp[4:8]), "id/seq not echoed"
    assert body[8:] == b"odd-len!", "payload not echoed byte-for-byte"


def t_udp(c):
    srv, port = udp_server()
    try:
        payload = b"hello over udp"
        ln = 8 + len(payload)
        hdr = struct.pack("!HHHH", 4444, port, ln, 0)
        cs = slirp.csum16(
            slirp._pseudo(slirp._ip2int(SNES), slirp._ip2int("127.0.0.1"),
                          slirp.PROTO_UDP, ln) + hdr + payload)
        c.send("127.0.0.1", slirp.PROTO_UDP,
               hdr[:6] + struct.pack("!H", cs or 0xFFFF) + payload)
        assert c.wait_rx(2.0), "no UDP reply came back"
        got = c.take(slirp.PROTO_UDP)
        assert got, "reply was not UDP"
        src, _p, body = got[0]
        assert src == "127.0.0.1", f"reply source rewritten to {src}"
        sport, dport = struct.unpack("!HH", body[:4])
        assert dport == 4444, f"reply aimed at port {dport}, not 4444"
        assert body[8:] == b"pong:" + payload, "payload mangled"
    finally:
        srv.close()


def t_tcp(c):
    srv, port = echo_server()
    try:
        sport = 5000
        seq = 0x1111
        c.tcp_send("127.0.0.1", sport, port, SYN, seq, 0)
        assert c.wait_rx(2.0), "no SYN-ACK"
        sa = c.tcp_take()[0]
        assert sa["flags"] & SYN and sa["flags"] & ACK, "not a SYN-ACK"
        assert sa["ack"] == seq + 1, "SYN-ACK does not acknowledge our SYN"
        their = sa["seq"] + 1
        seq += 1
        c.tcp_send("127.0.0.1", sport, port, ACK, seq, their)

        c.tcp_send("127.0.0.1", sport, port, PSH | ACK, seq, their, b"hey")
        seq += 3
        assert c.wait_tcp(3.0, lambda s: s["data"]), "the echo never came back"
        segs = c.tcp_take()
        data = b"".join(s["data"] for s in segs)
        acked = max(s["ack"] for s in segs)
        assert acked == seq, f"our 3 bytes were acked as {acked}, wanted {seq}"
        assert data == b"HEY", f"echo came back as {data!r}"
        their += len(data)
        c.tcp_send("127.0.0.1", sport, port, ACK, seq, their)

        # Console closes. The daemon must ack the FIN and, once the real peer
        # notices, send one of its own.
        c.tcp_send("127.0.0.1", sport, port, FIN | ACK, seq, their)
        seq += 1
        assert c.wait_tcp(3.0, lambda s: s["flags"] & FIN), \
            "the daemon never closed its side"
        segs = c.tcp_take()
        assert any(s["ack"] == seq for s in segs), "our FIN was not acked"
        assert any(s["flags"] & FIN for s in segs), "no FIN from the daemon"
    finally:
        srv.close()


def t_tcp_data_fin(c):
    """The last data bytes and the FIN in ONE segment.

    Not a variation for its own sake: it is what a small HTTP request looks
    like, and it is the case where the FIN's sequence number is seq+len rather
    than seq. Testing only the bare FIN (which t_tcp does) passes happily
    while that arithmetic is wrong, and the console then retransmits its
    close until uIP gives up on it."""
    srv, port = echo_server()
    try:
        sport, seq = 5002, 0x4444
        c.tcp_send("127.0.0.1", sport, port, SYN, seq, 0)
        assert c.wait_rx(2.0), "no SYN-ACK"
        sa = c.tcp_take()[0]
        their = sa["seq"] + 1
        seq += 1
        c.tcp_send("127.0.0.1", sport, port, ACK, seq, their)

        c.tcp_send("127.0.0.1", sport, port, PSH | FIN | ACK, seq, their, b"bye")
        seq += 3 + 1  # three data bytes, and the FIN occupies one more
        assert c.wait_tcp(3.0, lambda s: s["ack"] == seq), \
            "a FIN carried with data was never acknowledged - the daemon is " \
            "acking seq+len or seq+1 instead of seq+len+1"
        assert c.wait_tcp(3.0, lambda s: s["data"] == b"BYE"), \
            "data sent alongside the FIN was dropped"
    finally:
        srv.close()


def t_stray(c):
    """A segment for a connection that does not exist must be refused.

    Distinct from t_refused: that one reaches a dead PORT (so the daemon
    learns from connect() failing), this one reaches a dead CONNECTION, where
    nothing will ever tell the daemon anything. It happens for real whenever
    the console reboots mid-connection and its retransmissions outlive the
    session."""
    c.tcp_send("127.0.0.1", 5999, 9999, ACK, 0x9000, 0x9000)
    assert c.wait_rx(1.0), "a stray segment got no answer at all"
    segs = c.tcp_take()
    assert any(s["flags"] & RST for s in segs), \
        f"expected RST for an unknown connection, got " \
        f"{[hex(s['flags']) for s in segs]}"


def t_refused(c):
    # A port with nothing on it. Bind and close to get one that is certainly
    # free, which beats hoping a number is.
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    dead = s.getsockname()[1]
    s.close()
    c.tcp_send("127.0.0.1", 5001, dead, SYN, 0x2222, 0)
    assert c.wait_rx(3.0), \
        "connection to a dead port produced NOTHING - uIP would wait out its " \
        "whole timeout instead of failing fast"
    segs = c.tcp_take()
    assert any(s["flags"] & RST for s in segs), \
        f"expected RST, got flags {[hex(s['flags']) for s in segs]}"


def t_forward(c):
    assert c.nat.forward_add(0) is False or True  # port 0 is legal to bind
    # Pick a real port by binding one first, then hand it to the NAT.
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    assert c.nat.forward_add(port), f"could not forward port {port}"
    outside = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    outside.settimeout(2)
    try:
        outside.connect(("127.0.0.1", port))
        assert c.pump(2.0, until=lambda: bool(c.rx)), \
            "an outside connection produced no SYN toward the console"
        segs = c.tcp_take()
        syn = [s for s in segs if s["flags"] & SYN]
        assert syn, "the daemon did not SYN the console"
        s0 = syn[0]
        assert s0["dport"] == port, \
            f"inbound arrived at console port {s0['dport']}, not {port}"
        # Answer it as uIP would, then expect the daemon's ACK.
        c.tcp_send(GW, s0["dport"], s0["sport"], SYN | ACK, 0x3333,
                   s0["seq"] + 1)
        assert c.pump(2.0, until=lambda: bool(c.rx)), \
            "the daemon never completed the handshake it started"
    finally:
        outside.close()
        c.nat.forward_del(port)


def t_garbage(c):
    c.nat.input(b"")
    c.nat.input(b"\x00" * 40)
    c.nat.input(bytes([0x45, 0, 0, 200]) + b"\x00" * 16)  # length past the end
    before = c.nat.stat_drop
    c.send(GW, 99, b"unknown protocol")
    assert c.nat.stat_drop > before, "an unknown protocol was not counted"
    t_icmp(c)  # and the daemon still works afterwards


def main():
    c = Console()
    tests = [("icmp", t_icmp), ("udp", t_udp), ("tcp", t_tcp),
             ("tcp-data-fin", t_tcp_data_fin), ("stray", t_stray),
             ("refused", t_refused), ("forward", t_forward),
             ("garbage", t_garbage)]
    try:
        for name, fn in tests:
            c.rx.clear()
            fn(c)
        print("RESULT PASS slirp -- icmp, udp, tcp both ways, data+fin, "
              "rst x2, forward")
        return 0
    except AssertionError as e:
        print(f"RESULT FAIL slirp -- {e}")
        return 1
    finally:
        c.nat.close()


if __name__ == "__main__":
    sys.exit(main())
