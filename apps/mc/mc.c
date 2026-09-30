/* SNOS MC — a Minecraft server on a Super Nintendo (M13).
 *
 * The 65816 composes almost nothing here: tools/mkmc.py baked every static
 * packet of a protocol-754 session into cart ROM at build time (see
 * build/mcdata.h for the index), and this app is the machine that streams
 * them — a VarInt-length skipper for what the client says, a four-state
 * machine for the session, and a far-pointer copy loop for what we say.
 * The prober (tools/mcprobe.py) served these EXACT bytes to a real 1.16.5
 * client first, so what this file owes is transport, not protocol.
 *
 * Runtime-composed bytes, the complete list: Login Success (its length is
 * the player's name's), keepalive ids, the pong echo, chat echoes, and the
 * Block Change that restores a punched block — the world is read-only, and
 * the SNES puts it back.
 *
 * ------------------------------------------------------------ discipline
 *
 * A blob is a whole number of framed packets, so nothing may be injected
 * into the byte stream while one is mid-send — a keepalive spliced into the
 * middle of a chunk packet is corruption, not politeness. Keepalives and
 * replies go BETWEEN blobs. While a blob is going out the receive ring is
 * simply not drained: TCP backpressure (uIP closes the window; the client
 * stalls its position spam) is the correct and free behaviour, and the
 * client's inbound half keeps acking us regardless.
 *
 * TWO sessions, one port. Joining is two connections in quick succession —
 * the server-list ping, then the login — and on the real link the login's
 * SYN arrives while the ping connection is still tearing down. With a
 * single listener that SYN was refused with a RST, which is exactly what
 * Ricardo's first live attempt hit. Two channels bound to the same port
 * (the kernel and the daemon both allow it since this app) let the
 * handoff overlap; each session keeps its own framing state, and the
 * blob-streaming discipline stays single-active because sends happen
 * inside the session being pumped.
 */
#include "snos_abi.h"
#include "snoslib.h"
#include "mcdata.h"

#define NSESS 2
#define RXBUF 200          /* every packet we PARSE fits; larger ones are
                            * skipped by length, streaming */
#define KA_FRAMES 600u     /* keepalive every ~10 s; the client kicks ~30 */
/* The trap layer copies at most 128 bytes per SYS_NET_WRITE, so asking for
 * more just means a short write and another trip round the loop. */
#define WRCHUNK 128

static const mc_blob_t BLOB_STATUS = MC_STATUS_RESPONSE;
static const mc_blob_t BLOB_PONG = MC_PONG;
static const mc_blob_t BLOB_PRE = MC_PLAY_PRE;
static const mc_blob_t BLOB_POST = MC_PLAY_POST;
static const mc_blob_t BLOB_KA = MC_KEEPALIVE;
static const mc_blob_t BLOB_DEATH = MC_DEATH;
static const mc_blob_t BLOB_RESPAWN = MC_RESPAWN;
static const mc_blob_t CHUNKS[MC_NCHUNKS] = MC_CHUNK_TABLE;
static const uint32_t SCULPT_ROWS[5] = MC_SCULPT_ROWS;
/* The terrain, as tables indexed by SQUARED distance from each feature's
 * centre — so "what block was here?" costs a lookup and no square root. */
static const uint8_t MOUNT_TOP[MC_MOUNT_D2MAX] = MC_MOUNT_TOP;
static const uint8_t LAKE_FLOOR[MC_LAKE_D2MAX] = MC_LAKE_FLOOR;

/* Serverbound ids this file reacts to; everything else is skipped. */
#define S_CHAT 0x03
#define S_CLIENT_STATUS 0x04   /* action 0 = "respawn me" */
/* 1.16.5 numbering, anchored on the two ids a live client has already
 * proven: Chat 0x03 and Player Digging 0x1B. Counting between them lands
 * Player Position on 0x12 — 0x11 is Lock Difficulty, and reading THAT as a
 * position meant the console only ever saw the updates where the player's
 * aim had not changed. Look around while you fall and it saw nothing. */
#define S_POSITION 0x12
#define S_POSITION_ROT 0x13     /* ...and rotation: the common one, missed */
#define S_DIGGING 0x1B

/* Session states. */
#define ST_HANDSHAKE 0
#define ST_STATUS 1
#define ST_LOGIN 2
#define ST_PLAY 3

