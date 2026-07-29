#!/usr/bin/env python3
"""SNOS gateway: the far side of the link, doing everything the SNES cannot.

UNVERIFIED. The protocol itself is covered hermetically by snes/test/net.lua,
which speaks it frame for frame; this program is the path to the real internet
and cannot be exercised headlessly. Treat the first run as bring-up.

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

    It listens on no socket. Traffic arrives through a file the bridge writes,
    which means it cannot accidentally become an open proxy on the network.
    Worth keeping that way.
"""
import gzip
import io
import os
import socket
import struct
import traceback
import sys
import threading
import time
import urllib.error
import urllib.request

UP = "/tmp/snos-up.bin"       # SNES -> gateway
DOWN = "/tmp/snos-down.bin"   # gateway -> SNES

FRAME = 64
HDR = 4
SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
F_DATA = 0x01
CH_CTRL, CH_PAD = 0, 0xFF

C2S_FETCH, C2S_CLOSE, C2S_IMAGE, C2S_HELLO = 0x01, 0x02, 0x03, 0x04
C2S_RESOLVE, C2S_PING = 0x05, 0x06
S2C_OPEN, S2C_EOF, S2C_ERR, S2C_HELLO = 0x81, 0x82, 0x83, 0x84
S2C_RESOLVE, S2C_PING = 0x85, 0x86

# Standard ping payload. Not the MTU: 56 bytes is what ping(8) sends, and
# filling the MTU is a fragmentation test that fails for reasons having nothing
# to do with whether the host is reachable.
PING_PAYLOAD = 56
PING_TIMEOUT = 2.0
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


SCREEN_W, SCREEN_H = 256, 224
IMG_MAGIC = b"SIMG"


def to_img(data):
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

    sw, sh = im.size
    scale = min(SCREEN_W / sw, SCREEN_H / sh)
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


def resolve(name):
    """A records only. AAAA is deliberately excluded: an IPv6 address does not
    fit the 64-column console in any useful way, and nothing downstream of here
    can open one anyway."""
    try:
        infos = socket.getaddrinfo(name, None, socket.AF_INET)
    except socket.gaierror:
        return []
    seen, out = set(), []
    for info in infos:
        ip = info[4][0]
        if ip not in seen:
            seen.add(ip)
            out.append(ip)
    return out


def icmp_checksum(b):
    """The one's-complement sum RFC 792 asks for. macOS will not fill this in
    for us the way Linux does on an unprivileged ICMP socket — send a zero
    checksum here and every echo is silently discarded, which looks exactly like
    an unreachable host."""
    if len(b) % 2:
        b += b"\x00"
    total = 0
    for i in range(0, len(b), 2):
        total += (b[i] << 8) + b[i + 1]
    total = (total >> 16) + (total & 0xFFFF)
    total += total >> 16
    return ~total & 0xFFFF


