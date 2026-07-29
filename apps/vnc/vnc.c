/* SNOS VNC — a Super Nintendo as a thin client.
 *
 *     vnc HOST[:PORT]        default port 5900
 *
 *     arrow keys   move the cursor
 *     space        click
 *     ESC          quit
 *
 * This is a real RFB client. The console does the protocol itself — handshake,
 * pixel format negotiation, framebuffer updates, pointer events — and nothing
 * on the far side knows or cares that it is talking to a SNES.
 *
 * ------------------------------------------------------------- why VNC fits
 *
 * Three things make RFB a better match for this machine than anything we tried
 * before it:
 *
 * 1. THE CLIENT PICKS THE PIXEL FORMAT. We ask for 8 bits per pixel, true
 *    colour, R:3 G:3 B:2, and preload CGRAM with exactly that cube — so an
 *    incoming pixel byte IS a palette index. Nothing converts anything. The
 *    browser needed Pillow in the middle; this needs nobody.
 *
 *    RFB's colour-map mode (true-colour=0) maps onto Mode 3 even more directly,
 *    but it is barely exercised in modern servers and some refuse it. A fixed
 *    3:3:2 cube gets the same result from a format everybody tests, with no
 *    SetColourMapEntries handling and a palette that never changes mid-session.
 *
 * 2. INCREMENTAL UPDATES ARE THE PROTOCOL, not something we had to invent. A
 *    still screen sends zero bytes — measured against a real server, not
 *    assumed.
 *
 * 3. CopyRect says "this rectangle is already on screen somewhere else", which
 *    makes scrolling nearly free.
 *
 * ----------------------------------------------------------- the honest cost
 *
 * The SNES stores 8bpp as EIGHT BITPLANES, and RFB delivers linear bytes, so
 * every pixel has to be transposed. That sounds fatal and is not: the link
 * moves ~615 B/s while the transpose runs at roughly a screen per two seconds,
 * so the wire is thirty to a hundred times slower than the conversion. Even at
 * M7's 20 KB/s they are comparable. The CPU was never the problem.
 *
 * What it does cost is memory. Server rectangles are NOT tile-aligned — a real
 * server sent `123x21 at (133,104)` — so a rectangle lands across partial
 * tiles, and a tile cannot be rebuilt from a fragment. So the client keeps a
 * linear shadow of the framebuffer, writes rectangles into that, and rebuilds
 * whole tiles from it. 256x192 is 48 KB, which is why this lives in the app
 * arena in bank $7F rather than anywhere near bank $00's eight.
 */
#include "snos_abi.h"

#define FB_W 256
#define FB_H 192
/* Spelled out rather than FB_W * FB_H: int is 16 bits here, so the product
 * overflows to a negative array bound and the compiler rejects it. */
#define FB_SIZE 49152u
#define TILES_W (FB_W / 8)
#define TILES_H (FB_H / 8)
#define NTILES (TILES_W * TILES_H)

/* VRAM, laid out so nothing overlaps — the same arithmetic `web` uses:
 *   $0000-$BFFF  768 image tiles, 8bpp
 *   $C000-$DFFF  cursor sprite (OBSEL base 3)
 *   $E000-$E7FF  BG1 tilemap
 */
#define VRAM_OBJ 0xC000
#define VRAM_MAP 0xE000

#define R8(a) (*(volatile __far uint8_t *)(uint32_t)(a))

/* RFB server -> client message types */
#define MSG_UPDATE 0
#define MSG_COLOURMAP 1
#define MSG_BELL 2
#define MSG_CUT 3

#define ENC_RAW 0
#define ENC_COPYRECT 1

static char args[SNOS_ARGV_MAX];
static uint8_t fb[FB_SIZE];       /* linear shadow, bank $7F */
static uint8_t dirty[NTILES];
static uint16_t tile[32];         /* one tile, as PPU words */
static uint8_t hdr[32];
static uint16_t ch;
static uint16_t cx, cy;
static uint16_t srv_w, srv_h;
static int quit;

static void say(const char *s) { sys_puts(s); }