typedef struct {
  uint16_t ch;
  uint8_t st;
  /* Inbound framing: phase 0 = collecting the length VarInt, 1 = the body
   * (up to RXBUF of it), 2 = discarding an oversized body's tail. */
  uint8_t phase;
  uint8_t lenshift;
  uint16_t rxgot;
  uint32_t plen;
  uint32_t discard;
  uint16_t ka_at;
  uint8_t dead;       /* fell out of the world; waiting to be asked back */
  uint8_t poison;     /* 1 = unparseable stream, 2 = we said goodbye */
  uint16_t poison_at; /* when, so a peer that will not read cannot pin us */
  uint8_t rx[RXBUF];
  char pname[17];
  uint8_t uuid[16];
} sess_t;

static sess_t S[NSESS];
static sess_t *cur;        /* the session being pumped; handlers read it */

static uint8_t buf[128];   /* shared: runtime-composed packets, blob copies
                            * - sized to the trap layer's copy cap, so each
                            * net-write syscall carries a full helping */
static uint16_t digs, chats;

static void say(const char *s) { sys_puts(s); }
static void pump(void); /* the burst drains between blobs; see there */
/* A blob stream is going out. pump() is still called between blobs to keep
 * the receive ring shallow — an undrained ring stops uIP's window, which
 * the NAT reads as a dead peer — but while this is set it only CONSUMES:
 * dispatching would re-enter the burst from inside itself (a second join or
 * respawn nested in the first) on a machine with a 1.5 KB stack. */
static uint8_t in_burst;

/* ------------------------------------------------------------- outbound */

/* Stream one baked blob to cur's channel. Whole packets only, so nothing
 * else may write to this channel until it returns. 0 = connection died. */
static int send_blob(const mc_blob_t *b) {
  const uint8_t __far *src = (const uint8_t __far *)b->addr;
  uint16_t left = b->len;
  while (left) {
    int room = sys_net_wspace(cur->ch);
    uint16_t n, i;
    if (room <= 0) {
      uint16_t s2 = sys_net_state(cur->ch);
      if (s2 != NET_ST_OPEN && s2 != NET_ST_EOF) return 0;
      sys_yield();
      continue;
    }
    n = (uint16_t)room;
    if (n > WRCHUNK) n = WRCHUNK;
    if (n > left) n = left;
    /* The ROM address goes STRAIGHT into the syscall. SYS_NET_WRITE has
     * always taken a far pointer (bank + offset), so staging the bytes in
     * RAM first was a whole extra per-byte pass over ~146 KB that bought
     * nothing — three copies of every byte where two will do. */
    i = sys_net_write(cur->ch, (const char *)src, n);
    if (i == 0 || i == SYS_ERR) {
      /* Room in the ring but nothing taken: the channel state refused the
       * write (a client RST mid-chunk lands exactly here). Spinning on it
       * burned the whole machine once - net_wspace measures the ring, not
       * the connection. Same exit as the no-room path. */
      uint16_t s2 = sys_net_state(cur->ch);
      if (s2 != NET_ST_OPEN && s2 != NET_ST_EOF) return 0;
      sys_yield();
      continue;
    }
    src += i;
    left -= i;
  }
  return 1;
}

/* A small runtime-composed packet from buf[1..]: frame it and send it.
 * Everything composed here keeps the body under 127 bytes, so the frame's
 * length prefix is a single byte — buf[0]. */
static int send_small(uint16_t bodylen) {
  buf[0] = (uint8_t)bodylen;
  return sl_send_all(cur->ch, (const char *)buf, (uint16_t)(bodylen + 1));
}

static int send_keepalive(void) {
  const uint8_t __far *src = (const uint8_t __far *)BLOB_KA.addr;
  uint16_t i;
  for (i = 0; i < BLOB_KA.len; i++) buf[i] = src[i];
  buf[MC_KEEPALIVE_ID_OFF + 6] = (uint8_t)(sys_frames() >> 8);
  buf[MC_KEEPALIVE_ID_OFF + 7] = (uint8_t)sys_frames();
  return sl_send_all(cur->ch, (const char *)buf, BLOB_KA.len);
}

/* Clientbound chat: {"text":"..."}, position 0, our uuid. ASCII-sanitised
 * and truncated so the whole packet keeps a one-byte frame prefix. */
