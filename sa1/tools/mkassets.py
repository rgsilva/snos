#!/usr/bin/env python3
"""Build-time data for sa1probe, emitted as one assembly file.

  font4     128 chars of font8x8_basic as 4bpp tiles (plane 0 only)
  palette   256 BGR555 colours: 0 black, 1 white, 16-255 a hue wheel
  img8      64x32 test picture, chunky 8bpp (one byte per pixel, rows of 64)
  img4      the same picture & 15, packed 4bpp (left pixel in the low nibble)
  exp8/exp4 what type 1 character conversion must turn them into: 8 x 4
            planar tiles in row-major tile order, the PPU's own format
  arith_tbl SA-1 arithmetic cases and their expected results (see below)

The expected values are computed here from the documented behaviour
(fullsnes "SNES Cart SA-1 Character Conversion" / "Arithmetic Maths"), not
from any emulator, so the ROM judges the hardware against the documents.

Usage: mkassets.py <font8x8_basic.h> <out.s>   (also writes <out>.inc, the sizes)
"""
import colorsys
import re
import sys

W, H = 64, 32


def parse_font(path):
    rows = re.findall(r"\{([^{}]*)\}", open(path).read())
    font = [[int(v, 0) for v in r.split(",") if v.strip()] for r in rows]
    font = [g for g in font if len(g) == 8]
    assert len(font) == 128, len(font)
    return font


def rev(b):
    return int(f"{b:08b}"[::-1], 2)


def font4(font):
    out = bytearray()
    for g in font:
        tile = bytearray(32)
        for y in range(8):
            tile[2 * y] = rev(g[y])         # font8x8 is LSB-left
        out += tile
    return out


def palette():
    out = bytearray()
    for i in range(256):
        if i == 0:
            r = g = b = 0
        elif i == 1:
            r = g = b = 31
        elif i < 16:
            r = g = b = i * 2
        else:
            fr, fg, fb = colorsys.hsv_to_rgb((i - 16) / 240, 0.8, 0.9)
            r, g, b = int(fr * 31), int(fg * 31), int(fb * 31)
        c = r | g << 5 | b << 10
        out += bytes((c & 0xFF, c >> 8))
    return out