/* Defined below, but called from rd(): the read loop services the cursor so a
 * long transfer stays responsive. */
static int poll_input(void);

static void vaddr(uint16_t w) {
  R8(0x2116) = (uint8_t)w;
  R8(0x2117) = (uint8_t)(w >> 8);
}

/* ------------------------------------------------------------------- stream */

/* Read exactly n bytes, or fail. RFB is a framed protocol on a byte stream:
 * every message has a known length and reading "whatever arrived" would
 * desynchronise it permanently. */
static int rd(uint8_t *dst, uint16_t n) {
  uint16_t got = 0;
  while (got < n) {
    uint16_t k;
    /* Every pass, not only when starved. During a 48 KB transfer bytes are
     * almost always waiting, so the starvation branch below never runs — which
     * meant the cursor and ESC were ignored for the whole eighty seconds. */
    if (poll_input()) return 0;
    k = sys_net_read(ch, dst + got, (uint16_t)(n - got));
    if (k > 0 && k != SYS_ERR) {
      got = (uint16_t)(got + k);
      continue;
    }
    if (sys_net_state(ch) != NET_ST_OPEN && sys_net_state(ch) != NET_ST_EOF)
      return 0;
    if (poll_input()) return 0;
    sys_yield();
  }
  return 1;
}

static int wr(const uint8_t *src, uint16_t n) {
  uint16_t sent = 0;
  while (sent < n) {
    uint16_t k = sys_net_write(ch, (const char *)(src + sent),
                               (uint16_t)(n - sent));
    if (k == 0) {
      if (sys_net_state(ch) != NET_ST_OPEN && sys_net_state(ch) != NET_ST_EOF)
        return 0;
      sys_yield();
      continue;
    }
    sent = (uint16_t)(sent + k);
  }
  return 1;
}

static uint16_t be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

/* ---------------------------------------------------------------- handshake */

static int handshake(void) {
  uint16_t n, i;

  if (!rd(hdr, 12)) return 0;             /* "RFB 003.008\n" */
  if (hdr[0] != 'R' || hdr[1] != 'F' || hdr[2] != 'B') {
    say("not an RFB server\n");
    return 0;
  }
  if (!wr((const uint8_t *)"RFB 003.008\n", 12)) return 0;

  if (!rd(hdr, 1)) return 0;
  n = hdr[0];
  if (n == 0) {
    say("server refused the connection\n");
    return 0;
  }
  if (n > sizeof hdr) n = sizeof hdr;
  if (!rd(hdr, n)) return 0;
  for (i = 0; i < n; i++)
    if (hdr[i] == 1) break;
  if (i == n) {
    say("server wants authentication; only 'None' is supported\n");
    return 0;
  }
  hdr[0] = 1;
  if (!wr(hdr, 1)) return 0;              /* security type: None */
  if (!rd(hdr, 4)) return 0;              /* SecurityResult */
  if (hdr[0] || hdr[1] || hdr[2] || hdr[3]) {
    say("server rejected 'None' authentication\n");
    return 0;
  }

  hdr[0] = 1;
  if (!wr(hdr, 1)) return 0;              /* ClientInit: share the desktop */

  if (!rd(hdr, 24)) return 0;             /* ServerInit: size + pixel format */
  srv_w = be16(hdr);
  srv_h = be16(hdr + 2);
  {
    uint32_t namelen = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) |
                       ((uint32_t)hdr[22] << 8) | hdr[23];
    while (namelen) {
      uint16_t take = (namelen > sizeof hdr) ? (uint16_t)sizeof hdr
                                             : (uint16_t)namelen;
      if (!rd(hdr, take)) return 0;
      namelen -= take;
    }
  }

  /* SetPixelFormat: 8bpp, true colour, R:3 G:3 B:2. This is the message that
   * makes the whole app possible — after it, a pixel byte is a CGRAM index. */
  {
    uint8_t m[20];
    for (i = 0; i < 20; i++) m[i] = 0;
    m[0] = 0;                 /* message type */
    m[4] = 8;                 /* bits-per-pixel */
    m[5] = 8;                 /* depth */
    m[6] = 0;                 /* little-endian */
    m[7] = 1;                 /* true-colour */
    put16(m + 8, 7);          /* red-max   */
    put16(m + 10, 7);         /* green-max */
    put16(m + 12, 3);         /* blue-max  */
    m[14] = 5;                /* red-shift   */
    m[15] = 2;                /* green-shift */
    m[16] = 0;                /* blue-shift  */
    if (!wr(m, 20)) return 0;
  }

  /* SetEncodings. CopyRect first: preference order is meaningful, and a
   * scroll expressed as "copy that rectangle" costs four bytes instead of a
   * screenful. */
  {
    uint8_t m[12];
    m[0] = 2;
    m[1] = 0;
    put16(m + 2, 2);
    m[4] = 0; m[5] = 0; m[6] = 0; m[7] = ENC_COPYRECT;
    m[8] = 0; m[9] = 0; m[10] = 0; m[11] = ENC_RAW;
    if (!wr(m, 12)) return 0;
  }
  return 1;
}