static void send_chat(const char *a, const char *b) {
  uint16_t o = 2, n;
  const char *parts[2];
  int p;
  parts[0] = a;
  parts[1] = b;
  buf[1] = 0x0E;
  o++;                               /* string length, backpatched */
  buf[o++] = '{'; buf[o++] = '"'; buf[o++] = 't'; buf[o++] = 'e';
  buf[o++] = 'x'; buf[o++] = 't'; buf[o++] = '"'; buf[o++] = ':';
  buf[o++] = '"';
  /* 104 leaves room for the closing quote and brace, the position byte and
   * the 16-byte UUID inside a body that still fits a one-byte frame length
   * (< 128). The old cap of 55 left only 43 characters of text and cut the
   * welcome message off mid-sentence. */
  for (p = 0; p < 2; p++) {
    const char *s = parts[p];
    while (*s && o < 104) {
      char c = *s++;
      if (c >= 32 && c < 127 && c != '"' && c != '\\') buf[o++] = (uint8_t)c;
    }
  }
  buf[o++] = '"'; buf[o++] = '}';
  buf[2] = (uint8_t)(o - 3);         /* JSON string length */
  buf[o++] = 0;                      /* chat position */
  for (n = 0; n < 16; n++) buf[o++] = cur->uuid[n];
  send_small((uint16_t)(o - 1));
}

/* ------------------------------------------------------------- parsing */

/* Is that much of this packet really here? A declared length is the
 * CLIENT's claim; rxgot is what arrived. Parsing past it read whatever the
 * previous packet — or the previous CONNECTION, since rx survives
 * sess_reset — had left in the buffer, and then echoed those bytes back
 * onto the wire as a pong payload, a Block Change position or a player
 * name. */
static int have(uint16_t off, uint16_t need) {
  return (uint32_t)off + (uint32_t)need <= (uint32_t)cur->rxgot;
}

static uint32_t rd_varint(uint16_t *off) {
  uint32_t v = 0;
  uint8_t sh = 0;
  while (sh < 35 && *off < cur->rxgot) {
    uint8_t b = cur->rx[*off];
    (*off)++;
    v |= (uint32_t)(b & 0x7F) << sh;
    sh += 7;
    if (!(b & 0x80)) break;
  }
  return v;
}

/* Login Disconnect (login state, id 0x00): the protocol's own way to say no,
 * so the client shows the reason instead of an IOException. */
static void send_login_disconnect(const char *why) {
  uint16_t o = 2, i;
  buf[1] = 0x00;
  o++;                               /* string length, backpatched */
  buf[o++] = '{'; buf[o++] = '"'; buf[o++] = 't'; buf[o++] = 'e';
  buf[o++] = 'x'; buf[o++] = 't'; buf[o++] = '"'; buf[o++] = ':';
  buf[o++] = '"';
  for (i = 0; why[i] && o < 100; i++) {
    char c = why[i];
    if (c >= 32 && c < 127 && c != '"' && c != '\\') buf[o++] = (uint8_t)c;
  }
  buf[o++] = '"'; buf[o++] = '}';
  buf[2] = (uint8_t)(o - 3);
  send_small((uint16_t)(o - 1));
}

/* Squared distance from a feature's centre, or -1 for "nowhere near". The
 * guard bounds |d| to 64, so the square stays inside 16-bit arithmetic even
 * though the client may name any coordinate in 26 bits. */
static int feature_d2(int32_t x, int32_t z, int32_t cx, int32_t cz) {
  int32_t dx = x - cx, dz = z - cz;
  if (dx < -64 || dx > 64 || dz < -64 || dz > 64) return -1;
  return (int)(dx * dx + dz * dz);
}

/* The world, as the same RULE the baker used — hill, pond, shore, letters,
 * superflat — so a punched block is answered from arithmetic rather than
 * from a copy of the map. The order of the tests mirrors mkmc.block_at
 * exactly; a divergence here would restore the wrong block. */
