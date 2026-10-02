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


def console_block(x, y, z):
    """apps/mc/mc.c's world_block(), transliterated.

    The console answers a punched block from a RULE plus two tables rather
    than from a copy of the map, so this rule and mkmc.block_at have to
    agree everywhere — a divergence restores the WRONG block, visibly and
    only where somebody happens to dig. Structure and test order mirror the
    C line for line; the tables come from mkmc so both sides index the same
    numbers."""
    MOUNT, LAKE = mkmc.mount_table(), mkmc.lake_table()

    def d2_of(cx, cz):
        dx, dz = x - cx, z - cz
        if dx < -64 or dx > 64 or dz < -64 or dz > 64:
            return -1
        return dx * dx + dz * dz

    if y == 0:
        return mkmc.B_BEDROCK
    d2 = d2_of(mkmc.MOUNT_CX, mkmc.MOUNT_CZ)
    if 0 <= d2 < len(MOUNT):
        top = MOUNT[d2]
        if top != 0 and mkmc.SURFACE_Y < y <= top:
            return mkmc.B_GRASS if y == top else mkmc.B_STONE
        if d2 == 0 and y == top + 1:
            return mkmc.B_DIAMOND
    d2 = d2_of(mkmc.LAKE_CX, mkmc.LAKE_CZ)
    if 0 <= d2 < len(LAKE):
        f = LAKE[d2]
        if f != 0xFE:
            if y < f:
                return mkmc.B_DIRT
            if y == f:
                return mkmc.B_SAND
            if y <= mkmc.SURFACE_Y:
                return mkmc.B_WATER
            return mkmc.B_AIR
        if y == mkmc.SURFACE_Y:
            return mkmc.B_SAND
    if 0 < y < mkmc.SURFACE_Y:
        return mkmc.B_DIRT
    if y == mkmc.SURFACE_Y:
        return mkmc.B_GRASS
    if (z == mkmc.SCULPT_Z and 4 <= y <= 8
            and (x, y, z) in mkmc.SCULPTURE):
        return mkmc.B_STONE
    return mkmc.B_AIR


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

    # The console's dig-revert rule against the baker's world, everywhere a
    # player can reach: 80 x 80 columns, y 0..15.
    order = manifest["chunks"]
    cxs = [int(c.split("_")[1]) for c in order]
    czs = [int(c.split("_")[2]) for c in order]
    bad = None
    for cx in range(min(cxs), max(cxs) + 1):
        for cz in range(min(czs), max(czs) + 1):
            for lx in range(16):
                for lz in range(16):
                    x, z = cx * 16 + lx, cz * 16 + lz
                    for y in range(16):
                        a, b = mkmc.block_at(x, y, z), console_block(x, y, z)
                        if a != b and bad is None:
                            bad = (x, y, z, a, b)
    check(bad is None,
          f"the console's rule disagrees with the world at {bad}")

    # The terrain is really there, and really inside section 0.
    surf = [mkmc.block_at(x, mkmc.SURFACE_Y, z)
            for x in range(-32, 48) for z in range(-16, 64)]
    check(surf.count(mkmc.B_WATER) > 50, "the pond did not make it")
    check(surf.count(mkmc.B_SAND) > 20, "the shore did not make it")
    peak = max(mkmc.height_at(x, z)
               for x in range(-32, 48) for z in range(-16, 64))
    check(4 < peak < 16, f"the hill peaks at {peak}: flat, or out of section 0")
    check(mkmc.block_at(mkmc.MOUNT_CX, peak - 2, mkmc.MOUNT_CZ)
          == mkmc.B_GRASS, "the hill has no grass cap")

    # One diamond, standing on the summit's grass cap, its top face still
    # lit from section 0, and in its section's palette as a two-byte VarInt.
    check(mkmc.block_at(mkmc.MOUNT_CX, peak - 1, mkmc.MOUNT_CZ)
          == mkmc.B_DIAMOND, "no diamond on the summit")
    check(mkmc.DIAMOND[1] + 1 < 16, "the diamond's top face leaves section 0")
    diamonds = [(x, y, z) for x in range(-32, 48) for z in range(-16, 64)
                for y in range(16) if mkmc.block_at(x, y, z) == mkmc.B_DIAMOND]
    check(diamonds == [mkmc.DIAMOND], f"expected one diamond, got {diamonds}")
    sec = mkmc.section_bytes(mkmc.MOUNT_CX // 16, mkmc.MOUNT_CZ // 16, 0)
    off = 3                                       # block count, bits
    plen, off = read_varint(sec, off)
    palette = []
    for _ in range(plen):
        p, off = read_varint(sec, off)
        palette.append(p)
    check(mkmc.B_DIAMOND in palette,
          f"the summit's section palette {palette} lacks the diamond")

    # Death and respawn frame cleanly, and respawn really is a Respawn.
    check(walk_packets(blobs["death"], "death") ==
          [mkmc.P_UPDATE_HEALTH, mkmc.P_COMBAT_EVENT],
          "the death blob is not health-then-combat-event")
    check(walk_packets(blobs["respawn"], "respawn")[0] == mkmc.P_RESPAWN,
          "the respawn blob does not start with Respawn")

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