static int request(int incremental) {
  uint8_t m[10];
  m[0] = 3;
  m[1] = (uint8_t)(incremental ? 1 : 0);
  put16(m + 2, 0);
  put16(m + 4, 0);
  put16(m + 6, FB_W);
  put16(m + 8, FB_H);
  return wr(m, 10);
}

static int pointer(uint16_t x, uint16_t y, uint8_t mask) {
  uint8_t m[6];
  m[0] = 5;
  m[1] = mask;
  put16(m + 2, x);
  put16(m + 4, y);
  return wr(m, 6);
}

/* ------------------------------------------------------------------ display */

/* CGRAM as the 3:3:2 cube the server is now sending indices into. Built here
 * rather than shipped as a table because the arithmetic is shorter than the
 * 512 bytes it would occupy. */
static void load_palette(void) {
  uint16_t i;
  R8(0x2121) = 0;
  for (i = 0; i < 256; i++) {
    uint16_t r = (uint16_t)((i >> 5) & 7);
    uint16_t g = (uint16_t)((i >> 2) & 7);
    uint16_t b = (uint16_t)(i & 3);
    /* 3 bits -> 5 by replicating the top bits, which keeps white at white. */
    uint16_t r5 = (uint16_t)((r << 2) | (r >> 1));
    uint16_t g5 = (uint16_t)((g << 2) | (g >> 1));
    uint16_t b5 = (uint16_t)((b << 3) | (b << 1) | (b >> 1));
    uint16_t v = (uint16_t)(r5 | (g5 << 5) | (b5 << 10));
    R8(0x2122) = (uint8_t)v;
    R8(0x2122) = (uint8_t)(v >> 8);
  }
}

static const uint8_t ARROW[8] = {0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0x90, 0x10};
static const uint8_t EDGE[8] = {0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFC, 0xF8, 0x38};

static void load_cursor(void) {
  int i;
  vaddr(VRAM_OBJ >> 1);
  for (i = 0; i < 8; i++) {
    uint16_t w = (uint16_t)(ARROW[i] | ((uint16_t)(EDGE[i] & ~ARROW[i]) << 8));
    R8(0x2118) = (uint8_t)w;
    R8(0x2119) = (uint8_t)(w >> 8);
  }
  for (i = 0; i < 8; i++) {
    R8(0x2118) = 0;
    R8(0x2119) = 0;
  }
  /* Sprite palette 0 lives at CGRAM 128. The framebuffer owns all 256 entries,
   * so two of them are spent on a cursor that is always visible — a fair trade
   * against hunting for a grey arrow on a grey window. */
  R8(0x2121) = 129;
  R8(0x2122) = 0xFF; R8(0x2122) = 0x7F;   /* white */
  R8(0x2122) = 0x00; R8(0x2122) = 0x00;   /* black */
}

static void place_cursor(void) {
  R8(0x2102) = 0;
  R8(0x2103) = 0;
  R8(0x2104) = (uint8_t)cx;
  R8(0x2104) = (uint8_t)(cy ? cy - 1 : 0);
  R8(0x2104) = 0x00;
  R8(0x2104) = 0x30;
  R8(0x2102) = 0x00;
  R8(0x2103) = 0x01;
  R8(0x2104) = (uint8_t)((cx > 255) ? 0x01 : 0x00);
}

