#!/usr/bin/env python3
"""mkmc: bake a Minecraft server's wire bytes at build time (M13 step 1).

The design insight of the whole milestone, applied: the 65816 never composes
a Minecraft packet, it STREAMS ones this tool baked — the same build-time
philosophy as mkromfs and the app banks. Everything static about the session
becomes bytes here: the status JSON, the whole join sequence, every chunk
and its light, the keepalive template. The console's job shrinks to a
VarInt-length skipper, a three-state machine, and a blob copier.

Target: Java Edition, protocol 754 (1.16.5), offline mode. Chosen because it
is the last comfortable protocol for a machine like ours: plain TCP, no chat
signing (pre-1.19), no configuration-phase registry dump (pre-1.20.5),
palette chunks (1.13+) so a superflat section is ~2 KB. Compression is never
enabled — Set Compression is simply never sent, which the protocol allows
and which is the difference between "possible" and "zlib on a 65816".

Everything that can be FULLY framed is stored fully framed (length prefix,
packet id, payload), so the serving side — Python prober today, the `mc` app
streaming from ROM at step 2 — never edits those bytes. The exceptions are
templates with holes, and the manifest names each hole's offset:

  login_success   16-byte UUID + the player's name (length-prefixed string)
                  must be spliced; the packet length depends on the name, so
                  this one is framed at runtime from two baked halves.
  keepalive       framed, with an 8-byte id patched in place.
  pong            framed, with the client's 8-byte ping payload echoed.

Output: --out DIR writes the blobs plus manifest.json. Importing this module
and calling bake() returns the same bytes; tools/mcprobe.py does exactly
that, so what the real client validates is byte-for-byte what step 2 will
put in ROM.

Packet ids and registry ids below are for protocol 754 specifically. They
were written from the 1.16.5 protocol documentation and are VERIFIED BY THE
PROBER against a real client — if the client balks, the constant tables
here are the single place to fix.
"""
import argparse
import hashlib
import json
import math
import os
import struct

PROTOCOL = 754
VERSION_NAME = "1.16.5"

# ------------------------------------------------------------ wire scalars

def varint(v):
    """Unsigned LEB128-style VarInt, 7 bits per byte, MSB = continue."""
    v &= 0xFFFFFFFF
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def mcstring(s):
    b = s.encode("utf-8")
    return varint(len(b)) + b


def position(x, y, z):
    """The 1.14+ packed Position: x 26 bits, z 26 bits, y 12 bits."""
    return struct.pack(
        ">Q",
        ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF),
    )


def packet(pid, payload):
    """Frame one packet: VarInt(len) VarInt(id) payload. Uncompressed."""
    body = varint(pid) + payload
    return varint(len(body)) + body


def offline_uuid(name):
    """Vanilla's offline-mode UUID: md5 of 'OfflinePlayer:<name>', v3 bits."""
    d = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode()).digest())
    d[6] = (d[6] & 0x0F) | 0x30
    d[8] = (d[8] & 0x3F) | 0x80
    return bytes(d)


# ------------------------------------------------------------------- NBT
# A writer for exactly the subset the join sequence needs. Network NBT roots
# are a named TAG_Compound with an empty name.