static uint16_t world_block(int32_t x, int32_t y, int32_t z) {
  int d2;
  if (y == 0) return MC_B_BEDROCK;

  d2 = feature_d2(x, z, MC_MOUNT_CX, MC_MOUNT_CZ);
  if (d2 >= 0 && d2 < MC_MOUNT_D2MAX) {
    int32_t top = (int32_t)MOUNT_TOP[d2];
    if (top != 0 && y > MC_SURFACE_Y && y <= top)
      return (y == top) ? MC_B_GRASS : MC_B_STONE;
  }
  d2 = feature_d2(x, z, MC_LAKE_CX, MC_LAKE_CZ);
  if (d2 >= 0 && d2 < MC_LAKE_D2MAX) {
    uint8_t f = LAKE_FLOOR[d2];
    if (f != MC_LAKE_SHORE) {          /* in the water */
      if (y < (int32_t)f) return MC_B_DIRT;
      if (y == (int32_t)f) return MC_B_SAND;
      if (y <= MC_SURFACE_Y) return MC_B_WATER;
      return MC_B_AIR;
    }
    if (y == MC_SURFACE_Y) return MC_B_SAND;   /* the beach round it */
  }
  if (y > 0 && y < MC_SURFACE_Y) return MC_B_DIRT;
  if (y == MC_SURFACE_Y) return MC_B_GRASS;
  if (z == MC_SCULPT_Z && y >= 4 && y <= 8 &&
      x >= MC_SCULPT_X0 && x <= MC_SCULPT_X1) {
    if (SCULPT_ROWS[y - 4] & ((uint32_t)1 << (x - MC_SCULPT_X0)))
      return MC_B_STONE;
  }
  return MC_B_AIR;
}

/* The world has an edge, and past it a fall that would never end. The
 * client reports its position as three 64-bit doubles and this machine has
 * no FPU — but none is needed: the SIGN and EXPONENT alone answer "is the
 * player more than 32 blocks below zero?", which is the only question. */
static void handle_position(uint16_t off) {
  uint8_t b0, b1;
  uint16_t e;
  if (cur->dead || !have(off, 16)) return;
  b0 = cur->rx[off + 8];               /* Y is the second double */
  b1 = cur->rx[off + 9];
  if (!(b0 & 0x80)) return;            /* positive: still above the world */
  e = (uint16_t)(((uint16_t)(b0 & 0x7Fu) << 4) | (uint16_t)(b1 >> 4));
  if (e < MC_VOID_EXPONENT) return;
  cur->dead = 1;
  send_blob(&BLOB_DEATH);
  say(cur->pname);
  say(" fell out of the world\n");
}

static void handle_digging(uint16_t off) {
  uint32_t hi, lo;
  int32_t x, y, z;
  uint16_t o = off, i, state;
  uint32_t status = rd_varint(&o);
  if (status != 0 && status != 2) return;  /* only a break needs undoing */
  if (!have(o, 8)) return;                 /* no position: not ours to answer */
  hi = ((uint32_t)cur->rx[o] << 24) | ((uint32_t)cur->rx[o + 1] << 16) |
       ((uint32_t)cur->rx[o + 2] << 8) | cur->rx[o + 3];
  lo = ((uint32_t)cur->rx[o + 4] << 24) | ((uint32_t)cur->rx[o + 5] << 16) |
       ((uint32_t)cur->rx[o + 6] << 8) | cur->rx[o + 7];
  x = (int32_t)hi >> 6;
  z = (int32_t)(((hi & 0x3F) << 20) | (lo >> 12));
  if (z & 0x02000000l) z |= (int32_t)0xFC000000l;
  y = (int32_t)(lo & 0xFFF);
  if (y & 0x800) y |= (int32_t)0xFFFFF000l;

  /* Block Change 0x0B: the position echoed straight back (the 8 bytes are
   * already exactly right), then the original state. The world is a RULE,
   * so "original" is computed, not looked up. */
  state = world_block(x, y, z);
  buf[1] = 0x0B;
  for (i = 0; i < 8; i++) buf[2 + i] = cur->rx[o + i];
  buf[10] = (uint8_t)state;          /* every world state id is < 128 */
  send_small(10);
  digs++;
  say("dig - put it back\n");
}

