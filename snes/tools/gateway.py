#!/usr/bin/env python3
"""SNOS gateway: the far side of the link — a NAT, not a proxy.

Since the uIP cutover the console runs its own TCP/IP stack and this program
stopped understanding any protocol the console speaks. What is left is
exactly what a slirp box is:

    - the link peer: frames, sequence numbers, checksums, retransmission
      (the same wire discipline the real cable will use at M7);
    - the control channel, three verbs: HELLO ("who are you" -> version and
      this host's address), IPFWD/IPUNFWD (hold or release a real socket for
      an inbound port-forward, because the console is behind the NAT);
    - the NAT itself (slirp.py): user-mode, no root, no utun. IPv4 datagrams
      the console composed ride LINK_CH_IP as a length-prefixed byte stream,
      and every header on the console's side of this file is the 65816's.

Everything else this file used to do — DNS, TLS, HTTP, gzip, image
conversion, held sockets, stream channels — is gone with the protocol that
needed it. TLS is not coming back (period-correct, PLAN.md M11); anything
that needs a modern brain becomes an ordinary HTTP service the console's own
client calls (M12).

    python3 snes/tools/gateway.py

then load build/snosbridge.lua in Mesen's Script Window (which needs "Allow
access to I/O and OS functions" enabled) and type `net status`.
"""
import argparse
import os
import faulthandler
import signal
import socket
import time
import traceback

import slirp

# SNES -> gateway and back. Overridable so the host-side bridge test
# (test/host/bridge_test.py) can run against a private pair of files while a
# real session owns /tmp's.
UP = os.environ.get("SNOS_UP", "/tmp/snos-up.bin")
DOWN = os.environ.get("SNOS_DOWN", "/tmp/snos-down.bin")

FRAME = 256
HDR = 4
SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
F_DATA = 0x01
F_SYN = 0x02
CH_CTRL, CH_PAD = 0, 0xFF
# Raw IPv4 datagrams (M11), on a channel outside the old stream range. Both
# record parsers have always skipped an unknown channel by its length byte.
# Datagrams are length-prefixed (2 bytes, little-endian) inside the byte
# stream; see kernel/ip.c.
CH_IP = 0x10

# The control channel's surviving verbs. The retired ones ($01 FETCH,
# $02 CLOSE, $03 IMAGE, $07 CONNECT, $08 LISTEN, $09 HANGUP, $0A WEB and the
# replies $81 OPEN / $82 EOF) keep their numbers reserved: a code reused for
# a new verb would make an old ROM act on the wrong one.
C2S_HELLO = 0x04
C2S_IPFWD, C2S_IPUNFWD = 0x0B, 0x0C
S2C_ERR, S2C_HELLO = 0x83, 0x84

# 2 = "I do user-mode NAT": the console may run its own IP stack and send
# LINK_CH_IP records (M11). The console asks with HELLO before it sends a
# single packet — and since the cutover this is the only kind of daemon the
# console works with at all; a v1 answer leaves it saying so in `net status`.
GW_VERSION = 2


def checksum(f):
    # sum() runs at C speed; the per-byte Python loop this replaces was paid
    # twice per exchange and scales with the console's poll rate now that the
    # bridge answers every poll rather than one per video frame.
    return (sum(f) - f[SUM]) & 0xFF


