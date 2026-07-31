#!/usr/bin/env python3
"""Pad a Calypsi raw HiROM image to a power-of-two .sfc and fix the checksum.

The linker emits one raw image starting at $C00000. Mesen wants full banks
and a plausible header; we pad with 0xFF to the next power of two (min 128 KB)
and patch the checksum/complement at $FFDC-$FFDF so header scoring is happy.

Optional embedded blobs: `--at 0xOFFSET path/to/file` places a binary at a
fixed ROM byte offset before checksumming. HiROM maps SNES $C00000+N linearly
to ROM byte N, so the Mac ROM (--at 0x080000 => $C80000) and floppy image
(--at 0x100000 => $D00000) are reachable by fixed far pointers (macmem.c).
"""
import sys


def main(argv) -> None:
    src, dst = argv[0], argv[1]
    blobs = []                       # (offset, bytes)
    i = 2
    while i < len(argv):
        if argv[i] == "--at":
            off = int(argv[i + 1], 0)
            blobs.append((off, open(argv[i + 2], "rb").read()))
            i += 3
        else:
            raise SystemExit(f"unknown arg {argv[i]}")

    data = bytearray(open(src, "rb").read())
    # Overlap check: blobs are pasted in argv order, and a grown romfs (or a
    # multi-bank app) would silently overwrite its neighbour. Loud beats
    # subtle here -- this is the last step before the ROM exists.
    spans = sorted((off, off + len(b)) for off, b in blobs)
    for (a0, a1), (b0, b1) in zip(spans, spans[1:]):
        if a1 > b0:
            raise SystemExit(
                f"raw2sfc: blob overlap: [{a0:#x},{a1:#x}) collides with "
                f"[{b0:#x},{b1:#x}) -- check the Makefile's APP_AT/ROMFS_AT")
    for off, blob in blobs:
        end = off + len(blob)
        if end > len(data):
            data.extend(b"\xff" * (end - len(data)))
        data[off:end] = blob

    size = 128 * 1024
    while size < len(data):
        size *= 2
    data.extend(b"\xff" * (size - len(data)))

    # ROM-size byte ($FFD7) = log2(KB). Patch it to match the image we actually
    # produced rather than hand-maintaining it in header.s: a header that claims
    # 4 MB for a 256 KB file lowers Mesen's header-scoring confidence, which
    # surfaces as an unhelpful "cartridge not detected" instead of a size warning.
    kb = size // 1024
    data[0xFFD7] = kb.bit_length() - 1

    # checksum: sum of all bytes with checksum fields preset
    data[0xFFDC:0xFFDE] = b"\xff\xff"  # complement placeholder
    data[0xFFDE:0xFFE0] = b"\x00\x00"  # checksum placeholder
    chk = sum(data) & 0xFFFF
    data[0xFFDC] = ~chk & 0xFF
    data[0xFFDD] = (~chk >> 8) & 0xFF
    data[0xFFDE] = chk & 0xFF
    data[0xFFDF] = (chk >> 8) & 0xFF

    open(dst, "wb").write(data)
    extra = "".join(f" +{len(b)//1024}KB@{o:#08x}" for o, b in blobs)
    print(
        f"{dst}: {len(data) // 1024} KB, checksum {chk:#06x}, "
        f"romsize byte {data[0xFFD7]:#04x}{extra}"
    )


if __name__ == "__main__":
    main(sys.argv[1:])