def picture(font):
    px = [[16 + (x * 3 + y * 5) % 240 for x in range(W)] for y in range(H)]
    for n, ch in enumerate("SA-1"):
        g = font[ord(ch)]
        for y in range(16):
            for x in range(16):
                if g[y // 2] >> (x // 2) & 1:
                    px[8 + y][n * 16 + x] = 1
    return px


def planar(px, bpp):
    out = bytearray()
    for ty in range(H // 8):
        for tx in range(W // 8):
            tile = bytearray(8 * bpp)
            for y in range(8):
                for x in range(8):
                    v = px[ty * 8 + y][tx * 8 + x]
                    for p in range(bpp):
                        if v >> p & 1:
                            tile[y * 2 + (p >> 1) * 16 + (p & 1)] |= 0x80 >> x
            out += tile
    return out


# ------------------------------------------------------------------ arith
# Record (16 bytes): flags (1 = write MCNT, 2 = write MA), MCNT, MA, MB,
# repeat (MB is written this many times), expected MR0-MR4, expected OF,
# judge (1 = MR0-3, 2 = MR4, 4 = OF), group (0 mul, 1 div, 2 sum).
# Documented: multiply is signed 16 x 16 -> 32 and keeps MA; divide is signed
# dividend / unsigned divisor, quotient rounded down, remainder unsigned,
# x / 0 = 0 rem 0; the sum is 40 bits and MCNT bit 1 clears it. OF is judged
# only where every reading of the documents agrees (no overflow possible).
MUL, DIV, SUM = 0, 1, 2
CASES = [
    # group, flags, mcnt, ma, mb, repeat
    (MUL, 3, 0, 1234, 5678, 1),
    (MUL, 2, 0, -3, 7, 1),
    (MUL, 2, 0, -32768, -32768, 1),
    (MUL, 2, 0, 32767, -32768, 1),
    (MUL, 0, 0, None, 2, 1),          # MA kept from the previous multiply
    (DIV, 3, 1, 1000, 7, 1),
    (DIV, 2, 1, -1000, 7, 1),
    (DIV, 2, 1, -32768, 65535, 1),
    (DIV, 2, 1, 12345, 0, 1),
    (DIV, 2, 1, 40000 - 65536, 3, 1), # 40000 is a negative dividend
    (SUM, 3, 2, 1000, 1000, 1),
    (SUM, 2, 2, -500, 300, 1),
    (SUM, 2, 2, 32767, 32767, 1),
    (SUM, 3, 2, -3, 7, 1),
    (SUM, 3, 2, -32768, -32768, 512), # 2^39
    (SUM, 2, 2, -32768, -32768, 512), # 2^40: wraps to 0
]


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def arith():
    recs = []
    ma = 0
    acc = 0
    first_sum = True
    for group, flags, mcnt, a, b, rep in CASES:
        if flags & 2:
            ma = a & 0xFFFF
        b &= 0xFFFF
        judge, of = 1, 0
        if group == MUL:
            res = (s16(ma) * s16(b)) & 0xFFFFFFFF
        elif group == DIV:
            if b == 0:
                q = r = 0
            else:
                q = s16(ma) // b
                r = s16(ma) - q * b
            res = (r << 16) | (q & 0xFFFF)
            ma = 0                         # destroyed by a division
        else:
            if flags & 1:
                acc = 0
            for _ in range(rep):
                acc = (acc + s16(ma) * s16(b)) % (1 << 40)
            res = acc
            judge = 3
            if first_sum:
                judge |= 4
                first_sum = False
        recs.append((flags, mcnt, a if a is not None else 0, b, rep, res, of,
                     judge, group))
    return recs


def emit_bytes(out, label, data):
    out.append(f"{label}:")
    for i in range(0, len(data), 16):
        out.append("              .byte   " +
                   ", ".join(f"0x{b:02x}" for b in data[i:i + 16]))
    out.append(f"{label}_end:")


def main(argv):
    font = parse_font(argv[0])
    px8 = picture(font)
    px4 = [[v & 15 for v in row] for row in px8]
    img8 = bytes(v for row in px8 for v in row)
    img4 = bytes(row[x] | row[x + 1] << 4 for row in px4 for x in range(0, W, 2))
    out = [
        ";;; Generated by tools/mkassets.py. Do not edit.",
        "              .section code, root",
        "              .public font4, font4_end, palette, palette_end",
        "              .public img8, img4, exp8, exp4, arith_tbl",
    ]
    emit_bytes(out, "font4", font4(font))
    emit_bytes(out, "palette", palette())
    emit_bytes(out, "img8", img8)
    emit_bytes(out, "img4", img4)
    emit_bytes(out, "exp8", planar(px8, 8))
    emit_bytes(out, "exp4", planar(px4, 4))
    recs = arith()
    out.append("arith_tbl:")
    for flags, mcnt, a, b, rep, res, of, judge, group in recs:
        r = res.to_bytes(5, "little")
        out.append(
            f"              .byte   {flags}, {mcnt}\n"
            f"              .word   0x{a & 0xFFFF:04x}, 0x{b:04x}, {rep}\n"
            f"              .byte   " + ", ".join(f"0x{x:02x}" for x in r) +
            f", 0x{of:02x}, {judge}, {group}   ; = 0x{res:010x}")
    assert len(recs) * 6 <= 0xE0, "IM_RES holds 224 bytes"
    open(argv[1], "w").write("\n".join(out) + "\n")
    inc = [
        ";;; Generated by tools/mkassets.py. Do not edit.",
        f"FONT4_SIZE    .equ    {128 * 32}",
        f"PALETTE_SIZE  .equ    512",
        f"ARITH_N       .equ    {len(recs)}",
    ]
    open(argv[1].rsplit(".", 1)[0] + ".inc", "w").write("\n".join(inc) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
