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
import faulthandler
import signal
import socket
import struct

import browser
import subprocess
import tempfile
import traceback
import sys
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
C2S_WEB = 0x0A

# `web` operations, in the first payload byte after the channel.
WEB_OPEN, WEB_SCROLL, WEB_CLICK, WEB_BACK, WEB_FORWARD = 0, 1, 2, 3, 4

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

    # `web` asks for a shorter window than a photograph gets: a full-screen
    # 8bpp image leaves no 16 KB-aligned home for the cursor's sprite tiles, so
    # the browser gives up 32 scanlines to get a cursor. See browser.py.
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


# ---------------------------------------------------------------- rendering
#
# `render:URL` hands the SNES a PICTURE of a web page instead of its bytes.
#
# The console cannot lay out HTML — 735 KB of Wikipedia against a 64 KB app
# arena is not a tuning problem — but it is genuinely good at showing a 256
# colour bitmap, and the image path to do that already exists for photographs.
# So the browser is the same trick as TLS and JPEG: the far side does the part
# the 65816 cannot, and the SNES does the part it is good at.
#
# 512x448 and not 256x224. Rendering at the console's own resolution produces a
# grey smear where body text should be; rendering at double and letting to_img
# downscale 2:1 keeps the greyscale antialiasing that makes 4-pixel-wide glyphs
# readable. 512x448 is exactly twice 256x224, so the Lanczos step is a clean 2:1
# with no aspect correction. This is supersampling, and it is the difference
# between "you can tell those are words" and "you can read them".
RENDER_W, RENDER_H = 512, 448
RENDER_TIMEOUT = 30

CHROME_PATHS = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
]


def find_chrome():
    """Chrome is OPTIONAL. Everything else in this daemon runs on the standard
    library plus Pillow, and a headless browser is a different weight class of
    dependency — so its absence has to be a clean message rather than a
    traceback in a thread nobody is watching."""
    env = os.environ.get("SNOS_CHROME")
    if env:
        return env if os.path.exists(env) else None
    for path in CHROME_PATHS:
        if os.path.exists(path):
            return path
    return None