def ping(host, count=4):
    """Four real ICMP echoes, sent together, collected against one deadline.

    ping(8) spaces echoes a second apart because it is measuring a link over
    time. This is answering "is that host reachable", and sequential timeouts
    made the worst case count x timeout — eight seconds of a console sitting
    there with nothing to show, and long enough that the SNES gave up on the
    gateway before the gateway gave up on the host. Sent together the worst case
    is one timeout, and a lost echo is still a lost echo.

    Unprivileged ICMP datagram sockets work on macOS and on Linux with
    ping_group_range set, so this needs no root and no setuid helper.

    Note what this measures: the GATEWAY's reachability of the target, not the
    SNES's. The SNES has no IP stack — that is the entire premise — so there is
    no other reading available, and it is the one you want when asking whether
    the far side is on the network."""
    try:
        addr = socket.getaddrinfo(host, None, socket.AF_INET)[0][4][0]
    except (socket.gaierror, IndexError):
        return None, []

    results = [None] * count
    ident = os.getpid() & 0xFFFF
    sock = None
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,
                             socket.IPPROTO_ICMP)
        payload = b"\x42" * PING_PAYLOAD
        sent = {}
        for seq in range(1, count + 1):
            head = struct.pack("!BBHHH", 8, 0, 0, ident, seq)
            csum = icmp_checksum(head + payload)
            pkt = struct.pack("!BBHHH", 8, 0, csum, ident, seq) + payload
            sent[seq] = time.time()
            try:
                sock.sendto(pkt, (addr, 0))
            except OSError:
                sent.pop(seq, None)     # no route: that echo is simply lost

        deadline = time.time() + PING_TIMEOUT
        while any(r is None for r in results):
            left = deadline - time.time()
            if left <= 0:
                break
            sock.settimeout(left)
            try:
                data, _ = sock.recvfrom(1024)
            except (socket.timeout, OSError):
                break
            # macOS hands back the whole IP packet on this socket type, Linux
            # hands back the ICMP message alone. Skip an IP header if present
            # rather than assuming either.
            off = 0
            if len(data) >= 20 and (data[0] >> 4) == 4:
                off = (data[0] & 0x0F) * 4
            if len(data) < off + 8 or data[off] != 0:       # not an echo reply
                continue
            seq = struct.unpack("!H", data[off + 6:off + 8])[0]
            if seq in sent and 1 <= seq <= count:
                results[seq - 1] = int((time.time() - sent[seq]) * 1000)
    except OSError:
        pass
    finally:
        if sock is not None:
            sock.close()
    return addr, results


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
        self.inflight = None
        self.out = []            # [ch, bytearray] queued for the SNES
        self.lock = threading.Lock()
        self.cstate = 0
        self.ctype = 0
        self.clen = 0
        self.cbuf = bytearray()

    # ------------------------------------------------------------ outbound
    def push(self, ch, data, gen=None):
        with self.lock:
            if gen is not None and gen != self.gen:
                return          # a fetch from a session that no longer exists
            self.out.append([ch, bytearray(data)])

    def push_ctrl(self, typ, payload, gen=None):
        self.push(CH_CTRL, bytes([typ, len(payload)]) + payload, gen)

    def do_ping(self, host, gen):
        addr, results = ping(host)
        if addr is None:
            self.push_ctrl(S2C_PING, b"\x00", gen)      # could not resolve
            print("  -> name did not resolve")
            return
        got = sum(1 for r in results if r is not None)
        avg = [r for r in results if r is not None]
        print(f"  -> {got}/{len(results)} replied"
              + (f", avg {sum(avg) // len(avg)} ms" if avg else ""))
        # [1][count][one byte per echo: 0 = lost, else min(rtt_ms, 254)]
        body = bytes([1, len(results)]) + bytes(
            0 if r is None else min(r, 254) for r in results)
        self.push_ctrl(S2C_PING, body, gen)

    # ------------------------------------------------------------- fetching
    def fetch(self, ch, url, as_image=False, gen=None):
        """Wrapper whose only job is that this thread cannot die in silence.

        An unhandled exception here used to kill the thread and nothing else:
        no reply, no error, no log beyond a traceback nobody was watching, and
        a SNES waiting for a status that was never coming. That is
        indistinguishable from a hung network, and it is exactly the shape of
        the intermittent timeout that took two sessions to chase. A crash is now
        an error the console can print."""
        try:
            self._fetch(ch, url, as_image, gen)
        except Exception as e:
            print(f"  fetch crashed: {type(e).__name__}: {e}")
            traceback.print_exc()
            self.push_ctrl(S2C_ERR, bytes([ch, 3]), gen)

    def _fetch(self, ch, url, as_image=False, gen=None):
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
                    try:
                        body = gzip.decompress(body)
                    except Exception:
                        pass
        except urllib.error.HTTPError as e:
            status = e.code
            ctype = e.headers.get("Content-Type", "") if e.headers else ""
            body = e.read(MAX_BODY) or b""
            truncated = False
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
        self.push_ctrl(S2C_OPEN, struct.pack("<BHB", ch, status, flags), gen)
        # Chunked so one huge stream cannot monopolise the frame builder.
        for i in range(0, len(body), 512):
            self.push(ch, body[i:i + 512], gen)
        self.push_ctrl(S2C_EOF, bytes([ch]), gen)

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
        if self.ctype in (C2S_FETCH, C2S_IMAGE) and len(self.cbuf) >= 1:
            ch = self.cbuf[0]
            url = self.cbuf[1:].decode("ascii", "replace")
            want_img = self.ctype == C2S_IMAGE
            print(f"{'IMAGE' if want_img else 'FETCH'} ch{ch} {url}")
            threading.Thread(target=self.fetch,
                             args=(ch, url, want_img, self.gen),
                             daemon=True).start()
        elif self.ctype == C2S_HELLO:
            # Answered from the frame thread, so a fetch blocked on the network
            # cannot make the daemon look dead. It is not jumped to the front of
            # the queue, though: control shares the send queue with bodies, and
            # S2C_EOF has to stay behind the bytes it terminates. So a HELLO
            # issued mid-download waits for the download. That is fine for what
            # this is for — the shell is idle when you type `net status` — but
            # it is a probe of the daemon, not of the queue's depth.
            self.push_ctrl(S2C_HELLO,
                           bytes([GW_VERSION]) + local_ip().encode())
        elif self.ctype == C2S_RESOLVE and len(self.cbuf) >= 1:
            name = bytes(self.cbuf).decode("ascii", "replace")
            print(f"RESOLVE {name}")
            ips = resolve(name)
            # Text, not packed octets. The console is the only consumer and it
            # would only have to format them back; when something needs binary
            # addresses it will be an app, and it can have its own record.
            body = " ".join(ips[:4]).encode()
            if len(ips) > 4:
                body += f" +{len(ips) - 4}".encode()
            print(f"  -> {body.decode() if ips else 'not found'}")
            self.push_ctrl(S2C_RESOLVE, body)

        elif self.ctype == C2S_PING and len(self.cbuf) >= 1:
            host = bytes(self.cbuf).decode("ascii", "replace")
            print(f"PING {host}")
            # On its own thread: even bounded at one timeout this blocks for
            # up to two seconds, and the link must keep running or the SNES
            # decides the gateway died while it is busy proving something else
            # did — which is exactly what happened when echoes were sequential.
            threading.Thread(target=self.do_ping, args=(host, self.gen),
                             daemon=True).start()

        elif self.ctype == C2S_CLOSE and len(self.cbuf) >= 1:
            print(f"CLOSE ch{self.cbuf[0]}")

    def deliver(self, f):
        o = HDR
        while o + 2 <= FRAME:
            ch, ln = f[o], f[o + 1]
            if ch == CH_PAD or o + 2 + ln > FRAME:
                break
            if ch == CH_CTRL:
                for i in range(ln):
                    self.ctrl_byte(f[o + 2 + i])
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
                if not data:
                    self.out.pop(0)
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
                    self.out.clear()
                self.cstate, self.ctype, self.cbuf = 0, 0, bytearray()
                print("gateway: SNES restarted, session reset")
        else:
            self.peer_syn_seen = False

        if (f[FLAGS] & F_DATA) and f[SEQ] == self.rx_next:
            self.deliver(f)
            self.rx_next = (self.rx_next + 1) & 0xFF

    def next_out(self):
        if not self.awaiting:
            f, any_data = self.build()
            self.inflight = f
            if any_data:
                self.awaiting = True
            return f
        self.inflight[ACK] = (self.rx_next - 1) & 0xFF
        self.inflight[SUM] = 0
        self.inflight[SUM] = checksum(self.inflight)
        return self.inflight


def main():
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
    try:
        while True:
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
        print("\ngateway: stopped")


if __name__ == "__main__":
    main()