def local_ip():
    """The address the SNES should be reached at, since the SNES has no
    address of its own outside the NAT — anything it serves is served through
    this host's forwards.

    Asks the routing table which source address it would use to reach a public
    one. No packets are sent (UDP connect just binds), no interface parsing, no
    dependency, and it picks the right interface on a machine with several."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))     # TEST-NET-1: reserved, never routed
        return s.getsockname()[0]
    except OSError:
        return "0.0.0.0"                # no route at all: offline, and say so
    finally:
        s.close()


# --debug: every IPv4 datagram both ways, as hex. The link is the console's
# only voice, so when something goes wrong on the wire this is the whole
# truth of what crossed it - and hex, not raw bytes, because this lands in
# somebody's terminal.
DEBUG = False


def _hexdump(tag, data):
    print(f"{tag} {len(data)}B {data.hex()}")


class Gateway:
    def __init__(self):
        self.tx_seq = 0
        self.rx_next = 0
        self.awaiting = False
        self.inflight = None
        # "I have restarted; my sequence begins again here." Held until the
        # SNES acknowledges a frame, which is the only proof it adopted the
        # new base. Without it, restarting this daemon alone deadlocks the
        # link forever: we come back at seq 0, the SNES is still waiting for
        # seq 200, it never acks, and stop-and-wait resends the same frame
        # until the SNES reboots.
        self.syn = True
        self.peer_syn_seen = False
        # F_SYN tells the peer OUR sequence began again. It does not tell us
        # THEIRS, so on the first frame after a restart we adopt whatever
        # sequence they are already using. Announce and adopt are both
        # required; each alone fixes exactly one direction.
        self.rx_resync = True
        self.out = []            # [ch, bytearray] queued for the SNES
        # True when out[0] has already had bytes taken from it: a HELLO may
        # jump the queue, but never into the middle of a half-sent record.
        self.out_started = False
        self.cstate = 0
        self.ctype = 0
        self.clen = 0
        self.cbuf = bytearray()
        # SNOS_GW_STATS=1 telemetry: how every DOWN write was spent. Kept
        # through the cutover — "half-empty frames" and "full frames diluted
        # by empty exchanges" are opposite problems and totals cannot tell
        # them apart (PLAN.md, the 42%-fill lesson).
        self.st_resend = 0       # re-sent the in-flight frame (dup ack)
        self.st_fresh_data = 0   # built a new frame carrying payload
        self.st_fresh_empty = 0  # built a new frame with nothing to say
        self.st_payload = 0      # payload bytes across fresh data frames
        self.st_qdepth_sum = 0   # queue depth when building, for a mean
        self.st_qdepth_n = 0

        # The NAT, and the reassembly buffer for the datagram stream coming
        # the other way. Every datagram it produces is queued as an ordinary
        # CH_IP record, so it inherits the link's reliability, ordering and
        # backpressure without knowing any of them exist.
        self.slirp = slirp.Slirp(
            self.push_ip,
            link_busy=lambda: any(ch == CH_IP for ch, _ in self.out))
        self.ipbuf = bytearray()

    # ------------------------------------------------------------ outbound
    def push(self, ch, data):
        self.out.append([ch, bytearray(data)])

    def push_ctrl(self, typ, payload):
        self.push(CH_CTRL, bytes([typ, len(payload)]) + payload)

    def push_hello(self, payload):
        """Answer a HELLO ahead of whatever bulk data is queued.

        A HELLO names no channel and terminates nothing, so nothing downstream
        depends on where it sits in the stream — and `net status` is the one
        message that is worthless late: behind a long queue it once took
        eighty seconds to arrive, and the console reported the daemon missing
        and then healthy again with nothing wrong at either end.

        The one place it cannot go is in front of a control record that is
        HALF SENT. Channel 0 is a single byte stream on the far side,
        reassembled by a [type][len][payload] state machine, so splicing a
        whole message into the middle of a partly transmitted one would
        corrupt both. In that case it goes second — behind the record that
        has already started, and still in front of the backlog."""
        rec = [CH_CTRL, bytearray([S2C_HELLO, len(payload)]) + payload]
        mid_message = (self.out and self.out_started
                       and self.out[0][0] == CH_CTRL)
        if mid_message:
            self.out.insert(1, rec)
        else:
            self.out.insert(0, rec)
            self.out_started = False

    def push_ip(self, datagram):
        """One IPv4 datagram toward the console, length-prefixed."""
        if DEBUG:
            _hexdump("IP->snes", datagram)
        self.push(CH_IP, bytes([len(datagram) & 0xFF,
                                (len(datagram) >> 8) & 0xFF]) + datagram)

    def teardown(self):
        """Everything a dead session owned, released: the queued output, the
        NAT's flows and its bound forwards. The flows named sockets that
        belonged to the dead session; the forwards hold ports the next run of
        the same application needs."""
        self.out.clear()
        self.out_started = False
        self.slirp.close()
        self.ipbuf = bytearray()

    # ------------------------------------------------------------- inbound
    def ctrl_byte(self, b):
        if self.cstate == 0:
            self.ctype, self.cstate = b, 1
        elif self.cstate == 1:
            self.clen, self.cbuf = b, bytearray()
            self.cstate = 2 if b else 0
            if self.cstate == 0:
                self.dispatch()
        else:
            self.cbuf.append(b)
            if len(self.cbuf) >= self.clen:
                self.cstate = 0
                self.dispatch()

    def dispatch(self):
        if self.ctype == C2S_HELLO:
            # Jumped ahead of queued bulk data: a liveness probe that waits
            # behind a download reports the opposite of the truth.
            self.push_hello(bytes([GW_VERSION]) + local_ip().encode())
        elif self.ctype == C2S_IPFWD and len(self.cbuf) >= 3:
            ch = self.cbuf[0]
            port = self.cbuf[1] | (self.cbuf[2] << 8)
            if self.slirp.forward_add(port, ch):
                print(f"gateway: forwarding port {port} to the console's stack")
            else:
                # Reported against the channel, so "address already in use"
                # reaches the application through the path it already has for
                # a failed listen rather than as silence.
                self.push_ctrl(S2C_ERR, bytes([ch, 1]))
        elif self.ctype == C2S_IPUNFWD and len(self.cbuf) >= 3:
            port = self.cbuf[1] | (self.cbuf[2] << 8)
            self.slirp.forward_del(port, self.cbuf[0])
        # Any other type is a verb this daemon no longer speaks (or never
        # did). The record was already consumed whole by the state machine,
        # so an unknown verb costs nothing and desynchronises nothing.

    def ip_bytes(self, data):
        """Stream bytes from the console's IP channel: reassemble and route.

        A length that could not possibly be a datagram means the stream
        desynchronised, and nothing marks a boundary to resynchronise ON — so
        the buffer is dropped whole and the console is told, by the only means
        available on a byte stream, to start again: F_SYN at the next session
        reset. In practice this fires only after a restart on one side."""
        self.ipbuf += data
        while len(self.ipbuf) >= 2:
            ln = self.ipbuf[0] | (self.ipbuf[1] << 8)
            if ln == 0 or ln > slirp.MTU:
                print(f"gateway: IP stream desync (length {ln}), resetting")
                self.ipbuf = bytearray()
                return
            if len(self.ipbuf) < 2 + ln:
                return
            # NOTHING the console sends may take this process down. The link
            # is the console's only way to say anything, so a daemon that
            # dies on a surprising datagram takes away the very channel that
            # would report it — the failure looks like "the gateway stopped
            # answering", miles from the cause. (An IPv6 resolver address in
            # /etc/resolv.conf did exactly this, once.)
            try:
                if DEBUG:
                    _hexdump("snes->IP", bytes(self.ipbuf[2:2 + ln]))
                self.slirp.input(bytes(self.ipbuf[2:2 + ln]))
            except Exception:
                traceback.print_exc()
            del self.ipbuf[:2 + ln]

    def deliver(self, f):
        o = HDR
        while o + 2 <= FRAME:
            ch, ln = f[o], f[o + 1]
            if ch == CH_PAD or o + 2 + ln > FRAME:
                break
            if ch == CH_CTRL:
                for i in range(ln):
                    self.ctrl_byte(f[o + 2 + i])
            elif ch == CH_IP:
                self.ip_bytes(bytes(f[o + 2:o + 2 + ln]))
            # Any other channel is a stream record from a pre-cutover ROM:
            # stepped over by its length, exactly as the console steps over
            # records it no longer knows.
            o += 2 + ln

    # -------------------------------------------------------- frame builder
    def build(self):
        f = bytearray(FRAME)
        f[SEQ] = self.tx_seq
        f[ACK] = (self.rx_next - 1) & 0xFF
        o, any_data = HDR, False
        while self.out and o + 2 < FRAME:
            ch, data = self.out[0]
            n = min(len(data), FRAME - o - 2)
            if n <= 0:
                break
            f[o] = ch
            f[o + 1] = n
            f[o + 2:o + 2 + n] = data[:n]
            del data[:n]
            o += 2 + n
            if data:
                self.out_started = True
            else:
                self.out.pop(0)
                self.out_started = False
            any_data = True
        for i in range(o, FRAME):
            f[i] = CH_PAD
        if any_data:
            f[FLAGS] |= F_DATA
        if self.syn:
            f[FLAGS] |= F_SYN
        f[SUM] = 0
        f[SUM] = checksum(f)
        return f, any_data

    def on_frame(self, f):
        if f[SUM] != checksum(f):
            return                      # corrupt: say nothing, peer resends
        if self.rx_resync:
            self.rx_resync = False
            self.rx_next = f[SEQ]

        if self.awaiting and f[ACK] == self.tx_seq:
            self.awaiting = False
            self.tx_seq = (self.tx_seq + 1) & 0xFF
            # It acked us, so it has our sequence base. Stop announcing.
            self.syn = False

        # Mirror of the SNES's handling: the peer restarted, so adopt its
        # sequence once per episode and drop everything the old session
        # owned. Once per episode, not once per frame — honouring it on every
        # frame would re-deliver retransmissions, since the seq check is what
        # discards those.
        if f[FLAGS] & F_SYN:
            if not self.peer_syn_seen:
                self.peer_syn_seen = True
                self.rx_next = f[SEQ]
                self.teardown()
                self.cstate, self.ctype, self.cbuf = 0, 0, bytearray()
                # The in-flight frame belongs to the dead session as surely
                # as the queue did: left armed, next_out() kept resending it
                # and the FRESH console adopted, acked and delivered it -
                # possibly the tail half of an old record, read by the new
                # session's reassembler as a record start.
                self.awaiting = False
                self.inflight = None
                print("gateway: SNES restarted, session reset")
        else:
            self.peer_syn_seen = False

        if (f[FLAGS] & F_DATA) and f[SEQ] == self.rx_next:
            self.deliver(f)
            self.rx_next = (self.rx_next + 1) & 0xFF

    def next_out(self):
        if not self.awaiting:
            self.st_qdepth_sum += sum(len(d) for _, d in self.out)
            self.st_qdepth_n += 1
            f, any_data = self.build()
            self.inflight = f
            if any_data:
                self.awaiting = True
                self.st_fresh_data += 1
                self.st_payload += sum(
                    f[i + 1] for i in self._record_offsets(f))
            else:
                self.st_fresh_empty += 1
            return f
        self.st_resend += 1
        if os.environ.get("SNOS_GW_STATS") == "2":
            print(f"resend: seq={self.tx_seq} n={self.st_resend} "
                  f"t={time.time():.3f}")
        self.inflight[ACK] = (self.rx_next - 1) & 0xFF
        self.inflight[SUM] = 0
        self.inflight[SUM] = checksum(self.inflight)
        return self.inflight

    @staticmethod
    def _record_offsets(f):
        """Offsets of each [ch][len] record header in a built frame."""
        offs, o = [], HDR
        while o + 2 <= FRAME:
            ch, ln = f[o], f[o + 1]
            if ch == CH_PAD or o + 2 + ln > FRAME:
                break
            offs.append(o)
            o += 2 + ln
        return offs

    def stats_line(self):
        q = (self.st_qdepth_sum / self.st_qdepth_n) if self.st_qdepth_n else 0
        per = (self.st_payload / self.st_fresh_data) if self.st_fresh_data else 0
        return (f"stats: fresh_data={self.st_fresh_data} "
                f"({per:.1f} B/frame) fresh_empty={self.st_fresh_empty} "
                f"resend={self.st_resend} payload={self.st_payload} "
                f"qdepth_mean={q:.0f}")


def main():
    global DEBUG
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true",
                    help="hex-dump every IP datagram in both directions")
    DEBUG = ap.parse_args().debug

    # SIGUSR1 dumps the stack: the only way to see WHERE a wedged daemon is
    # wedged without guessing.
    faulthandler.register(signal.SIGUSR1, all_threads=True)

    # Do NOT truncate here. The bridge owns the length of both files — it
    # truncates them when the emulator starts a session — and a second truncator
    # silently invalidates the offset the OTHER end is reading from. That was a
    # real, reproducible deadlock: start the daemon, stop it, start it again,
    # and the SNES never saw another frame no matter how many times you reloaded
    # the bridge script, because the bridge was still reading at an offset far
    # past the end of a file this had just emptied underneath it.
    #
    # Instead, join the stream where it currently is: skip whatever was written
    # before we existed, and append.
    for path in (UP, DOWN):
        if not os.path.exists(path):
            open(path, "wb").close()
    gw = Gateway()
    up_off = os.path.getsize(UP) // FRAME * FRAME
    down = open(DOWN, "ab", buffering=0)
    last_moved = 0.0

    print(f"gateway: reading {UP}, writing {DOWN}")
    print("gateway: load tools/netbridge.lua in Mesen's Script Window")
    stats = os.environ.get("SNOS_GW_STATS")
    stats_at = time.time() + 5.0
    try:
        while True:
            if stats and time.time() >= stats_at:
                stats_at = time.time() + 5.0
                print(gw.stats_line())
                if gw.slirp.stat_in or gw.slirp.stat_out:
                    print(gw.slirp.stats_line())
            moved = False
            try:
                # The bridge truncates both files when it starts, so a restarted
                # emulator leaves this loop reading from an offset past the end
                # of a brand new file. It then picks up mid-stream once the file
                # grows back past that point, which desynchronises the protocol
                # and presents as a dead gateway — with the gateway right there,
                # running, looking healthy.
                #
                # You restart the emulator far more often than this daemon, so
                # this is the common case, not an edge one.
                if os.path.getsize(UP) < up_off:
                    print("gateway: link restarted, resetting")
                    up_off = 0
                    # Tear the old session down BEFORE replacing the object,
                    # so its NAT flows and forwards release their sockets.
                    gw.teardown()
                    gw = Gateway()
                    down.close()
                    down = open(DOWN, "ab", buffering=0)

                with open(UP, "rb") as f:
                    f.seek(up_off)
                    chunk = f.read(FRAME)
                if chunk and len(chunk) == FRAME:
                    up_off += FRAME
                    gw.on_frame(bytearray(chunk))
                    moved = True
            except FileNotFoundError:
                pass

            # STRICTLY one frame out per frame in. The real link is clocked by
            # the SNES: the gateway physically cannot transmit unless the SNES
            # runs an exchange, and the file bridge has to model that or it is
            # not the same protocol.
            #
            # Emitting while idle "so acks keep flowing" seems harmless and is
            # not: the loop spins at 250 Hz against a console polling at 60, so
            # the down file grows four times faster than it is consumed. The
            # bridge falls permanently behind, reads frames from minutes ago,
            # and the file reaches megabytes in a couple of minutes.
            # The NAT moves on wall-clock time, not on the console's polling:
            # a socket becomes readable when it becomes readable. Ticked on
            # BOTH paths for that reason -- ticking only when a frame arrived
            # would make every reply wait for the console to ask again.
            try:
                gw.slirp.tick()
            except Exception:
                traceback.print_exc()

            if moved:
                down.write(bytes(gw.next_out()))
                last_moved = time.time()
            else:
                # Adaptive, not fixed: at the console's old 60 Hz poll rate a
                # 4 ms nap was <7% of the round trip, but the console can now
                # poll many times per video frame and a fixed nap becomes the
                # wall. Stay snappy while a transfer is live, back off to the
                # old rate once the link has been quiet for a second.
                if time.time() - last_moved < 1.0:
                    time.sleep(0.0005)
                else:
                    time.sleep(0.004)
    except KeyboardInterrupt:
        # Daemon threads die with the process, but sockets and bound ports do
        # not deserve to be left to the OS's timing.
        gw.teardown()
        print("\ngateway: stopped")


if __name__ == "__main__":
    main()