static void handle_chat(uint16_t off) {
  uint16_t o = off, i, p;
  uint32_t n = rd_varint(&o);
  char text[49];
  char pre[21];
  if (n > 48) n = 48;
  for (i = 0; i < n && o + i < cur->rxgot; i++) {
    char c = (char)cur->rx[o + i];
    text[i] = (c >= 32 && c < 127) ? c : '?';
  }
  text[i] = 0;
  p = 0;
  pre[p++] = '<';
  for (i = 0; cur->pname[i] && p < 18; i++) pre[p++] = cur->pname[i];
  pre[p++] = '>';
  pre[p++] = ' ';
  pre[p] = 0;
  say(pre);
  say(text);
  say("\n");
  chats++;
  send_chat(pre, text);              /* the echo IS the server's chat */
}

/* The world: every chunk blob, spawn first, with a drain and a keepalive
 * between them. One function because the join needs it and a respawn might
 * (see the Client Status handler) — and because the in_burst guard wants a
 * single home rather than a copy per caller. */
static int send_world(void) {
  uint16_t i;
  int ok = 1;
  in_burst = 1;
  for (i = 0; i < MC_NCHUNKS; i++) {
    if (!send_blob(&CHUNKS[i])) { ok = 0; break; }
    pump();                          /* drain only, while in_burst holds */
    if ((i & 3) == 3) {
      if (!send_keepalive()) { ok = 0; break; }
      cur->ka_at = sys_frames();
    }
  }
  in_burst = 0;
  return ok;
}

/* One serverbound packet, fully collected (or truncated: only packets we
 * skip anyway ever are). */
static void handle_packet(void) {
  uint16_t off = 0;
  uint32_t pid;
  if (in_burst) return;   /* draining a burst: consume, do not dispatch */
  pid = rd_varint(&off);

  if (cur->st == ST_HANDSHAKE && pid == 0x00) {
    uint32_t next;
    (void)rd_varint(&off);           /* protocol; the client judges from our
                                      * status reply */
    next = rd_varint(&off);          /* server address string: skip */
    /* Its length is the client's to state, and off is 16-bit: unchecked,
     * the skip wrapped and the read after it wandered anywhere in the data
     * bank looking for the next-state byte. */
    if (next > cur->rxgot || !have((uint16_t)(off + next), 2)) return;
    off = (uint16_t)(off + next + 2);
    next = rd_varint(&off);
    cur->st = (next == 1) ? ST_STATUS : ST_LOGIN;
    return;
  }
  if (cur->st == ST_STATUS) {
    if (pid == 0x00) send_blob(&BLOB_STATUS);
    if (pid == 0x01) {
      const uint8_t __far *src = (const uint8_t __far *)BLOB_PONG.addr;
      uint16_t i;
      if (!have(off, 8)) return;     /* nothing to echo but stale bytes */
      for (i = 0; i < BLOB_PONG.len; i++) buf[i] = src[i];
      for (i = 0; i < 8; i++) buf[MC_PONG_PAYLOAD_OFF + i] = cur->rx[off + i];
      sl_send_all(cur->ch, (const char *)buf, BLOB_PONG.len);
    }
    return;
  }
  if (cur->st == ST_LOGIN && pid == 0x00) {
    uint32_t n = rd_varint(&off);
    uint16_t i, o;
    /* One player, as the status line advertises. A second login would get
     * its own multi-minute join burst, during which the incumbent's session
     * is never pumped and never keepalived — so the newcomer would silently
     * time the first player out to take their place. */
    for (i = 0; i < NSESS; i++) {
      if (&S[i] != cur && S[i].st == ST_PLAY) {
        send_login_disconnect("One player at a time. It is a Super Nintendo.");
        say("refused a second player\n");
        cur->poison = 2;
        cur->poison_at = sys_frames();
        return;
      }
    }
    if (n > 16) n = 16;
    if (n == 0 || !have(off, (uint16_t)n)) return;
    for (i = 0; i < n; i++) cur->pname[i] = (char)cur->rx[off + i];
    cur->pname[n] = 0;
    /* An offline-mode client accepts any UUID; vanilla's md5 recipe is not
     * worth a hash on this CPU. Name bytes + version/variant bits. */
    for (i = 0; i < 16; i++)
      cur->uuid[i] = (uint8_t)(cur->pname[i % (n ? n : 1)] + i);
    cur->uuid[6] = (uint8_t)((cur->uuid[6] & 0x0F) | 0x30);
    cur->uuid[8] = (uint8_t)((cur->uuid[8] & 0x3F) | 0x80);
    /* Login Success, the one runtime-framed packet. */
    o = 1;
    buf[o++] = 0x02;
    for (i = 0; i < 16; i++) buf[o++] = cur->uuid[i];
    buf[o++] = (uint8_t)n;
    for (i = 0; i < n; i++) buf[o++] = (uint8_t)cur->pname[i];
    if (!send_small((uint16_t)(o - 1))) return;
    say(cur->pname);
    say(" is logging in\n");
    cur->st = ST_PLAY;

    /* The join burst: baked bytes, keepalives AND a drain between blobs.
     * The drain is load-bearing, not politeness: the client keeps talking
     * during the burst (settings, brand, keepalive answers), and a ring
     * that is never drained crosses "free < MSS", which STOPS uIP's window
     * — a stopped window drops segments unacked, and the daemon's NAT
     * eventually reads twenty seconds of unacked retransmissions as a dead
     * peer and RESETS the client mid-join. Found live, at chunk ~13 of 25.
     * Between whole blobs a pump is safe: anything it composes (a chat
     * echo, a Block Change) is a whole packet in a legal gap. */
    if (!send_blob(&BLOB_PRE)) return;
    if (!send_world()) return;
    if (!send_blob(&BLOB_POST)) return;
    send_chat("Welcome to a Super Nintendo Minecraft Server. It's slow.",
              "");
    say(cur->pname);
    say(" joined the world\n");
    sys_notify(); /* run in the background, and a join still gets eyes */
    return;
  }
  if (cur->st == ST_PLAY) {
    if (pid == S_CHAT) handle_chat(off);
    else if (pid == S_DIGGING) handle_digging(off);
    else if (pid == S_POSITION || pid == S_POSITION_ROT) handle_position(off);
    else if (pid == S_CLIENT_STATUS) {
      /* The client owns the Respawn button; this is it being pressed. Back
       * to the spawn the world was built around — and NOT followed by the
       * chunks, because a same-dimension respawn leaves the client's
       * terrain alone (see mkmc.respawn_blob for why, and what to change
       * if a client ever lands in a void). */
      if (rd_varint(&off) == 0) {
        /* Not gated on cur->dead: the client only shows the button when it
         * believes the player is dead, and if the two of us ever disagree
         * the player is the one stuck looking at a screen with no way out. */
        cur->dead = 0;
        if (!send_blob(&BLOB_RESPAWN)) return;
        if (!send_blob(&BLOB_POST)) return;
        say(cur->pname);
        say(" respawned\n");
      }
    }
    /* Teleport confirms, settings, plugin messages, keepalive answers and
     * the 20 Hz position spam all land here and are ignored. */
  }
}