def render_page(url, width=RENDER_W, height=RENDER_H):
    """Screenshot a page. Returns PNG bytes, or raises with a usable message.

    One invocation per page, no persistent session: this renders the current
    window and nothing else, which is all the SNES can show at once anyway.
    Following a link means rendering again — the page does not have to be held
    open between requests, and not holding it open is what keeps this a
    subprocess call rather than a browser-automation dependency.

    Two things learned the hard way, both worth keeping:

    A throwaway --user-data-dir is not optional. Without it Chrome uses the
    real profile, which means rendering pages with the user's live cookies —
    their logged-in session sent to whatever the SNES asked for — and fighting
    their own running browser for the profile lock.

    But with it, Chrome writes the PNG and then NEVER EXITS. So this waits for
    the file rather than for the process, and kills the process itself. Waiting
    on exit hangs forever; that is what it did the first time."""
    chrome = find_chrome()
    if chrome is None:
        raise RuntimeError("no renderer configured (set SNOS_CHROME to a "
                           "Chrome or Chromium binary)")
    if "://" not in url:
        url = "https://" + url

    with tempfile.TemporaryDirectory() as tmp:
        shot = os.path.join(tmp, "page.png")
        cmd = [
            chrome, "--headless", "--no-sandbox", "--disable-gpu",
            f"--screenshot={shot}",
            f"--window-size={width},{height}",
            "--hide-scrollbars",
            "--no-first-run", "--no-default-browser-check",
            f"--user-data-dir={os.path.join(tmp, 'profile')}",
            url,
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE)
        deadline = time.time() + RENDER_TIMEOUT
        size = -1
        try:
            while time.time() < deadline:
                if os.path.exists(shot):
                    now = os.path.getsize(shot)
                    # Stable across two polls means the write finished. Reading
                    # the instant it appears can catch a partial PNG.
                    if now > 0 and now == size:
                        break
                    size = now
                elif proc.poll() is not None:
                    break               # died without producing anything
                time.sleep(0.1)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()

        if not os.path.exists(shot) or os.path.getsize(shot) == 0:
            err = (proc.stderr.read() or b"").decode("utf-8", "replace")
            # Chrome logs two harmless task_policy_set complaints on macOS every
            # single run; they are not the failure and must not be reported as
            # one.
            lines = [l for l in err.splitlines()
                     if l.strip() and "task_policy_set" not in l]
            raise RuntimeError("render produced no image: "
                               + (lines[-1] if lines else "timed out"))
        with open(shot, "rb") as f:
            return f.read()


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
        self.pages = {}          # channel -> browser.Session
        # `pages` is written by the frame loop (a close arrives) and by every
        # web worker (a page opens), so it needs a lock of its own — separate
        # from the send queue's, because a thread holding one must never be
        # waiting on the other.
        self.plock = threading.Lock()
        # Per channel, bumped every time the SNES closes one. `gen` is the same
        # idea for the whole session; this is the same idea for one channel, and
        # it is needed because forgetting what is QUEUED does not stop a worker
        # that has not queued anything yet. A `web` cancelled while Chrome was
        # still starting up went on to register its page and push 49 KB into a
        # channel the console had already given back. Captured when the request
        # is accepted, checked before anything is claimed or sent.
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

        Without this, a `web` window keeps being transmitted long after the
        application that asked for it has gone: 49 KB, eighty seconds of a
        615 B/s link, spent on a page nobody will ever see. Three separate
        symptoms came out of that one omission, and channel REUSE is why —

          the next `web` is handed channel 1 again and receives the tail of the
          dead page, S2C_OPEN and all, before its own, so two images end up
          interleaved in one cache file and the picture is garbage;

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

    def retire(self, page):
        """Take a browser session down without stalling the caller.

        close() waits for whatever CDP call is in flight — that is the whole
        point of the session lock — and the C2S_CLOSE that asks for it arrives
        on the frame loop, which cannot block for thirty seconds without
        stopping the link dead. So the session is marked dead at once, which is
        what an in-flight operation checks, and the waiting happens on a thread
        nobody is depending on."""
        if page is None:
            return
        page.dead = True
        threading.Thread(target=page.close, daemon=True).start()

    def drop(self, ch):
        """Close a stream and tell the SNES, once. Both directions can discover
        the end independently — the socket erroring on send, the reader thread
        seeing EOF — and a channel closed twice would send a stray EOF that the
        next connection on that channel would receive as its own."""
        sock = self.socks.pop(ch, None)
        if sock is None:
            return
        try:
            sock.close()
        except OSError:
            pass
        self.push_ctrl(S2C_EOF, bytes([ch]))
        print(f"  ch{ch} closed")

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
        self.drop(ch)

    def do_web(self, ch, op, arg, gen, cgen=None):
        """Drive a held-open browser page and send back a picture of it.

        Every operation answers with a full window, because the console has no
        way to apply a partial update to a bitmap it does not model — it has the
        pixels and nothing else. That is what makes v2's tile cache the whole
        performance story: a window is ~49 KB today and most of it is tiles the
        SNES was already holding.

        Runs on its own thread, and the page it drives can be taken away from it
        at any moment by a C2S_CLOSE on the frame loop. So every call into the
        browser happens under the session's own lock, and the session is checked
        for having been retired both before the work and before the answer is
        queued. The alternative is what this used to do: close the websocket
        under a screenshot in progress and lose the thread to
        `OSError: [Errno 9] Bad file descriptor`."""
        page = None
        try:
            if op == WEB_OPEN:
                with self.plock:
                    old = self.pages.pop(ch, None)
                self.retire(old)
                url = arg.decode("ascii", "replace")
                print(f"WEB ch{ch} open {url}")
                fresh = browser.Session()
                # Starting Chrome takes a second or two, and a `web` abandoned
                # in that window closed a channel this had not claimed yet: the
                # close found no page to retire, and this then registered one
                # and pushed a whole window into a channel the console had
                # given back. Claim and check together, under the lock.
                with self.plock:
                    ours = self.chgen.get(ch, 0) == cgen if cgen is not None \
                        else True
                    if ours:
                        self.pages[ch] = fresh
                if not ours:
                    print(f"  ch{ch} was closed while the browser was starting")
                    self.retire(fresh)
                    return
                page = fresh
            else:
                with self.plock:
                    page = self.pages.get(ch)
                if page is None:
                    print(f"  ch{ch} has no page open")
                    self.push_ctrl(S2C_ERR, bytes([ch, 5]), gen)
                    return

            with page.busy:
                if page.dead:
                    print(f"  ch{ch} was closed before it could be driven")
                    return
                if op == WEB_OPEN:
                    page.navigate(url)
                elif op == WEB_SCROLL:
                    # One signed byte of screens: the console cannot afford a
                    # finer unit when every move is a fresh image.
                    d = arg[0] if arg else 1
                    if d > 127:
                        d -= 256
                    print(f"WEB ch{ch} scroll {d:+d}")
                    page.scroll(d)
                elif op == WEB_CLICK:
                    x, y = (arg[0], arg[1]) if len(arg) >= 2 else (0, 0)
                    href = page.click(x, y)
                    print(f"WEB ch{ch} click ({x},{y}) -> {href or 'nothing there'}")
                elif op == WEB_BACK:
                    print(f"WEB ch{ch} back: {page.back()}")
                elif op == WEB_FORWARD:
                    print(f"WEB ch{ch} forward: {page.forward()}")
                else:
                    self.push_ctrl(S2C_ERR, bytes([ch, 6]), gen)
                    return
                if page.dead:
                    print(f"  ch{ch} was closed while it was working")
                    return
                png = page.shot()

            # Outside the lock: converting 33 KB of PNG to tiles is the one
            # expensive thing here that does not touch the browser, and holding
            # a session hostage during it would make every close wait for it.
            body = to_img(png, browser.SNES_W, browser.SNES_H)
            print(f"  -> {len(png)} B png, {len(body)} B of tiles")
            with self.plock:
                gone = (self.pages.get(ch) is not page
                        or (cgen is not None and self.chgen.get(ch, 0) != cgen))
            if gone:
                # Closed, or already reopened onto something else, while this
                # window was being built. Queueing 49 KB now would be queueing
                # it for whoever holds the channel next.
                print(f"  ch{ch} no longer wants this window")
                return
            self.reply(ch, 200, 1, body, gen, cgen)
        except Exception as e:
            print(f"  web failed: {type(e).__name__}: {e}")
            traceback.print_exc()
            with self.plock:
                mine = (page is None or self.pages.get(ch) is page) and (
                    cgen is None or self.chgen.get(ch, 0) == cgen)
                orphan = self.pages.pop(ch, None) if mine else None
            self.retire(orphan)
            if mine:
                self.push_ctrl(S2C_ERR, bytes([ch, 4]), gen)
            else:
                # The channel moved on while this was failing. Reporting the
                # error now would abort whatever holds it instead — an error
                # from a dead request killing a live one is worse than silence.
                print(f"  ch{ch} has moved on; the failure is not reported")

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
                self.drop(ch)
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

        # A rendered page never touches the HTTP path below: Chrome does the
        # fetching, so there is no response to read and no Content-Type to
        # believe. It rejoins at the image conversion, which is the whole point
        # — the bytes that reach the SNES are the same Mode 3 tiles a photograph
        # would have produced, and `view` needs no idea any of this happened.
        if url.startswith("render:"):
            target = url[len("render:"):]
            print(f"  rendering {target} at {RENDER_W}x{RENDER_H}")
            try:
                body = render_page(target)
            except Exception as e:
                print(f"  render failed: {e}")
                self.push_ctrl(S2C_ERR, bytes([ch, 4]), gen)
                return
            mark(f"rendered: {len(body)} bytes of PNG")
            try:
                before = len(body)
                body = to_img(body)
                print(f"  converted {before} -> {len(body)} bytes of Mode 3 tiles")
            except Exception as e:
                print(f"  image conversion failed: {e}")
                self.push_ctrl(S2C_ERR, bytes([ch, 2]), gen)
                return
            mark("replying")
            print(f"  -> 200, {len(body)} bytes [IMG]")
            self.reply(ch, 200, 1, body, gen, cgen)
            return

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

        elif self.ctype == C2S_WEB and len(self.cbuf) >= 2:
            ch, op = self.cbuf[0], self.cbuf[1]
            arg = bytes(self.cbuf[2:])
            threading.Thread(target=self.do_web,
                             args=(ch, op, arg, self.gen, cgen),
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
            with self.plock:
                page = self.pages.pop(ch, None)
            # Marked dead here and closed on a thread: this is the frame loop,
            # and close() waits for any CDP call still in flight. Closing it
            # inline used to pull the websocket out from under a worker
            # mid-screenshot, killing the thread on a bad file descriptor and
            # leaving the console waiting for a window that no longer had
            # anyone to send it.
            self.retire(page)
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
                    self.out.clear()
                    self.out_started = False
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
