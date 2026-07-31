#!/usr/bin/env python3
"""SNOS gateway: the far side of the link, doing everything the SNES cannot.

The protocol itself is covered hermetically by snes/test/net.lua, which speaks
it frame for frame; this program is the path to the real internet and has been
exercised for real by `get`, `view`, `httpd`, `netecho`, `irc` and `vnc` (the
2026-07 sessions, measured end to end). The parts no test reaches are the
real-internet edges: DNS failures, slow TLS peers, misbehaving servers.

    python3 snes/tools/gateway.py

then load snes/tools/netbridge.lua in Mesen's Script Window (which needs
"Allow access to I/O and OS functions" enabled) and type `get example.com`.

WHAT LIVES HERE, AND WHY
    DNS, TCP, TLS, HTTP, redirects, chunked encoding and gzip. Every one of
    those is either impossible or unreasonable on a 3.58 MHz 65816 — a TLS
    handshake alone runs to minutes — and all of them are a few lines here. The
    SNES receives a status code and plain bytes.

    This is a forward proxy, not a man-in-the-middle: it makes ordinary,
    honest TLS connections as itself. Nothing forges a certificate, so there is
    no CA to install and no cert store on the SNES.

    Its own traffic arrives through a file the bridge writes, so it cannot
    accidentally become an open proxy on the network. The sockets it DOES
    open are the ones the SNES asks for: C2S_LISTEN binds server ports on
    LISTEN_ADDR (every interface, deliberately — see the comment there), so
    what a SNES app serves is exactly as exposed as that app makes it.
"""
import gzip
import io
import os
import faulthandler
import signal
import socket
import struct

import traceback
import threading
import time
import urllib.error
import urllib.request

UP = "/tmp/snos-up.bin"       # SNES -> gateway
DOWN = "/tmp/snos-down.bin"   # gateway -> SNES

FRAME = 128
HDR = 4
SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
F_DATA = 0x01
CH_CTRL, CH_PAD = 0, 0xFF

C2S_FETCH, C2S_CLOSE, C2S_IMAGE, C2S_HELLO = 0x01, 0x02, 0x03, 0x04
C2S_CONNECT = 0x07
C2S_LISTEN, C2S_HANGUP = 0x08, 0x09
# 0x0A was C2S_WEB, retired with the `web` app (vnc superseded it); do not
# reuse the code — an old ROM against a new gateway must fail loudly, not
# alias onto something else.

# Servers bind on every interface deliberately: the point is a SNES serving a
# page to someone else's phone at a conference. That IS an exposure decision —
# what a SNES app serves becomes reachable by anything that can route to this
# host — and it is made here, once, where it can be seen and changed.
LISTEN_ADDR = "0.0.0.0"
S2C_OPEN, S2C_EOF, S2C_ERR, S2C_HELLO = 0x81, 0x82, 0x83, 0x84

F_SYN = 0x02
GW_VERSION = 1

USER_AGENT = "SNOS/0.1 (Super Nintendo; via gateway)"
# What the SNES will actually receive. It arrives at roughly 600 B/s, so half a
# megabyte is already fourteen minutes of transfer — the cap is a kindness, not
# a memory limit.
MAX_BODY = 512 * 1024

# A source image is a different matter: it gets scaled down to ~43 KB before
# anything crosses the link, so its original size costs the SNES nothing. A
# 960 KB PNG was being cut to MAX_BODY and then failing to decode with "image
# file is truncated", which points at the picture rather than at the cap that
# damaged it.
MAX_IMAGE_SOURCE = 16 * 1024 * 1024


def checksum(f):
    return sum(b for i, b in enumerate(f) if i != SUM) & 0xFF


def ctrl_channel(rec):
    """Which channel a queued control record is about, or None if it is about
    the session rather than a stream.

    push_ctrl queues exactly one whole message per record, so the payload
    starts at byte 2 and — for every per-channel type there is — its first byte
    is the channel. S2C_HELLO belongs to nobody and answers None."""
    if len(rec) >= 3 and rec[0] in (S2C_OPEN, S2C_EOF, S2C_ERR):
        return rec[2]
    return None


SCREEN_W, SCREEN_H = 256, 224
IMG_MAGIC = b"SIMG"


