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
import os
import struct
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

C2S_FETCH, C2S_CLOSE = 0x01, 0x02
S2C_OPEN, S2C_EOF, S2C_ERR = 0x81, 0x82, 0x83

USER_AGENT = "SNOS/0.1 (Super Nintendo; via gateway)"
MAX_BODY = 512 * 1024


def checksum(f):
    return sum(b for i, b in enumerate(f) if i != SUM) & 0xFF


class Gateway:
    def __init__(self):
        self.tx_seq = 0
        self.rx_next = 0
        self.awaiting = False
        self.inflight = None
        self.out = []            # [ch, bytearray] queued for the SNES
        self.lock = threading.Lock()
        self.cstate = 0
        self.ctype = 0
        self.clen = 0
        self.cbuf = bytearray()

    # ------------------------------------------------------------ outbound
    def push(self, ch, data):
        with self.lock:
            self.out.append([ch, bytearray(data)])

    def push_ctrl(self, typ, payload):
        self.push(CH_CTRL, bytes([typ, len(payload)]) + payload)

    # ------------------------------------------------------------- fetching
    def fetch(self, ch, url):
        """Runs on its own thread: a blocking fetch must not stall the link, or
        the SNES times out waiting for a status it was always going to get."""
        if "://" not in url:
            url = "https://" + url
        try:
            req = urllib.request.Request(
                url,
                headers={"User-Agent": USER_AGENT, "Accept-Encoding": "gzip"},
            )
            with urllib.request.urlopen(req, timeout=20) as resp:
                status = resp.status
                body = resp.read(MAX_BODY)
                if resp.headers.get("Content-Encoding", "") == "gzip":
                    try:
                        body = gzip.decompress(body)
                    except Exception:
                        pass
        except urllib.error.HTTPError as e:
            status = e.code
            body = e.read(MAX_BODY) or b""
        except Exception as e:
            print(f"  fetch failed: {e}")
            self.push_ctrl(S2C_ERR, bytes([ch, 1]))
            return

        print(f"  -> {status}, {len(body)} bytes")
        self.push_ctrl(S2C_OPEN, struct.pack("<BH", ch, status))
        # Chunked so one huge stream cannot monopolise the frame builder.
        for i in range(0, len(body), 512):
            self.push(ch, body[i:i + 512])
        self.push_ctrl(S2C_EOF, bytes([ch]))

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
        if self.ctype == C2S_FETCH and len(self.cbuf) >= 1:
            ch = self.cbuf[0]
            url = self.cbuf[1:].decode("ascii", "replace")
            print(f"FETCH ch{ch} {url}")
            threading.Thread(target=self.fetch, args=(ch, url), daemon=True).start()
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
        f[SUM] = 0
        f[SUM] = checksum(f)
        return f, any_data

    def on_frame(self, f):
        if f[SUM] != checksum(f):
            return                      # corrupt: say nothing, peer resends
        if self.awaiting and f[ACK] == self.tx_seq:
            self.awaiting = False
            self.tx_seq = (self.tx_seq + 1) & 0xFF
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
    for path in (UP, DOWN):
        open(path, "wb").close()
    gw = Gateway()
    up_off = 0
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