static void hide_sprites(void) {
  int i;
  R8(0x2102) = 0;
  R8(0x2103) = 0;
  for (i = 0; i < 128; i++) {
    R8(0x2104) = 0;
    R8(0x2104) = 0xF0;
    R8(0x2104) = 0;
    R8(0x2104) = 0;
  }
}

static void setup_ppu(void) {
  uint16_t r, c;

  /* Identity tilemap: tile (r,c) shows tile number r*32+c, so the tile grid is
   * the pixel grid and a shadow offset converts to a tile index by shifting. */
  vaddr(VRAM_MAP >> 1);
  for (r = 0; r < 32; r++) {
    for (c = 0; c < 32; c++) {
      uint16_t t = (r < TILES_H) ? (uint16_t)(r * TILES_W + c) : 0;
      R8(0x2118) = (uint8_t)t;
      R8(0x2119) = (uint8_t)(t >> 8);
    }
  }

  R8(0x2105) = 0x03;                             /* Mode 3, BG1 8bpp */
  R8(0x210D) = 0; R8(0x210D) = 0;
  R8(0x210E) = 0; R8(0x210E) = 0;
  R8(0x2107) = (uint8_t)((VRAM_MAP >> 10) << 2); /* BG1SC */
  R8(0x210B) = 0x00;                             /* BG1 tiles at $0000 */
  R8(0x2101) = 0x03;                             /* OBSEL: 8x8, base $C000 */
  R8(0x212C) = 0x11;                             /* BG1 + OBJ */
  R8(0x212D) = 0x00;
  R8(0x2100) = 0x0F;
}

/* Rebuild one tile from the shadow and push it to VRAM.
 *
 * This is the transpose: the shadow is linear bytes, the PPU wants eight
 * bitplanes, and a tile is 8 rows of 8 pixels producing 32 words in the order
 * (planes 0&1) x8, (planes 2&3) x8, (4&5) x8, (6&7) x8. */
static void flush_tile(uint16_t t) {
  uint16_t tx = (uint16_t)(t % TILES_W), ty = (uint16_t)(t / TILES_W);
  uint16_t base = (uint16_t)(ty * 8u * FB_W + tx * 8u);
  uint16_t r, i;

  for (r = 0; r < 8; r++) {
    uint8_t p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0;
    uint16_t row = (uint16_t)(base + r * FB_W);
    for (i = 0; i < 8; i++) {
      uint8_t v = fb[row + i];
      uint8_t bit = (uint8_t)(0x80u >> i);
      if (v & 0x01) p0 = (uint8_t)(p0 | bit);
      if (v & 0x02) p1 = (uint8_t)(p1 | bit);
      if (v & 0x04) p2 = (uint8_t)(p2 | bit);
      if (v & 0x08) p3 = (uint8_t)(p3 | bit);
      if (v & 0x10) p4 = (uint8_t)(p4 | bit);
      if (v & 0x20) p5 = (uint8_t)(p5 | bit);
      if (v & 0x40) p6 = (uint8_t)(p6 | bit);
      if (v & 0x80) p7 = (uint8_t)(p7 | bit);
    }
    tile[r] = (uint16_t)(p0 | ((uint16_t)p1 << 8));
    tile[8 + r] = (uint16_t)(p2 | ((uint16_t)p3 << 8));
    tile[16 + r] = (uint16_t)(p4 | ((uint16_t)p5 << 8));
    tile[24 + r] = (uint16_t)(p6 | ((uint16_t)p7 << 8));
  }

  vaddr((uint16_t)(t * 32u));
  for (i = 0; i < 32; i++) {
    R8(0x2118) = (uint8_t)tile[i];
    R8(0x2119) = (uint8_t)(tile[i] >> 8);
  }
}

