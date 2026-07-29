"""A throwaway RFB client, to answer one question before any 65816 is written.

The whole appeal of VNC for this machine rests on a claim: that the SNES can ask
the server for pixels in a format it can write straight into VRAM, so nothing in
the middle has to convert anything. RFB does let the client dictate the pixel
format — but "the spec allows it" and "servers actually do it" are different
statements, and betting a few hundred lines of 65816 on the first one would be
careless.

So this asks for 8-bit true colour in RGB 3:3:2 and checks what comes back.

Why 3:3:2 rather than RFB's colour-map mode (true-colour=0), which maps onto
Mode 3 + CGRAM even more directly: colour-map is barely exercised in modern
servers and some refuse it outright. With a 3:3:2 cube preloaded into CGRAM,
an incoming pixel byte IS the palette index — same result, no SetColourMapEntries
to handle, a palette that never changes mid-session, and a format servers
support because everyone tests it.
"""

import socket
import struct
import sys

# RFB message types, server -> client
FRAMEBUFFER_UPDATE = 0
SET_COLOUR_MAP_ENTRIES = 1
BELL = 2
SERVER_CUT_TEXT = 3

RAW, COPY_RECT, RRE, HEXTILE = 0, 1, 2, 5


class RFB:
    def __init__(self, host, port=5900):
        self.sock = socket.create_connection((host, port), timeout=15)
        self.sock.settimeout(15)
        self.buf = b""

    def read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("server closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def handshake(self):
        version = self.read(12)
        # Answer with the version the server offered, capped at 3.8.
        self.sock.sendall(b"RFB 003.008\n")

        n = self.read(1)[0]
        if n == 0:
            reason_len = struct.unpack("!I", self.read(4))[0]
            raise RuntimeError("rejected: " + self.read(reason_len).decode())
        types = self.read(n)
        if 1 not in types:
            raise RuntimeError(f"server will not accept None auth; offers {list(types)}")
        self.sock.sendall(bytes([1]))                      # security type: None
        result = struct.unpack("!I", self.read(4))[0]
        if result != 0:
            raise RuntimeError("auth failed even with None")

        self.sock.sendall(bytes([1]))                      # ClientInit: shared
        w, h = struct.unpack("!HH", self.read(4))
        pf = self.read(16)
        name_len = struct.unpack("!I", self.read(4))[0]
        name = self.read(name_len).decode("latin-1")
        self.w, self.h = w, h
        return version.decode().strip(), w, h, self.parse_pf(pf), name

    @staticmethod
    def parse_pf(pf):
        bpp, depth, big, true = pf[0], pf[1], pf[2], pf[3]
        rmax, gmax, bmax = struct.unpack("!HHH", pf[4:10])
        rsh, gsh, bsh = pf[10], pf[11], pf[12]
        return dict(bpp=bpp, depth=depth, big_endian=big, true_colour=true,
                    rmax=rmax, gmax=gmax, bmax=bmax,
                    rshift=rsh, gshift=gsh, bshift=bsh)

    def set_pixel_format_332(self):
        """8 bits per pixel, true colour, R:3 G:3 B:2 — the SNES's whole palette
        as a fixed cube, so a pixel byte is already a CGRAM index."""
        pf = struct.pack("!BBBBHHHBBBxxx",
                         8,      # bits-per-pixel
                         8,      # depth
                         0,      # big-endian-flag
                         1,      # true-colour-flag
                         7, 7, 3,          # red-max, green-max, blue-max
                         5, 2, 0)          # red-shift, green-shift, blue-shift
        self.sock.sendall(struct.pack("!Bxxx", 0) + pf)

    def set_encodings(self, encodings):
        msg = struct.pack("!BxH", 2, len(encodings))
        for e in encodings:
            msg += struct.pack("!i", e)
        self.sock.sendall(msg)

    def request(self, incremental, x=0, y=0, w=None, h=None):
        self.sock.sendall(struct.pack("!BBHHHH", 3, 1 if incremental else 0,
                                      x, y, w or self.w, h or self.h))

    def pointer(self, x, y, mask=0):
        self.sock.sendall(struct.pack("!BBHH", 5, mask, x, y))

    def update(self):
        """Read one server message; returns a list of rectangles."""
        while True:
            mtype = self.read(1)[0]
            if mtype == FRAMEBUFFER_UPDATE:
                break
            if mtype == SET_COLOUR_MAP_ENTRIES:
                self.read(3)
                first, count = struct.unpack("!HH", self.read(4))
                self.read(count * 6)
                continue
            if mtype == BELL:
                continue
            if mtype == SERVER_CUT_TEXT:
                self.read(3)
                n = struct.unpack("!I", self.read(4))[0]
                self.read(n)
                continue
            raise RuntimeError(f"unexpected server message type {mtype}")

        self.read(1)
        (nrects,) = struct.unpack("!H", self.read(2))
        rects = []
        for _ in range(nrects):
            x, y, w, h, enc = struct.unpack("!HHHHi", self.read(12))
            if enc == RAW:
                data = self.read(w * h)          # 1 byte per pixel at 8bpp
                rects.append((x, y, w, h, enc, data))
            elif enc == COPY_RECT:
                sx, sy = struct.unpack("!HH", self.read(4))
                rects.append((x, y, w, h, enc, (sx, sy)))
            else:
                rects.append((x, y, w, h, enc, None))
                raise RuntimeError(f"encoding {enc} not decoded by this spike")
        return rects


def cube332():
    """The palette a 3:3:2 byte indexes into, as SNES BGR555 words."""
    out = []
    for i in range(256):
        r = (i >> 5) & 7
        g = (i >> 2) & 7
        b = i & 3
        r5 = round(r * 31 / 7)
        g5 = round(g * 31 / 7)
        b5 = round(b * 31 / 3)
        out.append(r5 | (g5 << 5) | (b5 << 10))
    return out


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5900
    c = RFB(host, port)
    version, w, h, pf, name = c.handshake()
    print(f"server      : {version}  '{name}'")
    print(f"framebuffer : {w}x{h}")
    print(f"native format: {pf['bpp']}bpp depth {pf['depth']} "
          f"true_colour={pf['true_colour']} "
          f"max r{pf['rmax']} g{pf['gmax']} b{pf['bmax']}")

    c.set_pixel_format_332()
    c.set_encodings([COPY_RECT, RAW])
    print("requested   : 8bpp true-colour RGB 3:3:2, encodings [CopyRect, Raw]")

    c.request(False)
    rects = c.update()
    total = sum(len(r[5]) for r in rects if r[4] == RAW)
    print(f"\nfull update : {len(rects)} rect(s), {total} bytes of pixels")
    for x, y, rw, rh, enc, data in rects[:4]:
        kind = {RAW: "Raw", COPY_RECT: "CopyRect"}.get(enc, str(enc))
        print(f"  rect {rw}x{rh} at ({x},{y}) {kind}"
              + (f", {len(data)} bytes" if enc == RAW else f" from {data}"))

    expected = w * h
    if total == expected:
        print(f"\nOK: {total} bytes for {w}x{h} is exactly 1 byte per pixel.")
        print("    The server honoured 8bpp — a pixel byte is a CGRAM index.")
    else:
        print(f"\nWARNING: got {total} bytes for {w*h} pixels "
              f"({total / max(expected,1):.2f} bytes/pixel) — format NOT honoured")

    # An incremental request on a still screen should be nearly free. This is
    # the property the whole idea rests on, so measure it rather than assume.
    import time
    c.request(True)
    t0 = time.time()
    try:
        rects2 = c.update()
        n2 = sum(len(r[5]) for r in rects2 if r[4] == RAW)
        print(f"incremental : {len(rects2)} rect(s), {n2} bytes "
              f"after {time.time()-t0:.1f}s")
    except socket.timeout:
        print("incremental : nothing changed in 15s — zero bytes, as hoped")

    print(f"\nCGRAM cube  : {len(cube332())} entries, first 4 = "
          + " ".join(f"{v:04X}" for v in cube332()[:4]))

    # Paint what the console would show, using the SAME 3:3:2 cube the SNES
    # would have in CGRAM. This is not a preview of the server's screen — it is
    # a preview of the SNES's screen, which is the only one that matters.
    out = sys.argv[3] if len(sys.argv) > 3 else None
    if out:
        from PIL import Image
        fb = bytearray(w * h)
        for x, y, rw, rh, enc, data in rects:
            if enc != RAW:
                continue
            for row in range(rh):
                fb[(y + row) * w + x:(y + row) * w + x + rw] = \
                    data[row * rw:(row + 1) * rw]
        im = Image.new("RGB", (w, h))
        px = im.load()
        cube = cube332()
        for yy in range(h):
            for xx in range(w):
                v = cube[fb[yy * w + xx]]
                px[xx, yy] = ((v & 31) << 3, ((v >> 5) & 31) << 3,
                              ((v >> 10) & 31) << 3)
        im.resize((w * 2, h * 2), Image.NEAREST).save(out)
        print(f"wrote       : {out} — what the SNES would display")


if __name__ == "__main__":
    main()
