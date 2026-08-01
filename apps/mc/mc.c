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

static const mc_blob_t BLOB_STATUS = MC_STATUS_RESPONSE;
static const mc_blob_t BLOB_PONG = MC_PONG;
static const mc_blob_t BLOB_PRE = MC_PLAY_PRE;
static const mc_blob_t BLOB_POST = MC_PLAY_POST;
static const mc_blob_t BLOB_KA = MC_KEEPALIVE;
static const mc_blob_t CHUNKS[MC_NCHUNKS] = MC_CHUNK_TABLE;
static const uint32_t SCULPT_ROWS[5] = MC_SCULPT_ROWS;

/* Serverbound ids this file reacts to; everything else is skipped. */
#define S_CHAT 0x03
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
  uint8_t rx[RXBUF];
  char pname[17];
  uint8_t uuid[16];
} sess_t;

static sess_t S[NSESS];
static sess_t *cur;        /* the session being pumped; handlers read it */

static uint8_t buf[80];    /* shared: runtime-composed packets, blob copies */
static uint16_t digs, chats;

static void say(const char *s) { sys_puts(s); }
static void pump(void); /* the burst drains between blobs; see there */

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
    if (n > sizeof buf) n = sizeof buf;
    if (n > left) n = left;
    for (i = 0; i < n; i++) buf[i] = src[i];
    i = sys_net_write(cur->ch, buf, n);
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
  for (p = 0; p < 2; p++) {
    const char *s = parts[p];
    while (*s && o < 55) {
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

static uint32_t rd_varint(uint16_t *off) {
  uint32_t v = 0;
  uint8_t sh = 0, b;
  do {
    b = cur->rx[*off];
    (*off)++;
    v |= (uint32_t)(b & 0x7F) << sh;
    sh += 7;
  } while ((b & 0x80) && sh < 35 && *off < RXBUF);
  return v;
}

static uint16_t world_block(int32_t x, int32_t y, int32_t z) {
  if (y == 0) return MC_B_BEDROCK;
  if (y > 0 && y < MC_SURFACE_Y) return MC_B_DIRT;
  if (y == MC_SURFACE_Y) return MC_B_GRASS;
  if (z == MC_SCULPT_Z && y >= 4 && y <= 8 &&
      x >= MC_SCULPT_X0 && x <= MC_SCULPT_X1) {
    if (SCULPT_ROWS[y - 4] & ((uint32_t)1 << (x - MC_SCULPT_X0)))
      return MC_B_STONE;
  }
  return MC_B_AIR;
}

static void handle_digging(uint16_t off) {
  uint32_t hi, lo;
  int32_t x, y, z;
  uint16_t o = off, i, state;
  uint32_t status = rd_varint(&o);
  if (status != 0 && status != 2) return;  /* only a break needs undoing */
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
  for (i = 0; i < n && o + i < RXBUF; i++) {
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

/* One serverbound packet, fully collected (or truncated: only packets we
 * skip anyway ever are). */
static void handle_packet(void) {
  uint16_t off = 0;
  uint32_t pid = rd_varint(&off);

  if (cur->st == ST_HANDSHAKE && pid == 0x00) {
    uint32_t next;
    (void)rd_varint(&off);           /* protocol; the client judges from our
                                      * status reply */
    next = rd_varint(&off);          /* server address string: skip */
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
      for (i = 0; i < BLOB_PONG.len; i++) buf[i] = src[i];
      for (i = 0; i < 8; i++) buf[MC_PONG_PAYLOAD_OFF + i] = cur->rx[off + i];
      sl_send_all(cur->ch, (const char *)buf, BLOB_PONG.len);
    }
    return;
  }
  if (cur->st == ST_LOGIN && pid == 0x00) {
    uint32_t n = rd_varint(&off);
    uint16_t i, o;
    if (n > 16) n = 16;
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
    for (i = 0; i < MC_NCHUNKS; i++) {
      if (!send_blob(&CHUNKS[i])) return;
      pump();
      if ((i & 3) == 3) {
        if (!send_keepalive()) return;
        cur->ka_at = sys_frames();
      }
    }
    if (!send_blob(&BLOB_POST)) return;
    send_chat("Welcome to a Super Nintendo. ", "The world is read-only.");
    say(cur->pname);
    say(" joined the world\n");
    sys_notify(); /* run in the background, and a join still gets eyes */
    return;
  }
  if (cur->st == ST_PLAY) {
    if (pid == S_CHAT) handle_chat(off);
    else if (pid == S_DIGGING) handle_digging(off);
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
      if (s2 == NET_ST_EOF || s2 == NET_ST_ERROR) {
        if (cur->st == ST_PLAY) {
          say(cur->pname);
          say(" left\n");
        }
        /* On a live EOF the hangup drains politely; on ERROR the kernel
         * abandons the corpse and CLEARS the send queue, so the next
         * client cannot inherit the middle of this one's packet. */
        if (sys_net_hangup(cur->ch) == 0) sess_reset(cur);
      }
    }
    sys_yield();
  }
  for (i = 0; i < NSESS; i++) sys_net_close(S[i].ch);
  say("mc: stopped (");
  sys_putu(digs);
  say(" digs, ");
  sys_putu(chats);
  say(" chats)\n");
  return 0;
}