static void flush_dirty(void) {
  uint16_t t;
  /* Re-assert VMAIN on every flush, not once at startup. Anything that runs
   * between two flushes — and sys_yield() reaches the console and the keyboard
   * panel — can leave the VRAM increment mode somewhere else, and the result is
   * scrambled bitplanes on whichever tiles were written afterwards: scattered
   * noise on an otherwise black screen. One register write is cheaper than
   * reasoning about who else might have touched it. */
  R8(0x2115) = 0x80;
  /* Forced blank: VRAM takes writes only while blanking, and a full screen is
   * far more than one vblank's worth. The screen goes dark for the duration,
   * which on a first update is the honest cost of 48 KB arriving. */
  R8(0x2100) = 0x8F;
  for (t = 0; t < NTILES; t++) {
    if (!dirty[t]) continue;
    flush_tile(t);
    dirty[t] = 0;
  }
  R8(0x2100) = 0x0F;
}

static void mark(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint16_t tx0 = (uint16_t)(x / 8), ty0 = (uint16_t)(y / 8);
  uint16_t tx1 = (uint16_t)((x + w + 7) / 8), ty1 = (uint16_t)((y + h + 7) / 8);
  uint16_t r, c;
  if (tx1 > TILES_W) tx1 = TILES_W;
  if (ty1 > TILES_H) ty1 = TILES_H;
  for (r = ty0; r < ty1; r++)
    for (c = tx0; c < tx1; c++) dirty[r * TILES_W + c] = 1;
}

/* Cursor and quit, serviced while a transfer is in flight.
 *
 * This has to run from inside the read loop, not only between updates. A first
 * screen is 48 KB — eighty seconds at 615 B/s — and an application that only
 * checks the keyboard between whole updates ignores you for eighty seconds. The
 * cursor is a sprite, so moving it mid-transfer costs four bytes of OAM and no
 * network traffic at all.
 *
 * Returns 1 if the user asked to leave. */
static int poll_input(void) {
  for (;;) {
    int c = (int)sys_pollkey();
    if (c == (int)SYS_ERR) return 0;
    if (c == 0x1B) {
      quit = 1;
      return 1;
    }
    if (c == SNOS_KEY_LEFT) { cx = (uint16_t)(cx >= 8 ? cx - 8 : 0); }
    else if (c == SNOS_KEY_RIGHT) { cx = (uint16_t)(cx < FB_W - 8 ? cx + 8 : FB_W - 1); }
    else if (c == SNOS_KEY_UP) { cy = (uint16_t)(cy >= 8 ? cy - 8 : 0); }
    else if (c == SNOS_KEY_DOWN) { cy = (uint16_t)(cy < FB_H - 8 ? cy + 8 : FB_H - 1); }
    else if (c == ' ') { pointer(cx, cy, 1); pointer(cx, cy, 0); continue; }
    else continue;
    place_cursor();
  }
}

/* ------------------------------------------------------------------ updates */

