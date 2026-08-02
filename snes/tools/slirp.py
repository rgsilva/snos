"""User-mode NAT for SNOS (M11): the half of the gateway that is a NETWORK.

The stream gateway next door answers questions ("fetch this URL", "connect me
to this host") and hands the console bytes. This module answers nothing. It
takes IPv4 datagrams the SNES composed, and it puts IPv4 datagrams back — the
console runs the stack, and what happens here is address translation and
socket work, exactly as QEMU's slirp does for a guest.

Why user-mode NAT rather than a tun device: a tun/utun interface needs root
and changes the machine's routing table, and this is somebody's laptop. The
tradeoff is that only the FAR side of the NAT is synthetic — every header on
the console's side is real, composed and parsed by the 65816, which is the
whole point of the milestone. A real-interface mode can come later as a
flourish; nothing above this changes when it does.

------------------------------------------------------------------- topology

    10.0.2.15   the SNES              (constants, no DHCP — see ip.h)
    10.0.2.2    us: default route and NAT
    10.0.2.3    us: the resolver the console sends real DNS queries to

Two nodes on a point-to-point link, so there is no ARP, no ethernet header
and nothing to discover. The daemon's own sockets carry traffic onward, so
externally everything wears the host's address.

--------------------------------------------------------------------- TCP

We terminate the console's TCP and open a real socket per connection. The
console runs uIP, which handles ONE outstanding segment at a time, so the
sending logic here is deliberately stop-and-wait: send a segment, wait for
the ack, send the next. That is not a simplification to be undone later —
sending two segments to uIP means the second is dropped and retransmitted,
which is slower than not sending it.
"""
import errno
import os
import socket
import struct
import time

SNES_IP = "10.0.2.15"
GW_IP = "10.0.2.2"
DNS_IP = "10.0.2.3"

MTU = 1500             # matches the console's UIP_CONF_BUFFER_SIZE/IP_MTU
MSS = 1460             # MTU - 20 IP - 20 TCP
TCP_WINDOW = 2048      # what we advertise; we still send one segment at a time

PROTO_ICMP, PROTO_TCP, PROTO_UDP = 1, 6, 17

# Flags
FIN, SYN, RST, PSH, ACK = 0x01, 0x02, 0x04, 0x08, 0x10

# A full segment is 578 bytes on a link that moves ~615 B/s, so it takes most
# of a second to arrive before the ack can even start back. A 0.4 s timer
# therefore retransmitted every large segment two or three times BEFORE the
# first copy had finished transmitting, and each copy was appended to the same
# queue -- the backlog grew with every round until the connection was reset.
# The hermetic test never saw it because its "link" is a Python list.
RTO = 2.5
MAX_RETRIES = 8        # handshake/FIN: then give the connection up
# Data toward the console is different: uIP's flow control is DROP-WITHOUT-
# ACK (a stopped window doesn't ack, it ignores), and an app that streams a
# long burst without draining - mc's join, legitimately - keeps the window
# shut for minutes. Eight retries read that as death and RESET a healthy
# client mid-join (found live, silently: the reset wasn't even logged).
# Patience is bounded by TCP_IDLE anyway, so give data the long rope.
DATA_RETRIES = 120     # x RTO = five minutes of window-closed tolerance
UDP_IDLE = 60.0        # forget a UDP flow after this much silence
ICMP_IDLE = 15.0
# TCP had none, so any flow where BOTH sides went quiet lived forever: nothing
# drives the retransmit path, so MAX_RETRIES never fires. A console whose app
# died without a link reset, or a half-closed connection the far end simply
# kept open, leaked a socket and a table entry permanently.
TCP_IDLE = 300.0
TIME_WAIT = 4.0        # keep a closed flow's 4-tuple reserved this long
# A forwarded port is reachable by anything that can route here, which makes
# the accept loop the daemon's one unbounded input. Every accepted connection
# costs a descriptor, a table entry and a SYN re-queued every RTO for ~20 s
# before it is reaped -- so a client opening connections in a loop exhausts
# file descriptors (after which even the NAT's own sockets fail) and floods a
# 615 B/s link queue, all from outside. The console serves one connection per
# listener anyway, so a small cap costs nothing real and the OS's own backlog
# refuses the rest.
MAX_INBOUND_PER_PORT = 4
# How long an inbound client's half-close is held back from the console, so
# the server's reply can leave first — see the comment at the FIN in
# _pump_tcp. Long enough for an app polling at frame rate to answer, short
# enough that a netecho-style serve-and-hangup loop still cycles briskly.
FIN_GRACE = 0.75