/* Advance cur's inbound framer with whatever has arrived. */
static void pump(void) {
  uint8_t b;
  for (;;) {
    if (cur->phase == 2) {
      uint16_t n, want = sizeof buf;
      if ((uint32_t)want > cur->discard) want = (uint16_t)cur->discard;
      n = sys_net_read(cur->ch, buf, want);
      if (n == 0 || n == SYS_ERR) return;
      cur->discard -= n;
      if (cur->discard == 0) cur->phase = 0;
      continue;
    }
    if (cur->phase == 0) {
      if (sys_net_read(cur->ch, &b, 1) != 1) return;
      cur->plen |= (uint32_t)(b & 0x7F) << cur->lenshift;
      cur->lenshift += 7;
      /* A frame length needs at most 3 VarInt bytes at this protocol's
       * sizes; a longer prefix (or an absurd length) is a garbled or
       * hostile stream. Unbounded, the shift walked into undefined
       * territory and a "legal" 5-byte length parked the session
       * discarding 4 GB. Poison it; the main loop hangs up. */
      if (cur->lenshift > 28 || cur->plen > 32768ul) {
        cur->poison = 1;
        cur->poison_at = sys_frames();
        return;
      }
      if (b & 0x80) continue;
      cur->lenshift = 0;
      cur->rxgot = 0;
      if (cur->plen == 0) continue;  /* an empty frame carries nothing */
      cur->phase = 1;
      continue;
    }
    /* phase 1: collect the body, up to RXBUF of it. */
    {
      uint32_t keep = cur->plen > RXBUF ? RXBUF : cur->plen;
      uint16_t n = sys_net_read(cur->ch, cur->rx + cur->rxgot,
                                (uint16_t)(keep - cur->rxgot));
      if (n == 0 || n == SYS_ERR) return;
      cur->rxgot = (uint16_t)(cur->rxgot + n);
      if (cur->rxgot < keep) continue;
      if (cur->plen > keep) {
        cur->discard = cur->plen - keep;
        cur->phase = 2;
      } else {
        cur->phase = 0;
      }
      cur->plen = 0;
      handle_packet();               /* truncated: skippable ids only */
    }
  }
}