static int read_update(void) {
  uint16_t nrects, i;

  if (!rd(hdr, 3)) return 0;               /* padding + rect count */
  nrects = be16(hdr + 1);

  for (i = 0; i < nrects; i++) {
    uint16_t x, y, w, h, row;
    int32_t enc;
    if (!rd(hdr, 12)) return 0;
    x = be16(hdr);
    y = be16(hdr + 2);
    w = be16(hdr + 4);
    h = be16(hdr + 6);
    enc = (int32_t)(((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                    ((uint32_t)hdr[10] << 8) | hdr[11]);

    if (x >= FB_W || y >= FB_H) return 0;  /* a rect we cannot place */
    if ((uint16_t)(x + w) > FB_W) w = (uint16_t)(FB_W - x);
    if ((uint16_t)(y + h) > FB_H) h = (uint16_t)(FB_H - y);

    if (enc == ENC_RAW) {
      /* Straight into the shadow, a row at a time. No intermediate buffer:
       * a single rectangle can be 26 KB and there is nowhere to put it. */
      for (row = 0; row < h; row++)
        if (!rd(&fb[(uint16_t)((y + row) * FB_W + x)], w)) return 0;
      mark(x, y, w, h);
      /* Paint it NOW, not after the whole update. A server may split one screen
       * across several messages, so waiting meant painting a fraction of the
       * screen with stale VRAM — the console's own font — showing through the
       * rest. That was the corruption. It also turns an eighty-second first
       * screen into something you can watch arrive. */
      flush_dirty();
    } else if (enc == ENC_COPYRECT) {
      uint16_t sx, sy;
      if (!rd(hdr, 4)) return 0;
      sx = be16(hdr);
      sy = be16(hdr + 2);
      for (row = 0; row < h; row++) {
        uint16_t d = (uint16_t)((y + row) * FB_W + x);
        uint16_t s = (uint16_t)((sy + row) * FB_W + sx);
        uint16_t k;
        /* Direction matters when the regions overlap, which is exactly what a
         * scroll is. */
        if (d > s)
          for (k = w; k--;) fb[d + k] = fb[s + k];
        else
          for (k = 0; k < w; k++) fb[d + k] = fb[s + k];
      }
      mark(x, y, w, h);
    } else {
      /* We advertised only Raw and CopyRect; anything else means the stream is
       * no longer parseable and guessing would make it worse. */
      say("\nserver used an encoding we did not ask for\n");
      return 0;
    }
  }
  return 1;
}

/* Read one server message, handling the ones that are not updates. */
static int pump(void) {
  if (!rd(hdr, 1)) return 0;
  if (hdr[0] == MSG_UPDATE) return read_update();
  if (hdr[0] == MSG_BELL) return 1;
  if (hdr[0] == MSG_COLOURMAP) {
    uint16_t n;
    if (!rd(hdr, 5)) return 0;
    n = be16(hdr + 3);
    while (n--) if (!rd(hdr, 6)) return 0;
    return 1;
  }
  if (hdr[0] == MSG_CUT) {
    uint32_t n;
    if (!rd(hdr, 7)) return 0;
    n = ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[4] << 16) |
        ((uint32_t)hdr[5] << 8) | hdr[6];
    while (n) {
      uint16_t take = (n > sizeof hdr) ? (uint16_t)sizeof hdr : (uint16_t)n;
      if (!rd(hdr, take)) return 0;
      n -= take;
    }
    return 1;
  }
  say("\nunknown message from the server\n");
  return 0;
}

/* ---------------------------------------------------------------- the loop */

int main(void) {
  uint16_t i;
  int first = 1;

  cx = FB_W / 2;
  cy = FB_H / 2;
  quit = 0;

  snos_getargs(args, sizeof args);
  if (args[0] == 0) {
    say("usage: vnc HOST[:PORT]\n");
    return 1;
  }
  if (!sys_net_up()) {
    say("no gateway attached\n");
    return 1;
  }

  /* Default the port here rather than making the user type 5900 every time. */
  {
    int has_port = 0;
    for (i = 0; args[i]; i++)
      if (args[i] == ':') has_port = 1;
    if (!has_port) {
      const char *p = ":5900";
      while (*p && i < SNOS_ARGV_MAX - 1) args[i++] = *p++;
      args[i] = 0;
    }
  }

  say("connecting to ");
  say(args);
  sys_putc('\n');

  ch = sys_net_connect(args);
  if (ch == SYS_ERR) {
    say("no free channel\n");
    return 1;
  }
  {
    uint16_t deadline = (uint16_t)(sys_frames() + 1800u);
    while (sys_net_state(ch) == NET_ST_OPENING) {
      if ((int16_t)(sys_frames() - deadline) >= 0) {
        say("connection timed out\n");
        sys_net_close(ch);
        return 1;
      }
      sys_yield();
    }
  }
  if (sys_net_state(ch) != NET_ST_OPEN) {
    say("could not connect\n");
    sys_net_close(ch);
    return 1;
  }

  if (!handshake()) {
    sys_net_close(ch);
    return 1;
  }

  say("connected: ");
  sys_putu(srv_w);
  sys_putc('x');
  sys_putu(srv_h);
  if (srv_w != FB_W || srv_h != FB_H) {
    /* Say so rather than silently showing a corner. A server sized to the
     * console shows its whole desktop; anything else is cropped. */
    say(" (showing the top-left ");
    sys_putu(FB_W);
    sys_putc('x');
    sys_putu(FB_H);
    sys_putc(')');
  }
  say("\nESC quits\n");

  /* Start from a known screen: clear the shadow and paint every tile once, so
   * anything the server has not sent yet is black rather than whatever the
   * console left in VRAM. "Not arrived" then looks like not arrived. */
  /* One index, no multiply. The nested version of this guarded against a 16-bit
   * overflow that cannot happen — FB_SIZE is 49152 and a uint16_t reaches 65535
   * — and paid for it with a runtime multiply per byte, because the 65816 has no
   * multiply instruction. Forty-nine thousand calls to a software multiply is a
   * hang from the outside: it never reached the next print. */
  {
    uint16_t k;
    for (k = 0; k < FB_SIZE; k++) fb[k] = 0;
  }
  for (i = 0; i < NTILES; i++) dirty[i] = 1;

  say("receiving the first screen - about 80s on this link\n");
  say("it paints as it arrives; ESC quits\n");
  sys_gfx(1);
  R8(0x2100) = 0x8F;
  /* Increment the VRAM address after the HIGH byte, so a low/high pair writes
   * one word. Without this the console's last setting applies, the address
   * advances after the LOW byte, and every word is split across two addresses —
   * which paints the console's own font back at you, scrambled. `view` has
   * always set this; copying its VRAM layout without its VMAIN was the bug. */
  R8(0x2115) = 0x80;
  /* Stop all HDMA before touching the display.
   *
   * The keyboard panel drives HDMA on channels 3/4/5 and rewrites the BG
   * tilemap-base and scroll registers EVERY SCANLINE. Left running, the PPU
   * fetches tilemap entries from the wrong address part-way down the frame, so
   * arbitrary tile indices select arbitrary tile data and the screen fills with
   * scattered noise that flickers as HDMA re-triggers — on top of a picture that
   * is otherwise perfectly correct in VRAM.
   *
   * sys_gfx(1) is supposed to tear the split down, and under the test runner it
   * does, because no keys are bound there and the panel never existed. That is
   * exactly the configuration this was verified in, and exactly why the failure
   * only ever appeared on Ricardo's screen. One register write costs nothing and
   * does not depend on that teardown having happened. */
  R8(0x420C) = 0x00;
  load_palette();
  hide_sprites();
  load_cursor();
  place_cursor();
  setup_ppu();
  flush_dirty();

  while (!quit) {
    if (!request(first ? 0 : 1)) break;
    first = 0;
    if (!pump()) break;
    flush_dirty();

    /* Drain the keys that arrived while a screenful was being read. The cursor
     * moves locally with no round trip, which is the whole reason it is a
     * sprite — at 615 B/s a server-drawn cursor would lag by a second. */
    for (;;) {
      int c = (int)sys_pollkey();
      if (c == (int)SYS_ERR) break;
      if (c == 0x1B) {
        quit = 1;
        break;
      }
      if (c == SNOS_KEY_LEFT) { cx = (uint16_t)(cx >= 8 ? cx - 8 : 0); }
      else if (c == SNOS_KEY_RIGHT) { cx = (uint16_t)(cx < FB_W - 8 ? cx + 8 : FB_W - 1); }
      else if (c == SNOS_KEY_UP) { cy = (uint16_t)(cy >= 8 ? cy - 8 : 0); }
      else if (c == SNOS_KEY_DOWN) { cy = (uint16_t)(cy < FB_H - 8 ? cy + 8 : FB_H - 1); }
      else if (c == ' ') {
        /* Press and release at the cursor. The server moves its own pointer
         * there as a side effect, which is why we do not stream every cursor
         * step: each one would cost a framebuffer update of the area it left. */
        pointer(cx, cy, 1);
        pointer(cx, cy, 0);
      }
      place_cursor();
    }
  }

  R8(0x2100) = 0x8F;
  sys_gfx(0);
  sys_net_close(ch);
  say("\n");
  return 0;
}
