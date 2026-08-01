#!/usr/bin/env python3
"""Structural checks on mkmc's baked Minecraft blobs (M13 step 1).

Not a protocol conformance test — only a real client is that, which is what
tools/mcprobe.py is for. What CAN be asserted hermetically is that the baked
bytes are structurally sound in the ways the console's streamer depends on:
every blob parses as a whole number of framed packets, the chunk internals
account for every byte, the patch offsets in the manifest point where they
claim, and the world model obeys the section-0 constraint the light packet's
whole design rests on. A bug in any of these would surface on the client as
an opaque disconnect, which is the worst place to debug from.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..",
                                "tools"))
import mkmc  # noqa: E402

FAILS = []


def check(ok, what):
    if not ok:
        FAILS.append(what)
        print(f"  ! {what}")


def read_varint(data, off):
    v = 0
    for i in range(5):
        b = data[off + i]
        v |= (b & 0x7F) << (7 * i)
        if not (b & 0x80):
            return v, off + i + 1
    raise ValueError("varint too long")


def walk_packets(blob, name):
    """Every blob must be a whole number of framed packets. Returns ids."""
    ids = []
    off = 0
    while off < len(blob):
        try:
            length, off = read_varint(blob, off)
        except (IndexError, ValueError):
            check(False, f"{name}: truncated frame at {off}")
            return ids
        if off + length > len(blob):
            check(False, f"{name}: frame overruns blob at {off}")
            return ids
        pid, _ = read_varint(blob, off)
        ids.append(pid)
        off += length
    return ids


def skip_nbt(data, off):
    """Walk one named NBT tag starting at `off`; return the offset past it.
    Enough of a reader to prove the writer balanced."""
    tag = data[off]
    off += 1
    if tag == 0:
        return off
    nlen = struct.unpack(">H", data[off:off + 2])[0]
    off += 2 + nlen
    return skip_payload(data, off, tag)


def skip_payload(data, off, tag):
    if tag == 1:
        return off + 1
    if tag == 2:
        return off + 2
    if tag in (3, 5):
        return off + 4
    if tag in (4, 6):
        return off + 8
    if tag == 8:
        return off + 2 + struct.unpack(">H", data[off:off + 2])[0]
    if tag == 9:
        etype = data[off]
        count = struct.unpack(">i", data[off + 1:off + 5])[0]
        off += 5
        for _ in range(count):
            off = skip_payload(data, off, etype)
        return off
    if tag == 10:
        while data[off] != 0:
            off = skip_nbt(data, off)
        return off + 1
    if tag == 12:
        count = struct.unpack(">i", data[off:off + 4])[0]
        return off + 4 + 8 * count
    raise ValueError(f"nbt tag {tag}")


def main():
    blobs, manifest = mkmc.bake()

    # Deterministic: baking twice gives identical bytes (this is what lets
    # the ROM build and the prober serve provably the same session).
    blobs2, _ = mkmc.bake()
    check(all(blobs[k] == blobs2[k] for k in blobs), "bake is deterministic")

    # Every blob frames cleanly.
    for name in ("status_response", "pong", "play_pre", "play_post",
                 "keepalive"):
        walk_packets(blobs[name], name)
    for cname in manifest["chunks"]:
        ids = walk_packets(blobs[cname], cname)
        check(ids == [mkmc.P_UPDATE_LIGHT, mkmc.P_CHUNK_DATA],
              f"{cname}: expected light+chunk, got {ids}")

    pre_ids = walk_packets(blobs["play_pre"], "play_pre")
    check(pre_ids[0] == mkmc.P_JOIN_GAME, "play_pre starts with Join Game")
    post_ids = walk_packets(blobs["play_post"], "play_post")
    check(mkmc.P_POS_LOOK in post_ids, "play_post carries Position And Look")

    # The manifest's patch offsets point at the holes they claim.
    ka = blobs["keepalive"]
    o = manifest["keepalive_id_offset"]
    check(ka[o:o + 8] == b"\x00" * 8, "keepalive id hole is where declared")
    pong = blobs["pong"]
    o = manifest["pong_payload_offset"]
    check(pong[o:o + 8] == b"\x00" * 8, "pong payload hole is where declared")

    # Chunk internals account for every byte.
    cname = manifest["chunks"][0]
    blob = blobs[cname]
    off = 0
    length, off = read_varint(blob, off)          # light packet: skip
    off += length
    length, off = read_varint(blob, off)          # chunk packet
    end = off + length
    pid, off = read_varint(blob, off)
    cx, cz = struct.unpack(">ii", blob[off:off + 8])
    off += 8
    check((cx, cz) == tuple(manifest["spawn_chunk"]),
          "first chunk is the spawn chunk (spiral order)")
    check(blob[off] == 1, "full chunk flag")
    off += 1
    mask, off = read_varint(blob, off)
    check(mask == 1, f"superflat spawn chunk uses section 0 only (mask {mask})")
    off = skip_nbt(blob, off)                     # heightmaps
    count, off = read_varint(blob, off)
    check(count == 1024, "1024 biome entries")
    for _ in range(count):
        _, off = read_varint(blob, off)
    dsize, off = read_varint(blob, off)
    off += dsize
    nbe, off = read_varint(blob, off)
    check(nbe == 0, "no block entities")
    check(off == end, f"chunk payload accounts for every byte "
                      f"({off} vs {end})")

    # Heightmap packing: 256 9-bit values, 7 per long, 37 longs.
    check(len(mkmc.packed_longs([0] * 256, 9)) == 37,
          "heightmap packs to 37 longs (1.16 non-spanning rule)")

    # The world obeys the light packet's load-bearing assumption: every
    # non-air block lives in section 0 (y < 16).
    check(all(y < 16 for (_, y, _) in mkmc.SCULPTURE),
          "sculpture stays inside section 0")
    sx, sy_, sz = mkmc.SPAWN
    check(mkmc.block_at(int(sx), int(sy_) - 1, int(sz)) != mkmc.B_AIR,
          "spawn stands on something")
    check(mkmc.block_at(int(sx), int(sy_), int(sz)) == mkmc.B_AIR and
          mkmc.block_at(int(sx), int(sy_) + 1, int(sz)) == mkmc.B_AIR,
          "spawn is not inside a block")

    # Size sanity: the whole session must fit the step-2 ROM budget with
    # room to spare, and a single chunk must stay well under the netecho-
    # style streaming the console will do.
    total = sum(len(v) for v in blobs.values())
    check(total < 256 * 1024, f"blob total {total} under 256 KB")
    biggest = max(len(blobs[c]) for c in manifest["chunks"])
    check(biggest < 8 * 1024, f"largest chunk blob {biggest} under 8 KB")

    if FAILS:
        print(f"RESULT FAIL mkmc -- {len(FAILS)} checks failed")
        sys.exit(1)
    print(f"RESULT PASS mkmc -- {len(manifest['chunks'])} chunks, "
          f"{total} bytes baked, frames/offsets/world all sound")


if __name__ == "__main__":
    main()
