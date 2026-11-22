#!/usr/bin/env python3
"""Turn the linked bank-$00 image into a 4 MB SA-1 .sfc.

The linker emits exactly ROM offset $0000-$7FFF ($00:8000-$FFFF). This pads
to 4 MB with $FF, writes a marker into every 32 KB block, sets the ROM size
byte and fixes the checksum. The header itself is in src/header.s.

Marker, at block * $8000 + $7F80 (8 bytes):
  "MK", the block's ROM offset (24-bit little-endian), that offset inverted.
check_rom reads them through each CPU address to learn which ROM offset the
address maps to. The bank-$00 one sits in the 48 bytes header.s reserves.

Why 4 MB: the largest ROM a retail SA-1 cart used (so every SA-1
implementation must accept it), and the smallest that gives the four Super
MMC registers four distinct 1 MB blocks to prove their reset mapping.

Usage: mksfc.py <in.raw> <out.sfc>
"""
import sys

SIZE = 4 << 20
HDR = 0x7FC0


def main(argv):
    raw = open(argv[0], "rb").read()
    if len(raw) != 0x8000:
        raise SystemExit(f"mksfc: {argv[0]} is {len(raw):#x} bytes, want 0x8000")
    rom = bytearray(raw) + b"\xff" * (SIZE - len(raw))
    if rom[0x7F80:0x7FB0] != b"\xff" * 48:
        raise SystemExit("mksfc: marker area $7F80 is not the reserved filler")
    if rom[HDR + 0x15] != 0x23 or rom[HDR + 0x16] != 0x35:
        raise SystemExit("mksfc: header is not map $23 / type $35 (SA-1)")

    for off in range(0, SIZE, 0x8000):
        o = off.to_bytes(3, "little")
        rom[off + 0x7F80:off + 0x7F88] = b"MK" + o + bytes(b ^ 0xFF for b in o)

    rom[HDR + 0x17] = (SIZE >> 10).bit_length() - 1   # log2(KB): $0C
    rom[HDR + 0x1C:HDR + 0x20] = b"\xff\xff\x00\x00"
    chk = sum(rom) & 0xFFFF
    rom[HDR + 0x1C:HDR + 0x1E] = (chk ^ 0xFFFF).to_bytes(2, "little")
    rom[HDR + 0x1E:HDR + 0x20] = chk.to_bytes(2, "little")
    open(argv[1], "wb").write(rom)
    print(f"{argv[1]}: {SIZE >> 20} MB SA-1, checksum {chk:#06x}, "
          f"bank $00 {len(raw[:0x7F80].rstrip(bytes([0xff])))} of 32640 bytes used")


if __name__ == "__main__":
    main(sys.argv[1:])
