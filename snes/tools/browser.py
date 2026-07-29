"""A browser the SNES can drive: navigate, scroll, click, screenshot.

This is the far half of `web`. The console cannot lay out HTML — Wikipedia's
SNES article is 735 KB against a 64 KB app arena, which is not a tuning problem
— but it is genuinely good at showing a bitmap, so the split is the same one
that makes TLS and JPEG decoding possible: the far side does what a 3.58 MHz
65816 cannot, and the SNES does what it is good at.

WHY NOT WRP
-----------
WRP (tenox7/wrp) is the obvious prior art and does very nearly this, but it
serves HTML with `<img ismap>` to a *browser* client and takes clicks back as
`?x,y` on a GET. The SNES is not a browser, so using WRP would mean the gateway
impersonating one — fetching its page, scraping out the image, POSTing
coordinates — and WRP's nastiest open bug lives in exactly that round trip. We
send x,y as a control record on our own link, so the whole failure class is
absent. Borrow the idea, not the binary.

WHY A HAND-WRITTEN WEBSOCKET
----------------------------
Driving Chrome means CDP, and CDP commands are websocket-only — the HTTP
endpoints can list and create targets but cannot evaluate or click. Python has
no websocket client in the standard library, and `gateway.py`'s one virtue as a
dependency is that it needs nothing but Pillow. A client-side RFC 6455
implementation for small JSON messages is about eighty lines, which is cheaper
than a dependency and much cheaper than being unable to click.
"""

import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request

# The SNES window, and the CSS viewport that feeds it.
#
# 24 rows and not 28: a full-screen Mode 3 image is 57 KB of 8bpp tiles, and the
# sprite tile base can only sit on a 16 KB boundary — every one of which lands
# inside an image that large. 192 lines leaves tiles at $0000-$BFFF, the cursor
# sprite at $C000 and the tilemap at $E000, with no overlap and no corrupted
# block on screen. v2's 4-colour format makes tiles 16 bytes instead of 64 and
# frees over 30 KB, so this is a v1 compromise rather than a permanent one.
SNES_W, SNES_H = 256, 192

# Rendered at exactly double and downscaled 2:1. Rendering at the console's own
# resolution turns body text into a grey smear; the supersampled version is
# legible, and 2:1 needs no aspect correction.
CSS_W, CSS_H = SNES_W * 2, SNES_H * 2

CHROME_PATHS = [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
]


def find_chrome():
    env = os.environ.get("SNOS_CHROME")
    if env:
        return env if os.path.exists(env) else None
    for path in CHROME_PATHS:
        if os.path.exists(path):
            return path
    return shutil.which("google-chrome") or shutil.which("chromium")


# ------------------------------------------------------------------ websocket

class WebSocket:
    """The client half of RFC 6455, for small text frames.

    Deliberately partial: no extensions, no continuation frames on send, and
    fragmented receives are reassembled but never expected. CDP replies to the
    handful of methods used here are single text frames.
    """

    def __init__(self, url, timeout=30):
        # ws://host:port/path
        rest = url.split("://", 1)[1]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.sock = socket.create_connection((host, int(port or 80)), timeout=10)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET /{path} HTTP/1.1\r\n"
            f"Host: {hostport}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        # Read just the handshake response; anything after it is frame data and
        # must not be consumed here.
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(1)
            if not chunk:
                raise RuntimeError("websocket handshake closed early")
            buf += chunk
        if b"101" not in buf.split(b"\r\n")[0]:
            raise RuntimeError(f"websocket upgrade refused: {buf.splitlines()[0]!r}")
        self._rx = b""

    def _read(self, n):
        while len(self._rx) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("websocket closed")
            self._rx += chunk
        out, self._rx = self._rx[:n], self._rx[n:]
        return out

    def send(self, text):
        payload = text.encode()
        # Clients MUST mask. Servers reject unmasked frames outright.
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        if n < 126:
            hdr = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        self.sock.sendall(hdr + mask + masked)

    def recv(self):
        while True:
            b0, b1 = self._read(2)
            opcode = b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._read(8))[0]
            if b1 & 0x80:                      # a server frame should not be masked
                self._read(4)
            data = self._read(n) if n else b""
            if opcode == 0x8:
                raise RuntimeError("websocket closed by peer")
            if opcode == 0x9:                  # ping -> pong, keep the link alive
                self.sock.sendall(struct.pack("!BB", 0x8A, 0x80) + os.urandom(4))
                continue
            if opcode in (0x1, 0x2, 0x0):
                return data.decode("utf-8", "replace")
            # 0xA pong and anything else: ignore

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------- session