def _ip2int(s):
    return struct.unpack("!I", socket.inet_aton(s))[0]


def _int2ip(v):
    return socket.inet_ntoa(struct.pack("!I", v))


SNES_INT = _ip2int(SNES_IP)
GW_INT = _ip2int(GW_IP)
DNS_INT = _ip2int(DNS_IP)


def csum16(data, init=0):
    """RFC 1071 ones-complement sum, complemented — a header checksum."""
    if len(data) % 2:
        data += b"\0"
    s = init + sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def _pseudo(src, dst, proto, ln):
    return struct.pack("!IIBBH", src, dst, 0, proto, ln)


class _Flow:
    """Common bookkeeping: every NAT entry has a socket and an idle clock."""

    def __init__(self, sock):
        self.sock = sock
        self.touched = time.time()
        self.queries = 0       # datagrams sent on this flow (resolver rotation)

    def touch(self):
        self.touched = time.time()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class _Tcp(_Flow):
    # States. LISTEN is not one of ours: a console-initiated connection starts
    # at CONNECTING (we are dialling the real host) and a forwarded one starts
    # at SYN_SENT (we are dialling the console).
    CONNECTING, SYN_SENT, SYN_RCVD, EST, FIN_SENT, DONE = range(6)

    def __init__(self, sock, key, iss):
        _Flow.__init__(self, sock)
        self.key = key
        self.state = _Tcp.CONNECTING
        self.iss = iss
        self.snd_nxt = iss
        self.snd_una = iss
        self.rcv_nxt = 0
        self.outbuf = b""      # read from the socket, not yet sent
        self.inbuf = b""       # taken from the console, not yet written out
        self.unacked = 0       # bytes of outbuf currently in flight
        self.sent_at = 0.0
        self.retries = 0
        self.last_seg = None   # for retransmission
        self.peer_fin = False  # console sent FIN
        self.sock_eof = False  # real socket reached EOF
        self.fin_sent = False
        self.closed_at = 0.0
        self.inbound = False   # a port forward: the console is the server
        self.eof_at = 0.0      # when sock_eof was first seen
        self.last_from_console = 0.0  # console payload most recently arrived