def to_img(data, out_w=None, out_h=None):
    """Decode any image and render it as SNES Mode 3 tile data.

    Everything expensive happens here rather than on the console, which is the
    same division of labour that makes TLS possible: a 65816 cannot decode a
    JPEG, scale it, or quantise a palette in any reasonable time, and all three
    are a few lines with Pillow.

    Two details matter more than they look:

    Aspect ratio is preserved by fitting INSIDE 256x224 — never stretched, never
    cropped. A portrait source therefore pillarboxes.

    Colour is reduced to the SNES's 15-bit BGR555 space BEFORE quantising to
    256, not after. Picking 256 colours in 24-bit and truncating afterwards
    moves every palette entry by up to seven levels and visibly muddies a photo.
    """
    from PIL import Image

    im = Image.open(io.BytesIO(data))
    if im.mode in ("RGBA", "LA", "P"):
        # Composite onto black rather than letting Pillow drop the alpha, which
        # would leave transparent pixels as whatever happened to be underneath.
        bg = Image.new("RGB", im.size, (0, 0, 0))
        im = im.convert("RGBA")
        bg.paste(im, mask=im.split()[-1])
        im = bg
    else:
        im = im.convert("RGB")

    # Callers may ask for a smaller window than a photograph gets; the
    # dimensions stay parameters even though every current caller uses the
    # full screen.
    tw = out_w or SCREEN_W
    th = out_h or SCREEN_H
    sw, sh = im.size
    scale = min(tw / sw, th / sh)
    w, h = max(1, round(sw * scale)), max(1, round(sh * scale))
    im = im.resize((w, h), Image.LANCZOS)

    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            px[x, y] = (r & 0xF8, g & 0xF8, b & 0xF8)

    q = im.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)

    # Pad to whole tiles and centre on TILE boundaries, so the console builds a
    # trivial tilemap and never has to transpose anything.
    tw, th = (w + 7) // 8, (h + 7) // 8
    canvas = Image.new("P", (tw * 8, th * 8), 0)
    canvas.putpalette(q.getpalette())
    canvas.paste(q, ((tw * 8 - w) // 2, (th * 8 - h) // 2))

    pal = list(q.getpalette()[:768])
    pal += [0] * (768 - len(pal))

    out = bytearray(IMG_MAGIC)
    out += struct.pack("<HBBHHI", 1, 3, 0, tw, th, 0)
    for i in range(256):
        r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        out += struct.pack("<H", ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3))

    src = canvas.load()
    for ty in range(th):
        for tx in range(tw):
            # 8bpp is PLANE-GROUP major: bytes 0-15 are planes 0 and 1
            # interleaved across all eight rows, 16-31 are planes 2/3, and so
            # on. Row-major packing assembles perfectly and renders as
            # horizontal smear, which is a miserable thing to debug on hardware.
            for plane in range(0, 8, 2):
                for row in range(8):
                    lo = hi = 0
                    for bit in range(8):
                        v = src[tx * 8 + bit, ty * 8 + row]
                        lo = (lo << 1) | ((v >> plane) & 1)
                        hi = (hi << 1) | ((v >> (plane + 1)) & 1)
                    out.append(lo)
                    out.append(hi)
    return bytes(out)


def local_ip():
    """The address the SNES should be reached at, since the SNES has no address
    of its own — anything it ever serves is served through this host.

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


class Gateway:
    def __init__(self):
        self.tx_seq = 0
        self.rx_next = 0
        self.awaiting = False
        # "I have restarted; my sequence begins again here." Held until the SNES
        # acknowledges a frame, which is the only proof it adopted the new base.
        # Without it, restarting this daemon alone deadlocks the link forever:
        # we come back at seq 0, the SNES is still waiting for seq 200, it never
        # acks, and stop-and-wait resends the same frame until the SNES reboots.
        self.syn = True
        # Bumped whenever the session resets. A fetch thread started before the
        # reset captures the old value and drops its results instead of
        # injecting a dead session's bytes into the new one.
        self.gen = 0
        self.peer_syn_seen = False
        # F_SYN tells the peer OUR sequence began again. It does not tell us
        # THEIRS, and they have no reason to restart just because we did — so on
        # the first frame after a restart we adopt whatever sequence they are
        # already using. Without this the deadlock simply reverses: we come back
        # at rx_next=0, the SNES is calmly sending seq=1, and we reject every
        # frame it will ever send. Announce and adopt are both required; each
        # alone fixes exactly one direction.
        self.rx_resync = True
        # Live sockets, by channel, for connections the SNES owns rather than
        # requests it made. A fetch is a request-and-forget; these stay open and
        # carry bytes both ways until one side hangs up.
        self.socks = {}
        self.listeners = {}      # channel -> listening socket
        # Guards chgen; separate from the send queue's lock because a thread
        # holding one must never be waiting on the other.
        self.plock = threading.Lock()
        # Per channel, bumped every time the SNES closes one. `gen` is the same
        # idea for the whole session; this is the same idea for one channel, and
        # it is needed because forgetting what is QUEUED does not stop a worker
        # that has not queued anything yet: a request cancelled while its
        # worker was still starting up would otherwise claim the channel and
        # push a whole body into one the console had already given back.
        # Captured when the request is accepted, checked before anything is
        # claimed or sent.
        self.chgen = {}
        self.inflight = None
        self.out = []            # [ch, bytearray] queued for the SNES
        # True when out[0] has already had bytes taken from it. Only the head
        # can be part-sent, and knowing whether it is decides two things: where
        # a HELLO may safely jump to, and which records forget() may throw away.
        self.out_started = False
        self.lock = threading.Lock()
        self.cstate = 0
        self.ctype = 0
        self.clen = 0
        self.cbuf = bytearray()
        # SNOS_GW_STATS=1 telemetry: how every DOWN write was spent. The
        # 42%-fill question (PLAN.md) is ambiguous from totals alone — 49152
        # bytes over 947 frames is EITHER half-empty data frames (a producer
        # problem) or full frames diluted by empty/retransmit exchanges (a
        # pacing problem) — and those want opposite fixes. This is the
        # instrument that tells them apart.
        self.st_resend = 0       # re-sent the in-flight frame (dup ack)
        self.st_fresh_data = 0   # built a new frame carrying payload
        self.st_fresh_empty = 0  # built a new frame with nothing to say
        self.st_payload = 0      # payload bytes across fresh data frames
        self.st_qdepth_sum = 0   # queue depth when building, for a mean
        self.st_qdepth_n = 0

    # ------------------------------------------------------------ outbound
    def push(self, ch, data, gen=None):
        with self.lock:
            if gen is not None and gen != self.gen:
                return          # a fetch from a session that no longer exists
            self.out.append([ch, bytearray(data)])

    def push_ctrl(self, typ, payload, gen=None):
        self.push(CH_CTRL, bytes([typ, len(payload)]) + payload, gen)

    def push_hello(self, payload):
        """Answer a HELLO ahead of whatever bulk data is queued.

        A HELLO names no channel and terminates nothing, so nothing downstream
        depends on where it sits in the stream — and `net status` is the one
        message that is worthless late. Behind a 49 KB window it takes eighty
        seconds to arrive, which is how the console came to report the gateway
        missing and then healthy again a minute later, with nothing wrong at
        either end.

        S2C_OPEN, S2C_EOF and S2C_ERR must NOT do this and do not: an EOF has to
        stay behind the bytes it terminates, and an OPEN has to precede them.
        Only a session-level message can overtake anything.

        The one place it cannot go is in front of a control record that is HALF
        SENT. Channel 0 is a single byte stream on the far side, reassembled by
        a [type][len][payload] state machine, so splicing a whole message into
        the middle of a partly transmitted one would corrupt both. In that case
        it goes second — behind the record that has already started, and still
        in front of the backlog."""
        rec = [CH_CTRL, bytearray([S2C_HELLO, len(payload)]) + payload]
        with self.lock:
            mid_message = (self.out and self.out_started
                           and self.out[0][0] == CH_CTRL)
            if mid_message:
                self.out.insert(1, rec)
            else:
                self.out.insert(0, rec)
                self.out_started = False

    def forget(self, ch):
        """Throw away everything still queued for a channel the SNES has
        finished with.

        Without this, a dead transfer keeps being transmitted long after the
        application that asked for it has gone: 49 KB, eighty seconds of a
        615 B/s link, spent on a body nobody will ever see. Three separate
        symptoms came out of that one omission (in the era of the `web` app,
        which is how it was found), and channel REUSE is why —

          the next request is handed channel 1 again and receives the tail of
          the dead body, S2C_OPEN and all, before its own, so two payloads end
          up interleaved in one cache file and the result is garbage;

          a second page queues behind the first one's corpse and the app's own
          timeout fires before a byte of it arrives;

          and every control message queues there too, which is why a HELLO went
          unanswered for a minute. push_hello() now steps around that one, but
          the queue should not have been full of a dead page to begin with.

        test/net.lua, the canned gateway the suite runs against, has done this
        since it was written and says why. This one never got the same
        treatment, which is precisely why the tests could not see it.

        NOT done in drop(). That is the gateway noticing the far end went away,
        and the bytes already queued are ones the application still wants — a
        half-closed HTTP response consists of exactly those. Only the SNES
        saying C2S_CLOSE or C2S_HANGUP means "stop, I am not reading this".

        A control record that is already half transmitted is kept, because
        truncating a message mid-payload would desynchronise the far side's
        reassembly of channel 0. That leaves at most one stale S2C_EOF or
        S2C_ERR in flight, and the kernel ignores both for a channel it has
        freed."""
        with self.lock:
            head = self.out[0] if self.out else None
            keep = []
            for i, rec in enumerate(self.out):
                if rec[0] == CH_CTRL:
                    if ctrl_channel(rec[1]) == ch and not (
                            i == 0 and self.out_started):
                        continue
                elif rec[0] == ch:
                    continue
                keep.append(rec)
            if not keep or keep[0] is not head:
                self.out_started = False
            self.out = keep

    def stale(self, ch, cgen):
        """Has the console let this channel go since the work began?

        Channels are reused, so "let go" and "handed to something else" are the
        same event as far as anything already in flight is concerned. Answering
        yes means: claim nothing, send nothing, report nothing."""
        with self.plock:
            return cgen is not None and self.chgen.get(ch, 0) != cgen

    def released(self, ch):
        """The SNES has finished with a channel. Everything started before this
        moment is now working for nobody."""
        with self.plock:
            self.chgen[ch] = self.chgen.get(ch, 0) + 1

    def reply(self, ch, status, flags, body, gen, cgen=None):
        """The whole answer to one request: status, body, end of body.

        One method because the three parts are not independent. Deciding
        separately whether to send each of them is how half an answer ends up in
        a channel that has since been handed to something else — an S2C_OPEN
        with nothing behind it, or a body with no status in front of it. The
        channel is tested once, and then either all of it goes or none does."""
        if self.stale(ch, cgen):
            print(f"  ch{ch} was closed; {len(body)} bytes not sent")
            return
        self.push_ctrl(S2C_OPEN, struct.pack("<BHB", ch, status, flags), gen)
        # Chunked so one huge stream cannot monopolise the frame builder.
        for i in range(0, len(body), 512):
            self.push(ch, body[i:i + 512], gen)
        self.push_ctrl(S2C_EOF, bytes([ch]), gen)

    def drop(self, ch, gen=None):
        """Close a stream and tell the SNES, once. Both directions can discover
        the end independently — the socket erroring on send, the reader thread
        seeing EOF — and a channel closed twice would send a stray EOF that the
        next connection on that channel would receive as its own.

        The EOF carries the caller's generation like every other push: a pump
        thread from a pre-reset session that dies AFTER the reset must not
        inject an EOF into whatever the new session is doing with the channel."""
        sock = self.socks.pop(ch, None)
        if sock is None:
            return
        try:
            sock.close()
        except OSError:
            pass
        self.push_ctrl(S2C_EOF, bytes([ch]), gen)
        print(f"  ch{ch} closed")

    def teardown(self):
        """Everything a dead session owned, released: queued output, every
        worker's claim on its channel, every live socket, every bound port.

        Three callers, and every one of them used to leak: the F_SYN handler
        reset the queue but left sockets and listeners alive, so a reused
        channel number after a console reboot wrote the NEW session's bytes
        into the OLD session's TCP connection; the link-restart path built a
        fresh Gateway and orphaned the old accept threads, which kept the port
        bound until the daemon was killed ('listen failed: address in use' on
        the next run of the same app); and Ctrl-C closed nothing at all.
        Closing the listener is also what breaks its accept loop — the thread
        checks gen against the OBJECT it was born on, which is exactly why
        orphaning the object could never stop it."""
        with self.lock:
            self.out.clear()
            self.out_started = False
        for ch in set(self.chgen) | set(self.socks) | set(self.listeners) | set(range(8)):
            self.released(ch)
        for sock in list(self.socks.values()):
            try:
                sock.close()
            except OSError:
                pass
        self.socks.clear()
        for srv in list(self.listeners.values()):
            try:
                srv.close()
            except OSError:
                pass
        self.listeners.clear()

    def do_connect(self, ch, hostport, gen, cgen=None):
        """A plain TCP stream. The gateway still owns DNS and the socket, but
        the bytes in both directions belong to whatever the SNES is running."""
        host, _, port = hostport.partition(":")
        try:
            port = int(port) if port else 80
        except ValueError:
            port = 80
        try:
            sock = socket.create_connection((host, port), timeout=20)
        except OSError as e:
            print(f"  connect failed: {e}")
            self.push_ctrl(S2C_ERR, bytes([ch, 1]), gen)
            return
        sock.settimeout(None)
        # Connecting can take twenty seconds; the app can be long gone. The
        # close would have found no socket, so this would leave one open with
        # nobody on either end of it.
        if self.stale(ch, cgen):
            print(f"  ch{ch} was closed before the connection came up")
            try:
                sock.close()
            except OSError:
                pass
            return
        self.socks[ch] = sock
        print(f"  connected ch{ch} -> {host}:{port}")
        self.push_ctrl(S2C_OPEN, struct.pack("<BHB", ch, 200, 0), gen)
        self.pump(ch, sock, gen)

    def pump(self, ch, sock, gen):
        """Socket -> SNES until the peer hangs up.

        Reads are capped well under the receive ring so a burst cannot arrive
        faster than the link drains it; the queue is the backpressure, and the
        SNES refuses to ack a frame it has no room for."""
        try:
            while True:
                data = sock.recv(512)
                if not data:
                    # The PEER stopped sending. That is not the end of the
                    # connection: `printf | nc` and every HTTP client half-close
                    # after the request and then wait for the answer. Closing
                    # here threw the response away before it was written, which
                    # is exactly what it looked like — an echo server that
                    # accepted, received, and replied to nobody.
                    self.push_ctrl(S2C_EOF, bytes([ch]), gen)
                    return
                if gen is not None and gen != self.gen:
                    return              # session reset under us
                self.push(ch, data, gen)
                # Do not race ahead of a 615 B/s link with an unbounded queue.
                while sum(len(d) for _, d in self.out) > 4096:
                    time.sleep(0.02)
                    if gen is not None and gen != self.gen:
                        return
        except OSError:
            pass
        self.drop(ch, gen)

    def do_listen(self, ch, port, gen, cgen=None):
        """Bind, then hand ONE connection at a time to the SNES on this channel.

        One at a time because there are three channels total and a console that
        moves 615 B/s; a queue of parallel requests would be a queue of things
        timing out. The listener stays bound between connections, so the app
        loops "wait for OPEN, serve, close" without re-binding and without
        losing the port to a straggler.

        A browser opens six connections at once and will therefore look broken;
        curl and a phone fetching one page will not."""
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((LISTEN_ADDR, port))
            srv.listen(4)
        except OSError as e:
            print(f"  listen failed: {e}")
            self.push_ctrl(S2C_ERR, bytes([ch, 1]), gen)
            return
        # The app can have given up during bind(). The close would have found
        # no listener to release, so registering one now would hold the port
        # until the daemon is restarted — and the next run of the server would
        # fail to bind for no visible reason.
        if self.stale(ch, cgen):
            print(f"  ch{ch} was closed before the port was bound")
            try:
                srv.close()
            except OSError:
                pass
            return
        self.listeners[ch] = srv
        print(f"  listening ch{ch} on {LISTEN_ADDR}:{port} "
              f"(reach it at {local_ip()}:{port})")

        while True:
            try:
                conn, peer = srv.accept()
            except OSError:
                break
            if gen != self.gen or ch not in self.listeners:
                try:
                    conn.close()
                except OSError:
                    pass
                break
            print(f"  ch{ch} accepted {peer[0]}:{peer[1]}")
            conn.settimeout(None)
            self.socks[ch] = conn
            self.push_ctrl(S2C_OPEN, struct.pack("<BHB", ch, 200, 0), gen)
            # Blocks here for the life of the connection: one at a time is the
            # contract, and accepting more would need channels that do not exist.
            self.pump(ch, conn, gen)
            # One at a time: do not accept another until the app has finished
            # with this one, or two connections would share a channel.
            waited = 0.0
            while ch in self.socks and waited < 60.0 and gen == self.gen:
                time.sleep(0.02)
                waited += 0.02
            if ch in self.socks:
                print(f"  ch{ch} timed out waiting for the app to hang up")
                self.drop(ch, gen)
        try:
            srv.close()
        except OSError:
            pass
        self.listeners.pop(ch, None)

    # ------------------------------------------------------------- fetching
    def fetch(self, ch, url, as_image=False, gen=None, cgen=None):
        """Wrapper whose only job is that this thread cannot die in silence.

        An unhandled exception here used to kill the thread and nothing else:
        no reply, no error, no log beyond a traceback nobody was watching, and
        a SNES waiting for a status that was never coming. That is
        indistinguishable from a hung network, and it is exactly the shape of
        the intermittent timeout that took two sessions to chase. A crash is now
        an error the console can print."""
        try:
            self._fetch(ch, url, as_image, gen, cgen)
        except Exception as e:
            print(f"  fetch crashed: {type(e).__name__}: {e}")
            traceback.print_exc()
            if not self.stale(ch, cgen):
                self.push_ctrl(S2C_ERR, bytes([ch, 3]), gen)

    def _fetch(self, ch, url, as_image=False, gen=None, cgen=None):
        """Runs on its own thread: a blocking fetch must not stall the link, or
        the SNES times out waiting for a status it was always going to get.

        Each stage is logged with the time it took. A fetch that never answers
        has been reported and could not be reproduced here, and the log at the
        time showed only "FETCH" followed by "CLOSE" with nothing between —
        which says the thread produced no output but not WHERE it stopped.
        Silence is the same shape for "never started", "stuck opening the
        connection", "stuck reading the body" and "stuck converting", and those
        want completely different fixes."""
        t0 = time.time()

        def mark(what):
            print(f"  [{time.time() - t0:5.1f}s] {what}")

        mark("thread started")

        if "://" not in url:
            url = "https://" + url
        ctype = ""
        try:
            req = urllib.request.Request(
                url,
                headers={"User-Agent": USER_AGENT, "Accept-Encoding": "gzip"},
            )
            with urllib.request.urlopen(req, timeout=20) as resp:
                mark("connected")
                status = resp.status
                ctype = resp.headers.get("Content-Type", "")
                # Decide the cap from what came back, before reading it: an
                # image may be large because it is about to be shrunk, while
                # anything passed through verbatim has to cross a 600 B/s link.
                is_img = ctype.split(";")[0].strip().lower().startswith("image/")
                cap = MAX_IMAGE_SOURCE if (as_image or is_img) else MAX_BODY
                # One byte past the cap, so "exactly the cap" and "too big" are
                # distinguishable and truncation is never silent.
                body = resp.read(cap + 1)
                if len(body) > cap:
                    body = body[:cap]
                    truncated = True
                else:
                    truncated = False
                if resp.headers.get("Content-Encoding", "") == "gzip":
                    # A truncated gzip stream cannot be decoded, and a decode
                    # failure must not fall through to forwarding compressed
                    # bytes as if they were the page — that was a bare
                    # `except: pass`, and it delivered binary garbage as a 200
                    # with no log line saying why.
                    if truncated:
                        print(f"  gzip body exceeded the cap; cannot decode a "
                              f"truncated stream")
                        self.push_ctrl(S2C_ERR, bytes([ch, 3]), gen)
                        return
                    try:
                        body = gzip.decompress(body)
                    except Exception as e:
                        print(f"  gzip decode failed: {e}")
                        self.push_ctrl(S2C_ERR, bytes([ch, 3]), gen)
                        return
        except urllib.error.HTTPError as e:
            status = e.code
            ctype = e.headers.get("Content-Type", "") if e.headers else ""
            body = e.read(MAX_BODY) or b""
            truncated = False
            # Error pages are gzip-encoded like any other response; the old
            # path never decoded them, so a 404's body printed as garbage.
            if e.headers and e.headers.get("Content-Encoding", "") == "gzip":
                try:
                    body = gzip.decompress(body)
                except Exception:
                    body = b""
        except Exception as e:
            print(f"  fetch failed: {e}")
            self.push_ctrl(S2C_ERR, bytes([ch, 1]), gen)
            return

        # Auto-convert on Content-Type. The gateway is the only party that knows
        # what came back, and it is the only party that could decode it anyway —
        # so making the caller declare "this is an image" in advance was asking
        # the user to supply information the system already had. One `get` now
        # handles everything and the separate `image` command goes away.
        if not as_image and status == 200 and ctype.split(";")[0].strip().lower().startswith("image/"):
            as_image = True
            print(f"  {ctype} -> converting")

        mark(f"body read: {len(body)} bytes, {ctype or 'no content-type'}")
        if truncated:
            # Say so rather than handing on a damaged body. Silently truncating
            # is how a 960 KB PNG turned into "image file is truncated", a
            # message that blames the image for the gateway's cap.
            print(f"  body exceeded {len(body)} bytes and was truncated")

        if as_image and status == 200:
            try:
                before = len(body)
                body = to_img(body)
                print(f"  converted {before} -> {len(body)} bytes of Mode 3 tiles")
            except Exception as e:
                # A 200 that will not decode is a distinct failure from a 404,
                # and the console can do nothing useful with the raw bytes.
                print(f"  image conversion failed: {e}")
                self.push_ctrl(S2C_ERR, bytes([ch, 2]), gen)
                return

        # The flags byte tells the SNES the body is Mode 3 tile data rather than
        # whatever was requested. It needs to know: the file has to be saved as
        # .IMG for `view` to find it, and printing tile bytes to the console
        # would be noise. Older three-byte S2C_OPEN frames still parse — the
        # kernel treats a missing flags byte as zero — so the canned frames in
        # test/net.lua did not all have to change at once.
        # file:// and other non-HTTP schemes have no numeric status. Reaching
        # the bytes IS the success condition there, so call it 200 rather than
        # letting struct.pack refuse None and take the thread down with it.
        if not isinstance(status, int):
            status = 200
        flags = 1 if (as_image and status == 200) else 0
        mark("replying")
        print(f"  -> {status}, {len(body)} bytes{' [IMG]' if flags else ''}")
        self.reply(ch, status, flags, body, gen, cgen)

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
        # Every worker is handed the channel's generation as it stands NOW,
        # which is the moment the request was accepted. Reading it inside the
        # thread would be reading it after a close that has already happened,
        # and the whole point is to notice that close.
        cgen = self.chgen.get(self.cbuf[0], 0) if self.cbuf else 0
        if self.ctype in (C2S_FETCH, C2S_IMAGE) and len(self.cbuf) >= 1:
            ch = self.cbuf[0]
            url = self.cbuf[1:].decode("ascii", "replace")
            want_img = self.ctype == C2S_IMAGE
            print(f"{'IMAGE' if want_img else 'FETCH'} ch{ch} {url}")
            threading.Thread(target=self.fetch,
                             args=(ch, url, want_img, self.gen, cgen),
                             daemon=True).start()
        elif self.ctype == C2S_HELLO:
            # Answered from the frame thread, so a fetch blocked on the network
            # cannot make the daemon look dead — and now jumped ahead of queued
            # bulk data too, because a liveness probe that waits eighty seconds
            # behind a download reports the opposite of the truth. See
            # push_hello() for why only this message may overtake anything.
            self.push_hello(bytes([GW_VERSION]) + local_ip().encode())
        elif self.ctype == C2S_CONNECT and len(self.cbuf) >= 1:
            ch = self.cbuf[0]
            hostport = bytes(self.cbuf[1:]).decode("ascii", "replace")
            print(f"CONNECT ch{ch} {hostport}")
            threading.Thread(target=self.do_connect,
                             args=(ch, hostport, self.gen, cgen),
                             daemon=True).start()

        elif self.ctype == C2S_LISTEN and len(self.cbuf) >= 3:
            ch = self.cbuf[0]
            port = self.cbuf[1] | (self.cbuf[2] << 8)
            print(f"LISTEN ch{ch} port {port}")
            threading.Thread(target=self.do_listen,
                             args=(ch, port, self.gen, cgen),
                             daemon=True).start()

        elif self.ctype == C2S_HANGUP and len(self.cbuf) >= 1:
            # End this connection, keep the port. Everything the SNES sent
            # before this record has already been written to the socket —
            # records are processed in order — so closing now cannot truncate.
            ch = self.cbuf[0]
            sock = self.socks.pop(ch, None)
            if sock is not None:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    sock.close()
                except OSError:
                    pass
                print(f"  ch{ch} hung up by the app")
            # The app has finished with this connection and stopped reading it,
            # so anything of it still queued is bytes nobody will collect,
            # sitting in front of the next request's.
            self.released(ch)
            self.forget(ch)

        elif self.ctype == C2S_CLOSE and len(self.cbuf) >= 1:
            ch = self.cbuf[0]
            print(f"CLOSE ch{ch}")
            # Before anything else: every worker already running for this
            # channel is now working for nobody, including the ones that have
            # not claimed a socket or a page yet and would otherwise sail past
            # the teardown below.
            self.released(ch)
            sock = self.socks.pop(ch, None)
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
            srv = self.listeners.pop(ch, None)
            if srv is not None:
                # Release the port, or the next run of the app cannot bind it.
                try:
                    srv.close()
                except OSError:
                    pass
            # Everything still queued for this channel is now undeliverable.
            # Keeping it starves the link for a minute and hands the next user
            # of the channel — channels are reused — the tail of a dead page.
            self.forget(ch)

    def deliver(self, f):
        o = HDR
        while o + 2 <= FRAME:
            ch, ln = f[o], f[o + 1]
            if ch == CH_PAD or o + 2 + ln > FRAME:
                break
            if ch == CH_CTRL:
                for i in range(ln):
                    self.ctrl_byte(f[o + 2 + i])
            else:
                # Stream bytes from the SNES. Until connect existed this branch
                # had nothing to do — every byte the console sent was a control
                # record — which is why it could only ever ask for things.
                sock = self.socks.get(ch)
                if sock is not None:
                    try:
                        sock.sendall(bytes(f[o + 2:o + 2 + ln]))
                    except OSError:
                        self.drop(ch)
            o += 2 + ln

    # -------------------------------------------------------- frame builder
    def build(self):
        f = bytearray(FRAME)
        f[SEQ] = self.tx_seq
        f[ACK] = (self.rx_next - 1) & 0xFF
        o, any_data = HDR, False
        with self.lock:
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
        # sequence once per episode and drop everything the old session owned.
        # Once per episode, not once per frame — honouring it on every frame
        # would re-deliver retransmissions, since the seq check is what
        # discards those.
        if f[FLAGS] & F_SYN:
            if not self.peer_syn_seen:
                self.peer_syn_seen = True
                self.rx_next = f[SEQ]
                with self.lock:
                    self.gen += 1
                # Not just the queue: the sockets those channels named died
                # with the console's session, and a reused channel number must
                # never reach an old connection. See teardown().
                self.teardown()
                self.cstate, self.ctype, self.cbuf = 0, 0, bytearray()
                print("gateway: SNES restarted, session reset")
        else:
            self.peer_syn_seen = False

        if (f[FLAGS] & F_DATA) and f[SEQ] == self.rx_next:
            self.deliver(f)
            self.rx_next = (self.rx_next + 1) & 0xFF

    def next_out(self):
        if not self.awaiting:
            with self.lock:
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
    # SIGUSR1 dumps every thread's stack: the only way to see WHERE a wedged
    # daemon is wedged without guessing.
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

    print(f"gateway: reading {UP}, writing {DOWN}")
    print("gateway: load tools/netbridge.lua in Mesen's Script Window")
    stats = os.environ.get("SNOS_GW_STATS")
    stats_at = time.time() + 5.0
    try:
        while True:
            if stats and time.time() >= stats_at:
                stats_at = time.time() + 5.0
                print(gw.stats_line())
            moved = False
            try:
                # The bridge truncates both files when it starts, so a restarted
                # emulator leaves this loop reading from an offset past the end
                # of a brand new file. It then picks up mid-stream once the file
                # grows back past that point, which desynchronises the protocol
                # and presents as "timeout waiting for the gateway" — with the
                # gateway right there, running, looking healthy.
                #
                # You restart the emulator far more often than this daemon, so
                # this is the common case, not an edge one.
                if os.path.getsize(UP) < up_off:
                    print("gateway: link restarted, resetting")
                    up_off = 0
                    # Tear the old session down BEFORE abandoning the object:
                    # its accept threads test gen against the object they were
                    # born on, so replacing gw alone left them running with
                    # the port still bound.
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
            if moved:
                down.write(bytes(gw.next_out()))
            else:
                time.sleep(0.004)       # ~1/4 of a video frame
    except KeyboardInterrupt:
        # Daemon threads die with the process, but sockets and bound ports do
        # not deserve to be left to the OS's timing.
        gw.teardown()
        print("\ngateway: stopped")


if __name__ == "__main__":
    main()
