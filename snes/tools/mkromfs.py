#!/usr/bin/env python3
"""Pack a directory into a SNOS ROMFS image.

Layout (must match snes/kernel/fs.h):
    0   "SNFS"                      magic — deliberately not "SNOS", because
    4   u16 version                 app_scan() walks these banks looking for
    6   u16 file count              that string and would take the filesystem
    8   u8[8] reserved              for an application
    16  entries, 32 bytes each: name[16], u32 offset, u32 size, u8[8] reserved
    ... file data, 4-byte aligned

Usage: mkromfs.py <srcdir> <out.bin>
"""
import os
import struct
import sys

NAME_LEN = 16
ENT_SIZE = 32
HDR_SIZE = 16


def main(argv):
    srcdir, out_path = argv

    names = sorted(
        n for n in os.listdir(srcdir)
        if os.path.isfile(os.path.join(srcdir, n)) and not n.startswith(".")
    )
    for n in names:
        if len(n) > NAME_LEN:
            raise SystemExit(f"{n!r}: name exceeds {NAME_LEN} characters")

    data_start = HDR_SIZE + ENT_SIZE * len(names)
    entries, blob, off = bytearray(), bytearray(), data_start

    for n in names:
        content = open(os.path.join(srcdir, n), "rb").read()
        entries += n.encode("ascii").ljust(NAME_LEN, b"\0")
        entries += struct.pack("<II", off, len(content))
        entries += b"\0" * 8
        blob += content
        pad = -len(content) % 4
        blob += b"\0" * pad
        off += len(content) + pad

    img = bytearray(b"SNFS")
    img += struct.pack("<HH", 1, len(names))
    img += b"\0" * 8
    img += entries
    img += blob

    open(out_path, "wb").write(img)
    print(f"  romfs {out_path}: {len(names)} files, {len(img)} bytes")


if __name__ == "__main__":
    main(sys.argv[1:])