class Slirp:
    """One NAT instance. `output(datagram_bytes)` queues toward the console."""

    def __init__(self, output, log=print, link_busy=None):
        self.output = output
        self.log = log
        # Reports whether datagrams we already emitted are still waiting in
        # the link's out queue. The retransmit timers must not run against
        # a queue: a 578 B segment takes ~1 s to even LEAVE on a ~615 B/s
        # link, so a wall-clock RTO armed at queue time re-queued copies of
        # segments whose first copy had not finished transmitting - the
        # documented death spiral, regressed whenever several flows talk.
        self.link_busy = link_busy or (lambda: False)
        self.tcp = {}          # (sport, dip, dport) -> _Tcp
        self.udp = {}          # (sport, dip, dport) -> _Flow
        self.icmp = {}         # console ICMP id      -> _Flow
        self.forwards = {}     # real port            -> listening socket
        self.fwd_ch = {}       # real port            -> set of owner channels
        self.ident = 1
        self.iss = 0x2000
        # Monotonic, because uIP holds a closed 4-tuple in TIME_WAIT for 60
        # seconds and answers a SYN that matches one with a bare ACK, never a
        # SYN-ACK. Deriving the port from the table SIZE handed out 40000
        # again the moment the table emptied, so the second request to a
        # console server within a minute hung and then failed.
        self.next_eport = 40000
        self.stat_in = 0
        self.stat_out = 0
        self.stat_drop = 0
        self._resolvers = None

    # ------------------------------------------------------------ plumbing
    def close(self):
        for t in list(self.tcp.values()):
            t.close()
        for u in list(self.udp.values()):
            u.close()
        for i in list(self.icmp.values()):
            i.close()
        for s in list(self.forwards.values()):
            try:
                s.close()
            except OSError:
                pass
        self.tcp.clear()
        self.udp.clear()
        self.icmp.clear()
        self.forwards.clear()
        self.fwd_ch.clear()

    def _emit(self, src, dst, proto, payload):
        self.ident = (self.ident + 1) & 0xFFFF
        hdr = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(payload), self.ident,
                          0x4000, 64, proto, 0) + struct.pack("!II", src, dst)
        c = csum16(hdr)
        hdr = hdr[:10] + struct.pack("!H", c) + hdr[12:]
        self.stat_out += 1
        self.output(hdr + payload)

    def resolvers(self):
        """The host's own nameservers, for datagrams aimed at 10.0.2.3.

        Read once, from resolv.conf, because that is what every other client
        on this machine uses and inheriting it means the console resolves
        exactly what the laptop resolves — including a corporate split-horizon
        setup. The public fallback exists so a machine with no resolv.conf
        (rare, but a VPN can rewrite it mid-flight) still answers."""
        if self._resolvers is None:
            out = []
            try:
                with open("/etc/resolv.conf") as f:
                    for line in f:
                        parts = line.split()
                        if len(parts) < 2 or parts[0] != "nameserver":
                            continue
                        # IPv4 ONLY. This Mac lists two IPv6 link-local
                        # resolvers (fe80::...%en0) ahead of its IPv4 ones,
                        # and taking the first entry blindly aimed the
                        # console's queries at an address its IPv4-only stack
                        # can neither reach nor be answered from — it crashed
                        # the daemon outright, which took the whole link with
                        # it. Nothing here can use an IPv6 resolver, so skip
                        # them rather than fail on them.
                        try:
                            socket.inet_aton(parts[1])
                        except OSError:
                            continue
                        out.append(parts[1])
            except OSError:
                pass
            self._resolvers = out or ["1.1.1.1", "8.8.8.8"]
        return self._resolvers

    # --------------------------------------------------------------- input
    def input(self, pkt):
        """One IPv4 datagram from the console."""
        if len(pkt) < 20 or (pkt[0] >> 4) != 4:
            self.stat_drop += 1
            return
        ihl = (pkt[0] & 0x0F) * 4
        total = struct.unpack("!H", pkt[2:4])[0]
        if ihl < 20 or total < ihl or total > len(pkt):
            self.stat_drop += 1
            return
        if csum16(pkt[:ihl]) != 0:
            self.stat_drop += 1
            return
        proto = pkt[9]
        src, dst = struct.unpack("!II", pkt[12:20])
        if src != SNES_INT:
            # Only one machine lives out there. Anything else is a bug on the
            # console worth seeing rather than a packet worth routing.
            self.stat_drop += 1
            return
        self.stat_in += 1
        body = pkt[ihl:total]
        if proto == PROTO_ICMP:
            self._in_icmp(dst, body)
        elif proto == PROTO_UDP:
            self._in_udp(dst, body)
        elif proto == PROTO_TCP:
            self._in_tcp(dst, body)
        else:
            self.stat_drop += 1

    # ---------------------------------------------------------------- ICMP
    def _in_icmp(self, dst, body):
        if len(body) < 8 or csum16(body) != 0:
            self.stat_drop += 1
            return
        if body[0] != 8:  # only echo request means anything to a NAT
            self.stat_drop += 1
            return
        if dst == GW_INT or dst == DNS_INT:
            # We are the destination: answer ourselves. `ping 10.0.2.2` is the
            # first thing anyone tries, and it must work with no sockets at
            # all — it is the console's proof that its own stack composes and
            # parses IPv4, which is what M11 is.
            rep = bytearray(body)
            rep[0] = 0
            rep[2:4] = b"\0\0"
            rep[2:4] = struct.pack("!H", csum16(bytes(rep)))
            self._emit(dst, SNES_INT, PROTO_ICMP, bytes(rep))
            return

        ident = struct.unpack("!H", body[4:6])[0]
        flow = self.icmp.get(ident)
        if flow is None:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,
                                  socket.IPPROTO_ICMP)
            except OSError as e:
                # Linux hosts refuse this unless net.ipv4.ping_group_range says
                # otherwise, and raw sockets need root. Pinging the gateway
                # still works; pinging the world does not. Say so once rather
                # than dropping in silence.
                self.stat_drop += 1
                self.log(f"slirp: no unprivileged ICMP socket ({e}); "
                         f"ping to {_int2ip(dst)} dropped")
                return
            s.setblocking(False)
            flow = _Flow(s)
            self.icmp[ident] = flow
        flow.touch()
        try:
            flow.sock.sendto(bytes(body), (_int2ip(dst), 0))
        except OSError:
            self.stat_drop += 1

    def _pump_icmp(self):
        for ident, flow in list(self.icmp.items()):
            while True:
                try:
                    data, addr = flow.sock.recvfrom(MTU)
                except (BlockingIOError, InterruptedError):
                    break
                except OSError:
                    break
                if not data:
                    break
                # macOS hands back the whole IP datagram on a SOCK_DGRAM ICMP
                # socket; Linux hands back the ICMP message alone. Accept both
                # rather than guessing from the platform.
                if len(data) >= 20 and (data[0] >> 4) == 4:
                    ihl = (data[0] & 0x0F) * 4
                    peer = struct.unpack("!I", data[12:16])[0]
                    icmp = bytearray(data[ihl:])
                else:
                    peer = _ip2int(addr[0])
                    icmp = bytearray(data)
                if len(icmp) < 8:
                    continue
                # The console drops anything over its MTU by discarding the
                # WHOLE reassembly buffer, so one oversized reply costs
                # unrelated datagrams that were in flight. Every other emit
                # path here is bounded; this socket is unconnected and anyone
                # can send to it, so bound it explicitly.
                if len(icmp) + 20 > MTU:
                    self.stat_drop += 1
                    continue
                # Restore the console's identifier: Linux rewrites it to the
                # socket's port, macOS does not, and the console matches its
                # replies on that field.
                icmp[4:6] = struct.pack("!H", ident)
                icmp[2:4] = b"\0\0"
                icmp[2:4] = struct.pack("!H", csum16(bytes(icmp)))
                flow.touch()
                self._emit(peer, SNES_INT, PROTO_ICMP, bytes(icmp))

    # ----------------------------------------------------------------- UDP
    def _in_udp(self, dst, body):
        if len(body) < 8:
            self.stat_drop += 1
            return
        sport, dport, ln = struct.unpack("!HHH", body[:6])
        if ln < 8 or ln > len(body):
            self.stat_drop += 1
            return
        payload = body[8:ln]
        # A checksum of zero means "not computed" and is legal in IPv4 UDP.
        if struct.unpack("!H", body[6:8])[0] != 0:
            if csum16(_pseudo(SNES_INT, dst, PROTO_UDP, ln) + body[:ln]) != 0:
                self.stat_drop += 1
                return

        key = (sport, dst, dport)
        flow = self.udp.get(key)
        if flow is None:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setblocking(False)
            flow = _Flow(s)
            flow.reply_from = dst      # what the console must see as the source
            flow.sport = sport
            flow.dport = dport
            self.udp[key] = flow
        flow.touch()

        target = dst
        if dst == DNS_INT:
            # The console does its OWN DNS: this is a real query it composed,
            # and we are only the recursive resolver's address. Aim it at
            # whatever this machine resolves with -- ROTATING on each retry,
            # because the console retries a lookup four times on this same
            # flow and a first nameserver that has gone away (a VPN teardown
            # leaves stale entries in resolv.conf) would otherwise cost the
            # whole lookup rather than one try. It is also what makes the
            # public fallback pair below more than decoration.
            rs = self.resolvers()
            target = _ip2int(rs[flow.queries % len(rs)])
        flow.queries += 1
        try:
            flow.sock.sendto(payload, (_int2ip(target), dport))
        except OSError:
            self.stat_drop += 1

    def _udp_out(self, flow, payload):
        ln = 8 + len(payload)
        hdr = struct.pack("!HHHH", flow.dport, flow.sport, ln, 0)
        c = csum16(_pseudo(flow.reply_from, SNES_INT, PROTO_UDP, ln) + hdr + payload)
        if c == 0:
            c = 0xFFFF  # zero means "no checksum"; the real value is ~0
        seg = hdr[:6] + struct.pack("!H", c) + payload
        self._emit(flow.reply_from, SNES_INT, PROTO_UDP, seg)

    def _pump_udp(self):
        for key, flow in list(self.udp.items()):
            while True:
                try:
                    data, _addr = flow.sock.recvfrom(65535)
                except (BlockingIOError, InterruptedError):
                    break
                except OSError:
                    break
                if not data:
                    break
                if len(data) > MTU - 28:
                    # Too large for one datagram on this link. Dropping is
                    # what the ICMP pump already does; this path used to
                    # truncate and then compute a VALID checksum over the
                    # mangled payload, handing the console a well-formed
                    # datagram containing a cut-off DNS answer with no TC
                    # bit -- a lie it has no way to detect.
                    self.stat_drop += 1
                    continue
                flow.touch()
                self._udp_out(flow, data)

    # ----------------------------------------------------------------- TCP
    def _tcp_out(self, t, flags, payload=b"", seq=None):
        seq = t.snd_nxt if seq is None else seq
        sport, dip, dport = t.key
        # A SYN carries an MSS option. Omitting it is legal and was a mistake:
        # uIP only learns an MSS from this option on a passive open, so a
        # bare SYN left the console with whatever its connection slot held --
        # zero on a fresh one -- and it could never send a byte back.
        opts = b""
        if flags & SYN:
            opts = struct.pack("!BBH", 2, 4, MSS)
        off = (20 + len(opts)) // 4
        hdr = struct.pack("!HHIIBBHHH", dport, sport, seq, t.rcv_nxt,
                          off << 4, flags, TCP_WINDOW, 0, 0) + opts
        c = csum16(_pseudo(dip, SNES_INT, PROTO_TCP, len(hdr) + len(payload)) +
                   hdr + payload)
        seg = hdr[:16] + struct.pack("!H", c) + hdr[18:] + payload
        self._emit(dip, SNES_INT, PROTO_TCP, seg)

    def _rst(self, dip, sport, dport, ack_seq, ack, had_ack):
        """Refuse a connection we have no state for.

        Without this the console retransmits its SYN for the whole of uIP's
        connection timeout — twenty-odd seconds of an app looking hung — when
        the answer ("nothing is listening") was available immediately.

        `ack_seq` is the sequence the sender should resume at, computed by
        the CALLER: this used to be an unconditional seq + 1, which is right
        only when refusing a SYN and acknowledged a byte that was never sent
        on every other path. uIP 1.0 aborts on any RST matching the tuple
        without checking, so nothing here ever misbehaved -- but a stricter
        stack would ignore the RST and hang instead, and M7 changes what is
        on the far end."""
        hdr = struct.pack("!HHIIBBHHH", dport, sport,
                          ack if had_ack else 0, ack_seq & 0xFFFFFFFF,
                          5 << 4, RST | ACK, 0, 0, 0)
        c = csum16(_pseudo(dip, SNES_INT, PROTO_TCP, len(hdr)) + hdr)
        seg = hdr[:16] + struct.pack("!H", c) + hdr[18:]
        self._emit(dip, SNES_INT, PROTO_TCP, seg)

    def _in_tcp(self, dst, body):
        if len(body) < 20:
            self.stat_drop += 1
            return
        sport, dport, seq, ack = struct.unpack("!HHII", body[:12])
        off = (body[12] >> 4) * 4
        flags = body[13]
        if off < 20 or off > len(body):
            self.stat_drop += 1
            return
        if csum16(_pseudo(SNES_INT, dst, PROTO_TCP, len(body)) + body) != 0:
            self.log(f"DBG tcp BAD CHECKSUM off={off} len={len(body)} flags={flags:02x}")
            self.stat_drop += 1
            return
        payload = body[off:]
        key = (sport, dst, dport)
        t = self.tcp.get(key)

        # A bare SYN (no ACK) on a tuple we still hold is a new connection
        # reusing the port, not part of the old conversation: uIP has
        # forgotten it even though our TIME_WAIT -- or a flow left stale by a
        # peer that vanished -- has not. Falling through to the established
        # path answered it with SILENCE, so the console retransmitted its SYN
        # until the entry aged out: a hung connect with nothing logged.
        # CONNECTING and SYN_RCVD are excluded on purpose, because a SYN
        # arriving in those states IS an expected retransmission.
        if (t is not None and (flags & SYN) and not (flags & ACK)
                and t.state in (_Tcp.DONE, _Tcp.EST, _Tcp.FIN_SENT)):
            t.close()
            self.tcp.pop(key, None)
            t = None

        if t is None:
            if flags & SYN:
                self._tcp_open(key, seq)
            elif not (flags & RST):
                nxt = (seq + len(payload) +
                       (1 if flags & FIN else 0)) & 0xFFFFFFFF
                self._rst(dst, sport, dport, nxt, ack, bool(flags & ACK))
            return

        t.touch()
        if flags & RST:
            t.close()
            self.tcp.pop(key, None)
            return

        if t.state == _Tcp.CONNECTING:
            # Still dialling the real host. A retransmitted SYN is normal and
            # needs no answer: the SYN-ACK goes out when connect() completes.
            return

        if t.state == _Tcp.SYN_RCVD:
            if flags & ACK and ack == (t.iss + 1) & 0xFFFFFFFF:
                t.state = _Tcp.EST
                t.snd_una = ack
            else:
                return

        if t.state in (_Tcp.SYN_SENT,):
            # Our SYN toward the console (a port forward). The console's
            # SYN-ACK carries its ISS.
            if (flags & SYN) and (flags & ACK) and ack == (t.iss + 1) & 0xFFFFFFFF:
                t.rcv_nxt = (seq + 1) & 0xFFFFFFFF
                t.snd_una = ack
                t.state = _Tcp.EST
                self._tcp_out(t, ACK)
            return

        # ---- established (or half-closed) ----
        if flags & ACK:
            acked = (ack - t.snd_una) & 0xFFFFFFFF
            if 0 < acked <= t.unacked + (1 if t.fin_sent else 0):
                data_acked = min(acked, t.unacked)
                t.outbuf = t.outbuf[data_acked:]
                t.unacked -= data_acked
                t.snd_una = ack
                t.retries = 0
                # A PARTIAL ack leaves bytes still in flight. Clearing
                # last_seg unconditionally meant the retransmit path passed
                # None to len() one RTO later, which raised out of the pump
                # loop -- so every flow after this one in dict order stopped
                # being serviced too, and the connection was reset after a
                # dozen tracebacks.
                t.last_seg = t.outbuf[:t.unacked] if t.unacked else None
                if t.fin_sent and acked > data_acked:
                    t.state = _Tcp.DONE
                    t.closed_at = time.time()

        if payload:
            if t.inbuf:
                # Backed up: the far socket has not taken what we already
                # hold, so this segment is DROPPED WITHOUT AN ACK and uIP
                # retransmits it when we have drained. That is the
                # backpressure the comment below promises -- which the code
                # used to contradict by acking everything into a buffer with
                # no bound, so a slow or hostile external reader let the
                # console push at link rate into unbounded host memory, and
                # any reset then lost acked-but-unwritten bytes silently.
                pass
            elif seq == t.rcv_nxt:
                # send(), not sendall(): these sockets are non-blocking, so
                # sendall raises BlockingIOError the moment the kernel buffer
                # fills -- and BlockingIOError IS an OSError, so a slow peer
                # used to get the connection reset out from under it mid
                # upload. Whatever the kernel would not take is held here and
                # retried; the segment is acked only once all of it is gone,
                # which is what makes the console stop sending.
                try:
                    sent = t.sock.send(payload) if not t.inbuf else 0
                except (BlockingIOError, InterruptedError):
                    sent = 0
                except OSError:
                    self._tcp_reset(key, t)
                    return
                if sent < len(payload):
                    t.inbuf += payload[sent:]
                t.rcv_nxt = (t.rcv_nxt + len(payload)) & 0xFFFFFFFF
                t.last_from_console = time.time()
                self._tcp_out(t, ACK)
            else:
                # A retransmission of something already taken. Re-ack: uIP is
                # waiting for exactly this, and saying nothing costs a whole
                # retransmission timeout.
                self._tcp_out(t, ACK)

        if flags & FIN:
            # A FIN occupies the sequence number AFTER its payload, so the
            # test is against seq+len, not seq. Getting that wrong means a FIN
            # carried on the same segment as the last data (which is exactly
            # what a small HTTP response does) is never acked, and the peer
            # retransmits it until it gives up.
            fin_seq = (seq + len(payload)) & 0xFFFFFFFF
            if not t.peer_fin and fin_seq == t.rcv_nxt:
                t.rcv_nxt = (t.rcv_nxt + 1) & 0xFFFFFFFF
                t.peer_fin = True
                self._tcp_out(t, ACK)
                if not t.inbuf:
                    try:
                        t.sock.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
            elif t.peer_fin and (fin_seq + 1) & 0xFFFFFFFF == t.rcv_nxt:
                self._tcp_out(t, ACK)  # retransmitted FIN: re-ack, say no more

    def _tcp_open(self, key, seq):
        sport, dip, dport = key
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setblocking(False)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.iss = (self.iss + 0x1000) & 0xFFFFFFFF
        t = _Tcp(s, key, self.iss)
        t.rcv_nxt = (seq + 1) & 0xFFFFFFFF
        self.tcp[key] = t
        try:
            s.connect((_int2ip(dip), dport))
        except BlockingIOError:
            pass
        except OSError:
            self._tcp_reset(key, t)

    def _tcp_reset(self, key, t):
        self._rst(key[1], key[0], key[2], t.rcv_nxt, t.snd_nxt, True)
        t.close()
        self.tcp.pop(key, None)

    def _pump_tcp(self):
        now = time.time()
        for key, t in list(self.tcp.items()):
            if t.state == _Tcp.DONE:
                if now - t.closed_at > TIME_WAIT:
                    t.close()
                    self.tcp.pop(key, None)
                continue

            if t.state == _Tcp.CONNECTING:
                err = t.sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if err == 0:
                    try:
                        t.sock.getpeername()
                    except OSError:
                        continue          # not connected yet
                    t.state = _Tcp.SYN_RCVD
                    self._tcp_out(t, SYN | ACK, seq=t.iss)
                    t.snd_nxt = (t.iss + 1) & 0xFFFFFFFF
                    t.sent_at = now
                elif err not in (errno.EINPROGRESS, errno.EALREADY):
                    self.log(f"slirp: connect to {_int2ip(key[1])}:{key[2]} "
                             f"failed ({os.strerror(err)})")
                    self._tcp_reset(key, t)
                continue

            if t.state == _Tcp.SYN_RCVD or t.state == _Tcp.SYN_SENT:
                if self.link_busy():
                    t.sent_at = now
                elif now - t.sent_at > RTO:
                    t.retries += 1
                    if t.retries > MAX_RETRIES:
                        self._tcp_reset(key, t)
                        continue
                    t.sent_at = now
                    if t.state == _Tcp.SYN_RCVD:
                        self._tcp_out(t, SYN | ACK, seq=t.iss)
                    else:
                        self._tcp_out(t, SYN, seq=t.iss)
                continue

            # Drain anything the far side's socket would not take earlier.
            if t.inbuf:
                try:
                    n = t.sock.send(t.inbuf)
                    t.inbuf = t.inbuf[n:]
                    t.touch()
                    if not t.inbuf and t.peer_fin:
                        # The close was held back until its data was written.
                        try:
                            t.sock.shutdown(socket.SHUT_WR)
                        except OSError:
                            pass
                except (BlockingIOError, InterruptedError):
                    pass
                except OSError:
                    self._tcp_reset(key, t)
                    continue

            # Established: read what the far side has, if there is room.
            if not t.sock_eof and len(t.outbuf) < MSS:
                try:
                    data = t.sock.recv(MSS - len(t.outbuf))
                    if data:
                        t.outbuf += data
                    else:
                        if not t.sock_eof:
                            t.eof_at = now
                        t.sock_eof = True
                except (BlockingIOError, InterruptedError):
                    pass
                except OSError:
                    if not t.sock_eof:
                        t.eof_at = now
                    t.sock_eof = True

            if t.unacked:
                if self.link_busy():
                    t.sent_at = now      # clock starts when the wire clears
                elif now - t.sent_at > RTO:
                    t.retries += 1
                    if t.retries > DATA_RETRIES:
                        self.log(f"slirp: giving up on a data segment "
                                 f"unacked for {t.retries} retries")
                        self._tcp_reset(key, t)
                        continue
                    t.sent_at = now
                    self._tcp_out(t, PSH | ACK, t.last_seg, seq=t.snd_una)
            elif t.outbuf:
                t.touch()
                n = min(len(t.outbuf), MSS)
                t.last_seg = t.outbuf[:n]
                t.unacked = n
                self._tcp_out(t, PSH | ACK, t.last_seg, seq=t.snd_una)
                t.snd_nxt = (t.snd_una + n) & 0xFFFFFFFF
                t.sent_at = now
                t.retries = 0
            elif t.sock_eof and not t.fin_sent:
                # uIP has no CLOSE_WAIT: the FIN we forward kills BOTH
                # directions of the console's connection at once, so a client
                # that half-closes right after its request (printf | nc) races
                # the server's reply — whichever of the reply segment and this
                # FIN reaches uIP first wins. For INBOUND forwards the daemon
                # absorbs that: hold the FIN until the console's replies have
                # gone quiet for a moment. Outbound flows (get's end-of-body
                # IS the far server's FIN) keep prompt EOF — the console has
                # nothing left to say on those and the grace would only add
                # latency to every fetch.
                if t.inbound and now - max(t.eof_at, t.last_from_console) \
                        < FIN_GRACE:
                    pass
                else:
                    t.fin_sent = True
                    self._tcp_out(t, FIN | ACK, seq=t.snd_nxt)
                    t.snd_nxt = (t.snd_nxt + 1) & 0xFFFFFFFF
                    t.sent_at = now
                    t.retries = 0
            elif t.fin_sent and now - t.sent_at > RTO:
                t.retries += 1
                if t.retries > MAX_RETRIES:
                    t.close()
                    self.tcp.pop(key, None)
                    continue
                t.sent_at = now
                self._tcp_out(t, FIN | ACK, seq=(t.snd_nxt - 1) & 0xFFFFFFFF)

    # ------------------------------------------------------- port forwards
    def forward_add(self, port, ch=None):
        """Bind a real port and hand every connection to the console.

        This is what makes `httpd` a host rather than something the gateway
        proxies: the socket is ours (the console is behind NAT and unreachable
        from the LAN), but everything above the SYN is the console's."""
        if port in self.forwards:
            # Already ours. A SECOND channel on the SAME port is legitimate
            # since the mc server: two listeners so a ping connection and a
            # join connection can overlap. The port keeps one socket and a
            # SET of owner channels; it closes when the last owner leaves.
            self.fwd_ch.setdefault(port, set()).add(ch)
            return True
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setblocking(False)
        try:
            s.bind((os.environ.get("SNOS_LISTEN_ADDR", "0.0.0.0"), port))
            s.listen(4)
        except OSError as e:
            self.log(f"slirp: cannot bind port {port}: {e}")
            s.close()
            return False
        self.forwards[port] = s
        self.fwd_ch[port] = {ch}
        return True

    def forward_del(self, port, ch=None):
        # Ownership is checked: one application closing a channel must not be
        # able to unbind a port another one is serving on. The socket closes
        # only when the LAST owning channel lets go.
        owners = self.fwd_ch.get(port)
        if ch is not None:
            if owners is None or ch not in owners:
                return
            owners.discard(ch)
            if owners:
                return
        s = self.forwards.pop(port, None)
        self.fwd_ch.pop(port, None)
        if s is not None:
            try:
                s.close()
            except OSError:
                pass

    def forward_del_by_channel(self, ch):
        for port in [p for p, owners in self.fwd_ch.items() if ch in owners]:
            self.forward_del(port, ch)

    def _pump_forwards(self):
        for port, srv in list(self.forwards.items()):
            live = sum(1 for (sp, dip, _dp), t in self.tcp.items()
                       if sp == port and dip == GW_INT
                       and t.state != _Tcp.DONE)
            while True:
                if live >= MAX_INBOUND_PER_PORT:
                    break
                try:
                    conn, addr = srv.accept()
                except (BlockingIOError, InterruptedError):
                    break
                except OSError:
                    break
                conn.setblocking(False)
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                # The console is the SERVER here, so the 4-tuple is inverted
                # relative to an outbound flow: it must look to uIP like a
                # connection arriving at `port` from somewhere on the NAT.
                sport = port
                dport = self.next_eport
                self.next_eport += 1
                if self.next_eport >= 60000:
                    self.next_eport = 40000
                while (sport, GW_INT, dport) in self.tcp:
                    dport = self.next_eport
                    self.next_eport += 1
                    if self.next_eport >= 60000:
                        self.next_eport = 40000
                key = (sport, GW_INT, dport)
                self.iss = (self.iss + 0x1000) & 0xFFFFFFFF
                t = _Tcp(conn, key, self.iss)
                t.state = _Tcp.SYN_SENT
                t.inbound = True
                t.sent_at = time.time()
                self.tcp[key] = t
                live += 1
                self._tcp_out(t, SYN, seq=t.iss)
                t.snd_nxt = (t.iss + 1) & 0xFFFFFFFF

    # ---------------------------------------------------------------- tick
    def tick(self):
        """Move everything that can move. Called from the gateway's main loop,
        which runs at ~250 Hz — four times the console's poll rate, so nothing
        here needs a thread or a selector."""
        self._pump_forwards()
        self._pump_icmp()
        self._pump_udp()
        self._pump_tcp()
        now = time.time()
        for ident, flow in list(self.icmp.items()):
            if now - flow.touched > ICMP_IDLE:
                flow.close()
                self.icmp.pop(ident, None)
        for key, flow in list(self.udp.items()):
            if now - flow.touched > UDP_IDLE:
                flow.close()
                self.udp.pop(key, None)
        for key, t in list(self.tcp.items()):
            if now - t.touched > TCP_IDLE:
                self.log(f"slirp: dropping idle connection {key}")
                t.close()
                self.tcp.pop(key, None)

    def stats_line(self):
        return (f"slirp: in={self.stat_in} out={self.stat_out} "
                f"drop={self.stat_drop} tcp={len(self.tcp)} "
                f"udp={len(self.udp)} fwd={len(self.forwards)}")