class Session:
    """One Chrome page, held open for the life of a SNES browsing session.

    Held open rather than one-shot per request, and not only for speed: clicking
    needs the same page instance that produced the screenshot, and scrolling
    needs the page's own scroll position. A fresh Chrome per action could screen-
    shot but never interact.

    THREADING. Held open means held open ACROSS threads: the gateway drives a
    page from a worker while the frame loop can decide at any moment that the
    console has hung up. So a session carries its own lock and a `dead` flag —
    `busy` makes close() wait for whatever CDP call is in flight instead of
    pulling the socket out from under it, and `dead` stops the next call from
    starting. Without the pair, closing mid-screenshot killed the worker with
    `OSError: [Errno 9] Bad file descriptor` and the SNES waited for a page that
    was never coming.
    """

    def __init__(self):
        # Before anything that can raise: close() must be safe on a session
        # whose constructor gave up half way.
        self.busy = threading.RLock()
        self.dead = False
        self.closed = False
        self.ws = None
        self.proc = None
        self.profile = None

        chrome = find_chrome()
        if chrome is None:
            raise RuntimeError("no renderer configured (set SNOS_CHROME)")

        # Ask the OS for a free port rather than guessing one. Chrome writes the
        # port it actually took to DevToolsActivePort, but reading that races
        # against startup; picking it ourselves is deterministic.
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()

        self.profile = tempfile.mkdtemp(prefix="snos-web-")
        self.proc = subprocess.Popen(
            [
                chrome, "--headless", "--no-sandbox", "--disable-gpu",
                f"--remote-debugging-port={self.port}",
                f"--user-data-dir={self.profile}",
                f"--window-size={CSS_W},{CSS_H}",
                "--hide-scrollbars", "--no-first-run",
                "--no-default-browser-check", "--disable-extensions",
                "about:blank",
            ],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )

        self.next_id = 1
        self.history = []          # visited URLs, for back/forward
        self.hpos = -1
        # From here on Chrome is RUNNING, so every exit has to go through
        # close(). Raising straight out used to leak a headless browser and its
        # profile directory for the life of the daemon, once per failed open.
        try:
            deadline = time.time() + 30
            last = None
            while time.time() < deadline:
                try:
                    targets = json.loads(self._http("/json/list"))
                    pages = [t for t in targets if t.get("type") == "page"
                             and t.get("webSocketDebuggerUrl")]
                    if pages:
                        self.ws = WebSocket(pages[0]["webSocketDebuggerUrl"])
                        break
                except Exception as e:
                    last = e
                if self.proc.poll() is not None:
                    raise RuntimeError("chrome exited before it was reachable")
                time.sleep(0.2)
            if self.ws is None:
                raise RuntimeError(f"chrome never became reachable: {last}")

            self.cmd("Page.enable")
            self.cmd("Runtime.enable")
            # --window-size sets the OS window, which is NOT the viewport:
            # Chrome handed back 512x241 when asked for 512x384. Override the
            # metrics so the screenshot is exactly the size the console expects,
            # or to_img letterboxes a short image into a tall window and the
            # page arrives squashed into the middle of the screen.
            self.cmd("Emulation.setDeviceMetricsOverride",
                     {"width": CSS_W, "height": CSS_H,
                      "deviceScaleFactor": 1, "mobile": False})
        except BaseException:
            self.close()
            raise

    def _http(self, path):
        with urllib.request.urlopen(
                f"http://127.0.0.1:{self.port}{path}", timeout=5) as r:
            return r.read().decode()

    def cmd(self, method, params=None, timeout=30):
        """One CDP call. Events arrive interleaved with replies and are skipped
        by id, which is why this reads in a loop instead of taking the next
        frame as the answer."""
        mid = self.next_id
        self.next_id += 1
        self.ws.send(json.dumps({"id": mid, "method": method,
                                 "params": params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = json.loads(self.ws.recv())
            if msg.get("id") != mid:
                continue                       # an event, or another reply
            if "error" in msg:
                raise RuntimeError(f"{method}: {msg['error'].get('message')}")
            return msg.get("result", {})
        raise RuntimeError(f"{method}: no reply within {timeout}s")

    def eval(self, expr):
        r = self.cmd("Runtime.evaluate",
                     {"expression": expr, "returnByValue": True})
        return r.get("result", {}).get("value")

    # ------------------------------------------------------------ navigation

    def _settle(self, budget=8.0):
        """Wait for the page to stop being obviously unfinished.

        Polling document.readyState rather than waiting on Page.loadEventFired:
        a page that never fires load (an ad frame that hangs, a socket that
        stays open) would otherwise block forever, and a screenshot of a
        mostly-loaded page is a far better answer than no screenshot."""
        deadline = time.time() + budget
        while time.time() < deadline:
            try:
                if self.eval("document.readyState") == "complete":
                    break
            except Exception:
                pass
            time.sleep(0.15)
        time.sleep(0.3)                        # let webfonts and layout land

    def navigate(self, url, record=True):
        if "://" not in url:
            url = "https://" + url
        self.cmd("Page.navigate", {"url": url})
        self._settle()
        if record:
            # Truncate any forward history, exactly like a browser.
            del self.history[self.hpos + 1:]
            self.history.append(url)
            self.hpos = len(self.history) - 1
        return url

    def back(self):
        if self.hpos <= 0:
            return False
        self.hpos -= 1
        self.navigate(self.history[self.hpos], record=False)
        return True

    def forward(self):
        if self.hpos + 1 >= len(self.history):
            return False
        self.hpos += 1
        self.navigate(self.history[self.hpos], record=False)
        return True

    def scroll(self, screens):
        """Scroll by whole windows. Page-quantised on purpose: every move costs
        a fresh image across a 615 B/s link, so a pixel-level scroll would be
        the most expensive gesture in the app."""
        before = self.eval("window.scrollY") or 0
        self.eval(f"window.scrollBy(0, {int(screens * CSS_H)})")
        time.sleep(0.2)
        after = self.eval("window.scrollY") or 0
        return after != before

    def click(self, x, y):
        """Click at a SNES pixel. Returns the URL followed, or None.

        Coordinates are doubled because the console sees a 2:1 downscale of the
        CSS viewport, and they are viewport-relative — which is what
        elementFromPoint wants, so the page's own scroll position needs no
        correction here.

        Links are resolved and navigated rather than synthesising a mouse event:
        a real click can land on an overlay, open a new tab, or run script that
        goes nowhere, and none of those have anywhere to go on a SNES. A click
        on something that is not a link does nothing, which is the honest
        outcome when the console cannot show what it hit."""
        expr = (
            "(function(){var e=document.elementFromPoint(%d,%d);"
            "if(!e)return null;var a=e.closest('a');"
            "return a&&a.href?a.href:null;})()" % (x * 2, y * 2)
        )
        href = self.eval(expr)
        if not href or href.startswith("javascript:"):
            return None
        self.navigate(href)
        return href

    def shot(self):
        """PNG of the current viewport, at CSS_W x CSS_H."""
        r = self.cmd("Page.captureScreenshot", {"format": "png"})
        return base64.b64decode(r["data"])

    def close(self):
        """Shut the browser down. Idempotent, and safe to call while another
        thread is mid-operation.

        It was neither, and that was a use-after-close: a C2S_CLOSE arriving on
        the gateway's frame loop closed this websocket while a worker thread was
        inside Page.captureScreenshot, and the worker died on
        `OSError: [Errno 9] Bad file descriptor`. What the console saw was a page
        that never arrived and `[web exited 0]` with no explanation, because the
        thread that owed it an answer was gone.

        `dead` first and outside the lock, so an operation that has not started
        yet gives up immediately rather than waiting for a session that is going
        away. Then the lock, which is what makes this WAIT for a call already in
        flight instead of cutting it off. Callers who cannot afford to wait —
        the frame loop — hand this to a thread; see Gateway.retire()."""
        self.dead = True
        with self.busy:
            if self.closed:
                return
            self.closed = True
            if self.ws is not None:
                self.ws.close()
            if self.proc is not None and self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
            if self.profile is not None:
                shutil.rmtree(self.profile, ignore_errors=True)