static void sess_reset(sess_t *s) {
  s->st = ST_HANDSHAKE;
  s->phase = 0;
  s->plen = 0;
  s->lenshift = 0;
  s->rxgot = 0;
  s->discard = 0;
  s->poison = 0;
  s->dead = 0;
}

int main(void) {
  int i, r;
  say("mc: a Minecraft server (1.16.5, one player)\n");
  if (!sys_net_up()) {
    say("no link attached\n");
    return 1;
  }
  /* Two listeners, one port: the ping connection and the login connection
   * overlap during every join, and the second SYN must land somewhere. */
  for (i = 0; i < NSESS; i++) {
    r = sys_net_listen(MC_PORT);
    if (r == SYS_ERR) {
      say("cannot listen on 25565 (no ip? port taken?)\n");
      return 1;
    }
    S[i].ch = (uint16_t)r;
    sess_reset(&S[i]);
  }
  say("listening on 25565 - 'net status' shows the address\n");
  say("any key stops\n");

  for (;;) {
    if (sys_pollkey() != SYS_ERR) break;
    for (i = 0; i < NSESS; i++) {
      uint16_t s2;
      cur = &S[i];
      s2 = sys_net_state(cur->ch);
      if (s2 == NET_ST_OPEN) {
        pump();
        s2 = sys_net_state(cur->ch);
        if (s2 == NET_ST_OPEN && cur->st == ST_PLAY &&
            (uint16_t)(sys_frames() - cur->ka_at) >= KA_FRAMES) {
          cur->ka_at = sys_frames();
          send_keepalive();
        }
      }
      if (cur->poison && (s2 == NET_ST_OPEN || s2 == NET_ST_EOF)) {
        /* End it — but not before anything we just SAID has actually gone.
         * net_hangup refuses while the send queue holds bytes or uIP has a
         * segment unacknowledged, which is exactly "the client now has our
         * Login Disconnect"; closing outright turned a polite refusal into
         * a RST that discarded the message along with the connection. The
         * port is kept, so the listener simply comes back. */
        if (sys_net_hangup(cur->ch) == 0) {
          say(cur->poison == 2 ? "disconnected them\n"
                               : "garbled client - hung up\n");
          sess_reset(cur);
        } else if ((uint16_t)(sys_frames() - cur->poison_at) > 600u) {
          /* Ten seconds and it has not taken its answer: the peer is gone
           * or not reading, and a listener must not be pinned by either. */
          uint16_t again;
          say("client will not take its answer - aborting\n");
          sys_net_close(cur->ch);
          again = sys_net_listen(MC_PORT);
          if (again == SYS_ERR) {
            /* Park the slot rather than hold SYS_ERR as a channel number
             * and ask the kernel about it forever; the other still serves. */
            say("(one listener lost - restart mc to get it back)\n");
            cur->ch = 0;             /* channel 0 is the kernel's, never ours */
          } else {
            cur->ch = again;
          }
          sess_reset(cur);
        }
        continue;
      }
      if (s2 == NET_ST_EOF || s2 == NET_ST_ERROR) {
        if (cur->st == ST_PLAY) {
          say(cur->pname);
          say(" left\n");
        }
        /* On a live EOF the hangup drains politely; on ERROR the kernel
         * abandons the corpse and CLEARS the send queue, so the next
         * client cannot inherit the middle of this one's packet. */
        if (sys_net_hangup(cur->ch) == 0) sess_reset(cur);
        else if (s2 == NET_ST_ERROR) {
          say("mc: listener lost\n");
          goto done;
        }
      }
    }
    sys_yield();
  }
done:
  for (i = 0; i < NSESS; i++) sys_net_close(S[i].ch);
  say("mc: stopped (");
  sys_putu(digs);
  say(" digs, ");
  sys_putu(chats);
  say(" chats)\n");
  return 0;
}