def _nbt_payload(v):
    if isinstance(v, NByte):
        return b"\x01", struct.pack(">b", v.v)
    if isinstance(v, NInt):
        return b"\x03", struct.pack(">i", v.v)
    if isinstance(v, NLong):
        return b"\x04", struct.pack(">q", v.v)
    if isinstance(v, NFloat):
        return b"\x05", struct.pack(">f", v.v)
    if isinstance(v, NDouble):
        return b"\x06", struct.pack(">d", v.v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        return b"\x08", struct.pack(">H", len(b)) + b
    if isinstance(v, NLongArray):
        return b"\x0c", struct.pack(">i", len(v.v)) + b"".join(
            struct.pack(">Q", x & 0xFFFFFFFFFFFFFFFF) for x in v.v)
    if isinstance(v, list):  # TAG_List of compounds (all our lists are)
        out = b"\x0a" + struct.pack(">i", len(v))
        for item in v:
            out += _compound_body(item)
        return b"\x09", out
    if isinstance(v, dict):
        return b"\x0a", _compound_body(v)
    raise TypeError(f"nbt: {type(v)}")


def _compound_body(d):
    out = bytearray()
    for k, v in d.items():
        tag, payload = _nbt_payload(v)
        kb = k.encode("utf-8")
        out += tag + struct.pack(">H", len(kb)) + kb + payload
    out += b"\x00"
    return bytes(out)


def nbt_root(d):
    return b"\x0a\x00\x00" + _compound_body(d)


class NByte:
    def __init__(self, v): self.v = v
class NInt:
    def __init__(self, v): self.v = v
class NLong:
    def __init__(self, v): self.v = v
class NFloat:
    def __init__(self, v): self.v = v
class NDouble:
    def __init__(self, v): self.v = v
class NLongArray:
    def __init__(self, v): self.v = v


# ------------------------------------------------- protocol 754 packet ids
# Clientbound (play) unless said otherwise.
P_BLOCK_CHANGE = 0x0B
P_DIFFICULTY = 0x0D
P_CHAT = 0x0E
P_PLUGIN_MSG = 0x17
P_KEEPALIVE = 0x1F
P_CHUNK_DATA = 0x20
P_UPDATE_LIGHT = 0x23
P_JOIN_GAME = 0x24
P_ABILITIES = 0x30
P_PLAYER_INFO = 0x32
P_POS_LOOK = 0x34
P_HELD_ITEM = 0x3F
P_VIEW_POSITION = 0x40
P_SPAWN_POSITION = 0x42
P_TIME_UPDATE = 0x4E
# Login state
P_COMBAT_EVENT = 0x31
P_RESPAWN = 0x39
P_UPDATE_HEALTH = 0x49
P_LOGIN_SUCCESS = 0x02
# Status state
P_STATUS_RESPONSE = 0x00
P_PONG = 0x01

# Serverbound ids the runtime cares about (the rest are skipped by length).
S_TELEPORT_CONFIRM = 0x00
S_CHAT = 0x03
S_KEEPALIVE = 0x10
S_PLAYER_DIGGING = 0x1B
S_CLIENT_STATUS = 0x04       # action 0 = "respawn me"
S_PLAYER_POSITION = 0x11
S_PLAYER_POSITION_ROT = 0x12

# --------------------------------------------------- block/biome registry
# Global block-state ids for 1.16.5 — only the handful the world uses.
B_AIR = 0
B_STONE = 1
B_GRASS = 9        # grass_block[snowy=false]
B_DIRT = 10
B_BEDROCK = 33
B_WATER = 34   # water[level=0], the source block: 16 states follow bedrock
B_SAND = 66    # ...then lava's 16, then sand
BIOME_PLAINS = 1

# ------------------------------------------------------------- the world
#
# Superflat: bedrock at y0, dirt y1..2, grass at y3 — one mixed section per
# chunk, everything above single-valued air (which costs nothing to NOT
# send: the primary bit mask simply omits empty sections). On top of it, a
# sculpture: block letters standing on the grass, because ROM is the one
# resource this machine is rich in and bare flatness would waste the view.
#
# The sculpture is a wall of stone on the z = SCULPT_Z plane, y 4..8, drawn
# from the 5-row font below. Spawn is south of it, facing north (yaw 180).

SURFACE_Y = 3          # grass layer
SCULPT_Z = 8
SPAWN = (0.5, 4.0, 20.5)     # standing on the grass, letters to the north
SPAWN_CHUNK = (0, 1)         # chunk containing SPAWN
VIEW_DISTANCE = 2            # server-dictated; 5x5 chunks around spawn

# ---------------------------------------------------------------- terrain
# A tiny hill and a pond, because ROM is the resource this machine has to
# spare and a bare plain wastes the view. Both stay inside SECTION 0
# (y < 16): the light packet's whole design rests on every face of the world
# living there, and mkmc_test asserts it.
#
# Shapes are integer cones — no float anywhere — and the CONSOLE gets them as
# a table indexed by SQUARED distance (81 bytes each), so its dig-revert can
# answer "what was here?" without a square root.
MOUNT_CX, MOUNT_CZ, MOUNT_R, MOUNT_H = -22, 4, 9, 10
LAKE_CX, LAKE_CZ, LAKE_R = 22, 16, 7
# How far you fall before the world gives up on you. A power of two, because
# the console decides from the EXPONENT of the client's Y alone — it has no
# floating point to compare with. Deep enough that the fall reads as a fall.
VOID_DEPTH = 1024
SHORE_R = LAKE_R + 2


def mount_top(x, z):
    """Top y of the hill at this column, or None."""
    dx, dz = x - MOUNT_CX, z - MOUNT_CZ
    d2 = dx * dx + dz * dz
    if d2 >= MOUNT_R * MOUNT_R:
        return None
    h = MOUNT_H - (math.isqrt(d2) * MOUNT_H) // MOUNT_R
    return SURFACE_Y + h if h > 0 else None


def lake_floor(x, z):
    """Floor y of the pond at this column, or None. Water fills from just
    above it up to the surface."""
    dx, dz = x - LAKE_CX, z - LAKE_CZ
    d2 = dx * dx + dz * dz
    if d2 >= LAKE_R * LAKE_R:
        return None
    depth = 2 - (2 * math.isqrt(d2)) // LAKE_R
    return max(1, SURFACE_Y - 1 - depth)


def mount_table():
    """Top y per squared distance — what the console indexes instead of
    taking a square root. Shared with emit_header so the test that compares
    the console's RULE against this one is checking the same numbers."""
    out = []
    for d2 in range(MOUNT_R * MOUNT_R):
        h = MOUNT_H - (math.isqrt(d2) * MOUNT_H) // MOUNT_R
        out.append(SURFACE_Y + h if h > 0 else 0)
    return out


def lake_table():
    """Floor y per squared distance, or 0xFE for the sand shore beyond the
    water's edge."""
    out = []
    for d2 in range(SHORE_R * SHORE_R):
        if d2 < LAKE_R * LAKE_R:
            depth = 2 - (2 * math.isqrt(d2)) // LAKE_R
            out.append(max(1, SURFACE_Y - 1 - depth))
        else:
            out.append(0xFE)
    return out


def is_shore(x, z):
    dx, dz = x - LAKE_CX, z - LAKE_CZ
    d2 = dx * dx + dz * dz
    return LAKE_R * LAKE_R <= d2 < SHORE_R * SHORE_R


FONT = {
    "S": ["####", "#...", "####", "...#", "####"],
    "N": ["#..#", "##.#", "#.##", "#..#", "#..#"],
    "O": ["####", "#..#", "#..#", "#..#", "####"],
}
SCULPT_TEXT = "SNOS"


def sculpture_columns():
    """{(x, y, z): state} for the standing letters, centred on x = 0."""
    cols = {}
    widths = [len(FONT[c][0]) for c in SCULPT_TEXT]
    total = sum(widths) + (len(SCULPT_TEXT) - 1)  # 1 column gap between
    x0 = -(total // 2)
    x = x0
    for ch in SCULPT_TEXT:
        rows = FONT[ch]
        for ry, row in enumerate(rows):
            y = 4 + (len(rows) - 1 - ry)  # row 0 is the TOP of the glyph
            for rx, cell in enumerate(row):
                if cell == "#":
                    cols[(x + rx, y, SCULPT_Z)] = B_STONE
        x += len(rows[0]) + 1
    return cols


SCULPTURE = sculpture_columns()


def block_at(x, y, z):
    """The static world, as a RULE — which is what lets the console answer a
    dig with the block that was there, from a few lines of C and two small
    tables rather than a copy of the map."""
    if y == 0:
        return B_BEDROCK
    top = mount_top(x, z)
    if top is not None and SURFACE_Y < y <= top:
        return B_GRASS if y == top else B_STONE
    floor = lake_floor(x, z)
    if floor is not None:
        if y < floor:
            return B_DIRT
        if y == floor:
            return B_SAND
        if y <= SURFACE_Y:
            return B_WATER
        return B_AIR
    if y <= 2:
        return B_DIRT
    if y == SURFACE_Y:
        return B_SAND if is_shore(x, z) else B_GRASS
    return SCULPTURE.get((x, y, z), B_AIR)


def height_at(x, z):
    """MOTION_BLOCKING: y of the highest blocking block, plus one."""
    for y in range(15, SURFACE_Y - 1, -1):
        if block_at(x, y, z) != B_AIR:
            return y + 1
    return SURFACE_Y + 1


# ------------------------------------------------------- chunk encoding

def packed_longs(values, bits):
    """1.16+ packing: entries never span longs; 64//bits entries per long,
    filled from the least significant bit."""
    per = 64 // bits
    longs = []
    for i in range(0, len(values), per):
        word = 0
        for j, v in enumerate(values[i:i + per]):
            word |= (v & ((1 << bits) - 1)) << (j * bits)
        longs.append(word)
    return longs


def heightmap_nbt(cx, cz):
    hm = [height_at(cx * 16 + lx, cz * 16 + lz)
          for lz in range(16) for lx in range(16)]
    packed = NLongArray(packed_longs(hm, 9))
    return nbt_root({"MOTION_BLOCKING": packed,
                     "WORLD_SURFACE": NLongArray(packed_longs(hm, 9))})


def section_bytes(cx, cz, sy):
    """One 16^3 section, palette-encoded, or None if it is all air."""
    palette = [B_AIR]
    blocks = []
    nonair = 0
    for y in range(16):
        for z in range(16):
            for x in range(16):
                b = block_at(cx * 16 + x, sy * 16 + y, cz * 16 + z)
                if b != B_AIR:
                    nonair += 1
                if b not in palette:
                    palette.append(b)
                blocks.append(b)
    if nonair == 0:
        return None
    bits = max(4, (len(palette) - 1).bit_length())  # 4 is the indirect floor
    idx = {b: i for i, b in enumerate(palette)}
    longs = packed_longs([idx[b] for b in blocks], bits)
    out = struct.pack(">h", nonair)
    out += bytes([bits])
    out += varint(len(palette))
    for p in palette:
        out += varint(p)
    out += varint(len(longs))
    out += b"".join(struct.pack(">Q", w) for w in longs)
    return out


def chunk_packet(cx, cz):
    """Chunk Data 0x20, full chunk, uncompressed."""
    sections = []
    mask = 0
    for sy in range(16):
        s = section_bytes(cx, cz, sy)
        if s is not None:
            mask |= 1 << sy
            sections.append(s)
    data = b"".join(sections)
    biomes = b"".join(varint(BIOME_PLAINS) for _ in range(1024))
    payload = struct.pack(">ii", cx, cz)
    payload += b"\x01"                       # full chunk
    payload += varint(mask)
    payload += heightmap_nbt(cx, cz)
    payload += varint(1024) + biomes
    payload += varint(len(data)) + data
    payload += varint(0)                     # no block entities
    return packet(P_CHUNK_DATA, payload)


def light_packet(cx, cz):
    """Update Light 0x23. Only section 0 (mask bit 1: the mask covers
    sections -1..16) carries a sky-light array — the world's every face
    lives in section 0, and faces sample light from adjacent blocks, which
    are also in section 0. Everything else is declared empty. Full-bright
    (15) throughout the array: light under the grass is wrong and invisible.
    """
    ALL = (1 << 18) - 1
    payload = varint(cx) + varint(cz)
    payload += b"\x01"                       # trust edges
    payload += varint(1 << 1)                # sky light: section 0
    payload += varint(0)                     # block light: none
    payload += varint(ALL & ~(1 << 1))       # empty sky light: the rest
    payload += varint(ALL)                   # empty block light: everything
    payload += varint(2048) + b"\xff" * 2048
    return packet(P_UPDATE_LIGHT, payload)


# ---------------------------------------------------------- join sequence

def dimension_element():
    return {
        "piglin_safe": NByte(0),
        "natural": NByte(1),
        "ambient_light": NFloat(0.0),
        "infiniburn": "minecraft:infiniburn_overworld",
        "respawn_anchor_works": NByte(0),
        "has_skylight": NByte(1),
        "bed_works": NByte(1),
        "effects": "minecraft:overworld",
        "has_raids": NByte(1),
        "logical_height": NInt(256),
        "coordinate_scale": NDouble(1.0),
        "ultrawarm": NByte(0),
        "has_ceiling": NByte(0),
    }


def biome_element():
    # The client hard-validates this MapLike on Join Game decode and its
    # error names any missing key outright — `depth` and `scale` were found
    # exactly that way (first real-client run, 2026-08-01). Values are
    # vanilla plains.
    return {
        "precipitation": "rain",
        "depth": NFloat(0.125),
        "scale": NFloat(0.05),
        "temperature": NFloat(0.8),
        "downfall": NFloat(0.4),
        "category": "plains",
        "effects": {
            "sky_color": NInt(7907327),
            "water_fog_color": NInt(329011),
            "fog_color": NInt(12638463),
            "water_color": NInt(4159204),
            "mood_sound": {
                "tick_delay": NInt(6000),
                "offset": NDouble(2.0),
                "sound": "minecraft:ambient.cave",
                "block_search_extent": NInt(8),
            },
        },
    }


def join_game_packet():
    codec = nbt_root({
        "minecraft:dimension_type": {
            "type": "minecraft:dimension_type",
            "value": [{
                "name": "minecraft:overworld",
                "id": NInt(0),
                "element": dimension_element(),
            }],
        },
        "minecraft:worldgen/biome": {
            "type": "minecraft:worldgen/biome",
            "value": [{
                "name": "minecraft:plains",
                "id": NInt(BIOME_PLAINS),
                "element": biome_element(),
            }],
        },
    })
    payload = struct.pack(">i", 1)           # entity id
    payload += b"\x00"                       # not hardcore
    payload += bytes([1])                    # gamemode: creative
    payload += struct.pack(">b", -1)         # previous gamemode
    payload += varint(1) + mcstring("minecraft:overworld")
    payload += codec
    payload += nbt_root(dimension_element())
    payload += mcstring("minecraft:overworld")
    payload += struct.pack(">q", 0)          # hashed seed
    payload += varint(1)                     # max players
    payload += varint(VIEW_DISTANCE)
    payload += b"\x00"                       # reduced debug info
    payload += b"\x01"                       # enable respawn screen
    payload += b"\x00"                       # is debug
    payload += b"\x01"                       # is flat: superflat horizon
    return packet(P_JOIN_GAME, payload)


def spiral(cx, cz, r):
    """Chunk coords in rings outward from (cx, cz): spawn chunk FIRST, so
    'Loading terrain' clears on the earliest bytes, not the last."""
    out = [(cx, cz)]
    for ring in range(1, r + 1):
        for dz in range(-ring, ring + 1):
            for dx in range(-ring, ring + 1):
                if max(abs(dx), abs(dz)) == ring:
                    out.append((cx + dx, cz + dz))
    return out


def death_blob():
    """What falling out of the world looks like.

    No health is what raises the death screen; the combat event is what puts
    a line on it. Nothing else happens — the client owns the Respawn button,
    and the console does nothing until it asks (Client Status, action 0)."""
    out = packet(P_UPDATE_HEALTH,
                 struct.pack(">f", 0.0) + varint(0) + struct.pack(">f", 0.0))
    out += packet(P_COMBAT_EVENT,
                  varint(2) +                       # ENTITY_DEAD
                  varint(1) +                       # our player entity id
                  struct.pack(">i", -1) +           # killed by no entity
                  mcstring('{"text":"fell out of the world"}'))
    return out


def respawn_blob():
    """The answer to "respawn me": same world, same dimension, back at spawn.

    Deliberately NOT followed by the chunks. In 1.16.5 the client rebuilds
    its world only when the dimension KEY changes (handleRespawn compares it
    against the one it already has), and this respawn stays in
    minecraft:overworld — so the terrain it already holds is still good, and
    re-sending 146 KB would cost four minutes on the emulated link for
    nothing. If a client ever does respawn into a void, the fix is one line:
    call send_world() between this blob and play_post."""
    payload = nbt_root(dimension_element())
    payload += mcstring("minecraft:overworld")
    payload += struct.pack(">q", 0)          # hashed seed
    payload += bytes([1])                    # gamemode: creative
    payload += bytes([1])                    # previous gamemode
    payload += b"\x00"                       # is debug
    payload += b"\x01"                       # is flat
    payload += b"\x00"                       # copy metadata
    out = packet(P_RESPAWN, payload)
    out += packet(P_UPDATE_HEALTH,
                  struct.pack(">f", 20.0) + varint(20) + struct.pack(">f", 5.0))
    out += packet(P_ABILITIES,
                  bytes([0x0D]) + struct.pack(">ff", 0.05, 0.1))
    out += packet(P_HELD_ITEM, bytes([0]))
    out += packet(P_VIEW_POSITION,
                  varint(SPAWN_CHUNK[0]) + varint(SPAWN_CHUNK[1]))
    return out


def status_json():
    return json.dumps({
        "version": {"name": VERSION_NAME, "protocol": PROTOCOL},
        "players": {"max": 1, "online": 0, "sample": []},
        "description": {"text": "On a Super Nintendo. Yes, really."},
    }, separators=(",", ":"))


def bake():
    """All the blobs, exactly as they will live in ROM."""
    blobs = {}

    blobs["status_response"] = packet(
        P_STATUS_RESPONSE, mcstring(status_json()))
    # Pong: framed with an 8-byte hole for the echoed payload.
    blobs["pong"] = packet(P_PONG, b"\x00" * 8)

    # Login Success cannot be pre-framed (the name's length is the
    # client's); bake the id byte so the runtime only splices fields.
    blobs["login_success_id"] = varint(P_LOGIN_SUCCESS)

    # The pre-chunk join burst, one blob, in vanilla's order.
    pre = b""
    pre += join_game_packet()
    pre += packet(P_PLUGIN_MSG,
                  mcstring("minecraft:brand") + mcstring("SNOS"))
    pre += packet(P_DIFFICULTY, bytes([0, 1]))          # peaceful, locked
    pre += packet(P_ABILITIES,
                  bytes([0x0D]) + struct.pack(">ff", 0.05, 0.1))
    pre += packet(P_HELD_ITEM, bytes([0]))
    pre += packet(P_VIEW_POSITION,
                  varint(SPAWN_CHUNK[0]) + varint(SPAWN_CHUNK[1]))
    blobs["play_pre"] = pre

    # Chunks, spawn-first, light before data (vanilla's order), one blob
    # per chunk so the streamer can interleave keepalives between them.
    order = spiral(*SPAWN_CHUNK, VIEW_DISTANCE)
    for cx, cz in order:
        blobs[f"chunk_{cx}_{cz}"] = light_packet(cx, cz) + \
            chunk_packet(cx, cz)
    chunk_names = [f"chunk_{cx}_{cz}" for cx, cz in order]

    # After the chunks: where the world is, where the player is, what time
    # it is. Fixed teleport id 1 — the confirm is accepted regardless.
    post = b""
    post += packet(P_SPAWN_POSITION, position(0, 4, SCULPT_Z))
    post += packet(
        P_POS_LOOK,
        struct.pack(">dddff", SPAWN[0], SPAWN[1], SPAWN[2], 180.0, 0.0) +
        b"\x00" + varint(1))
    post += packet(P_TIME_UPDATE, struct.pack(">qq", 0, -6000))  # frozen noon
    blobs["play_post"] = post

    # Keepalive: framed, 8-byte id at a fixed offset for patching.
    ka = packet(P_KEEPALIVE, b"\x00" * 8)
    blobs["keepalive"] = ka
    blobs["death"] = death_blob()
    blobs["respawn"] = respawn_blob()

    manifest = {
        "protocol": PROTOCOL,
        "version": VERSION_NAME,
        "spawn": SPAWN,
        "spawn_chunk": SPAWN_CHUNK,
        "view_distance": VIEW_DISTANCE,
        "chunks": chunk_names,
        "keepalive_id_offset": len(ka) - 8,
        "pong_payload_offset": len(blobs["pong"]) - 8,
        "surface_y": SURFACE_Y,
        "sculpt_z": SCULPT_Z,
        "blocks": {"air": B_AIR, "stone": B_STONE, "grass": B_GRASS,
                   "dirt": B_DIRT, "bedrock": B_BEDROCK},
        "sizes": {k: len(v) for k, v in blobs.items()},
    }
    return blobs, manifest


def rom_layout(blobs, manifest, base):
    """Place the blobs into one binary for the cart, and answer where each
    landed as an absolute 24-bit address.

    No blob may cross a 64 KB bank boundary: the streamer reads through a
    __far pointer, and keeping every blob inside one bank means nothing ever
    depends on Calypsi's far-pointer arithmetic carrying between banks — a
    thing best not discovered on real hardware. Blobs are at most ~6 KB, so
    first-fit with bank padding wastes almost nothing."""
    assert base % 0x10000 == 0, "MC data must start on a bank boundary"
    order = ["status_response", "pong", "login_success_id", "play_pre"] + \
        manifest["chunks"] + ["play_post", "keepalive", "death", "respawn"]
    image = bytearray()
    placed = {}
    for name in order:
        data = blobs[name]
        off = len(image)
        room = 0x10000 - (off & 0xFFFF)
        if len(data) > room:
            image += b"\xff" * room
            off = len(image)
        placed[name] = (base + off, len(data))
        image += data
    return bytes(image), placed, order


def emit_header(path, blobs, manifest, placed, order):
    """build/mcdata.h: the app's compile-time index into the ROM image."""
    chunks = manifest["chunks"]
    with open(path, "w") as f:
        w = f.write
        w("/* Generated by tools/mkmc.py - do not edit. The addresses are\n")
        w(" * absolute 24-bit cart addresses; the Makefile's MCDATA_AT and\n")
        w(" * mkmc's --base MUST agree or every blob is garbage. */\n")
        w("#ifndef MCDATA_H\n#define MCDATA_H\n#include <stdint.h>\n\n")
        w("typedef struct { uint32_t addr; uint16_t len; } mc_blob_t;\n\n")
        for name in ("status_response", "pong", "login_success_id",
                     "play_pre", "play_post", "keepalive", "death",
                     "respawn"):
            a, ln = placed[name]
            w(f"#define MC_{name.upper()} "
              f"{{ 0x{a:06x}ul, {ln}u }}\n")
        w(f"\n#define MC_NCHUNKS {len(chunks)}\n")
        w("#define MC_CHUNK_TABLE { \\\n")
        for c in chunks:
            a, ln = placed[c]
            w(f"  {{ 0x{a:06x}ul, {ln}u }}, \\\n")
        w("}\n\n")
        w(f"#define MC_KEEPALIVE_ID_OFF {manifest['keepalive_id_offset']}\n")
        w(f"#define MC_PONG_PAYLOAD_OFF {manifest['pong_payload_offset']}\n")
        w(f"#define MC_PROTOCOL {PROTOCOL}\n")
        w(f"#define MC_PORT 25565\n\n")
        w("/* The static world, as the dig-revert rule needs it. */\n")
        w(f"#define MC_SURFACE_Y {SURFACE_Y}\n")
        w(f"#define MC_SCULPT_Z {SCULPT_Z}\n")
        w(f"#define MC_B_AIR {B_AIR}\n#define MC_B_STONE {B_STONE}\n")
        w(f"#define MC_B_GRASS {B_GRASS}\n#define MC_B_DIRT {B_DIRT}\n")
        w(f"#define MC_B_BEDROCK {B_BEDROCK}\n")
        w("/* The hill and the pond, indexed by SQUARED distance from each\n")
        w(" * centre — so the console answers \"what was here?\" with a table\n")
        w(" * lookup and no square root. Entry 0 of each is the centre. */\n")
        w(f"#define MC_MOUNT_CX ({MOUNT_CX})\n")
        w(f"#define MC_MOUNT_CZ ({MOUNT_CZ})\n")
        w(f"#define MC_MOUNT_D2MAX {MOUNT_R * MOUNT_R}\n")
        w("#define MC_MOUNT_TOP { \\\n  ")
        for d2, v in enumerate(mount_table()):
            w(f"{v},")
            if d2 % 16 == 15:
                w(" \\\n  ")
        w(" \\\n}\n")
        w(f"#define MC_LAKE_CX ({LAKE_CX})\n")
        w(f"#define MC_LAKE_CZ ({LAKE_CZ})\n")
        w(f"#define MC_LAKE_D2MAX {SHORE_R * SHORE_R}\n")
        w("#define MC_LAKE_SHORE 0xFE   /* sand, not a floor height */\n")
        w("#define MC_LAKE_FLOOR { \\\n  ")
        for d2, v in enumerate(lake_table()):
            w(f"{v},")
            if d2 % 16 == 15:
                w(" \\\n  ")
        w(" \\\n}\n")
        w(f"#define MC_B_WATER {B_WATER}\n#define MC_B_SAND {B_SAND}\n")
        w("/* Below this, the world has ended. The console only ever tests\n")
        w(" * the SIGN and EXPONENT of the client's 64-bit Y, so the depth is\n")
        w(" * a power of two by construction. */\n")
        w(f"/* kill below Y = -{VOID_DEPTH} */\n")
        w(f"#define MC_VOID_EXPONENT {1023 + VOID_DEPTH.bit_length() - 1}\n")
        xs = [x for (x, _, _) in SCULPTURE]
        x0, x1 = min(xs), max(xs)
        w(f"#define MC_SCULPT_X0 ({x0})\n#define MC_SCULPT_X1 ({x1})\n")
        w("/* One bit per column of the letter wall, rows y=4..8, bit i =\n")
        w(" * x - MC_SCULPT_X0. */\n")
        w("#define MC_SCULPT_ROWS { \\\n")
        for y in range(4, 9):
            m = 0
            for (x, yy, _z) in SCULPTURE:
                if yy == y:
                    m |= 1 << (x - x0)
            w(f"  0x{m:08x}ul, \\\n")
        w("}\n\n#endif\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write loose blobs + manifest here")
    ap.add_argument("--rom", help="write the cart image here")
    ap.add_argument("--header", help="write the C index here (with --rom)")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=0xE00000,
                    help="absolute cart address of --rom's first byte")
    args = ap.parse_args()
    blobs, manifest = bake()
    total = sum(len(v) for v in blobs.values())

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        for name, data in blobs.items():
            with open(os.path.join(args.out, name + ".bin"), "wb") as f:
                f.write(data)
        with open(os.path.join(args.out, "manifest.json"), "w") as f:
            json.dump(manifest, f, indent=1)

    if args.rom:
        image, placed, order = rom_layout(blobs, manifest, args.base)
        with open(args.rom, "wb") as f:
            f.write(image)
        if args.header:
            emit_header(args.header, blobs, manifest, placed, order)
        print(f"mkmc: rom image {len(image)} bytes at 0x{args.base:06x}, "
              f"{len(order)} blobs ({len(manifest['chunks'])} chunks)")
    else:
        print(f"mkmc: {len(blobs)} blobs, {total} bytes "
              f"({len(manifest['chunks'])} chunks)")


if __name__ == "__main__":
    main()
