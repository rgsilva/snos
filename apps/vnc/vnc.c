/* SNOS VNC — a Super Nintendo as a thin client.
 *
 *     vnc HOST[:PORT]        default port 5900
 *
 *     arrow keys   move the cursor
 *     W A S D      pan the window over a larger desktop
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
 * ...and a fourth, which is the whole of the panning below:
 *
 * 4. FramebufferUpdateRequest CARRIES A RECTANGLE. "Send me this part of your
 *    desktop" is already the protocol's own idea, so a console that shows a
 *    window into something bigger than itself needs no extension, no server
 *    cooperation and no new message. It just asks for the part it is looking at.
 *
 * ----------------------------------------------------------- the honest cost
 *
 * The SNES stores 8bpp as EIGHT BITPLANES, and RFB delivers linear bytes, so
 * every pixel has to be transposed. That is affordable but not free, and the
 * figure that used to be here — "roughly a screen per two seconds" — was an
 * estimate and was wrong by an order of magnitude. MEASURED: rebuilding all 768
 * tiles takes about 1015 frames, so seventeen seconds a screen, or 1.3 frames a
 * tile at -O0.
 *
 * THE WIRE IS NO LONGER THE BOTTLENECK, and that is a change since this was
 * written. It used to need eighty seconds for that screen at 615 B/s; the link
 * work between then and now took it to about 3.5 KB/s, and a first screen
 * measured end to end against TigerVNC through the real gateway, paced to 60 Hz,
 * is THIRTY seconds — roughly seventeen of transpose and fourteen of wire. So
 * the transpose is now the larger half, and hand-writing it in 65816 is the next
 * lever rather than a someday one. Panning feels this directly: half of a pan
 * step is spent transposing 96 tiles.
 *
 * What it also costs is memory. Server rectangles are NOT tile-aligned — a real
 * server sent `123x21 at (133,104)` — so a rectangle lands across partial
 * tiles, and a tile cannot be rebuilt from a fragment. So the client keeps a
 * linear shadow of the framebuffer, writes rectangles into that, and rebuilds
 * whole tiles from it. 256x192 is 48 KB, which is why this lives in the app
 * arena in bank $7F rather than anywhere near bank $00's eight.
 *
 * ------------------------------------------------------------------ panning
 *
 * A 256-pixel-wide X server makes Chromium lay a page out at 256 CSS pixels,
 * and the text wraps into a column too narrow to read. The server is therefore
 * 512x384 (tools/vncbox/start.sh) and the console shows a 256x192 WINDOW into
 * it, moved with WASD. Same glyphs at the same size, four times the page in
 * reach. Downscaling a wider render instead was tried on paper and rejected:
 * 4-pixel glyphs are a grey smear.
 *
 * THE SHADOW STAYS VIEWPORT-SIZED. It has to: 48 KB already fills most of bank
 * $7F, and 512x384 would be 196 KB against a 64 KB VRAM with 48 KB of it in
 * tile data. So panning is not "scroll a bigger picture", it is "ask the server
 * for the rectangle you are now looking at" — which is one field of a message
 * RFB already has.
 *
 * The part that decides whether it feels usable is what panning does NOT do.
 * The naive version re-requests the whole 48 KB window: fourteen seconds at the
 * measured ~3.5 KB/s, which reads as broken. Instead:
 *
 *   THE SHADOW IS A TORUS. Server pixel (sx, sy) always lives at shadow byte
 *   (sx mod 256, sy mod 192), whatever the pan origin is. Move the window by
 *   32 pixels and the 224 columns you keep DO NOT MOVE — they are already at
 *   the right shadow address, so not one pixel is copied and not one tile is
 *   rebuilt. The 32 columns that scrolled off occupy exactly the shadow bytes
 *   the 32 newly exposed columns need, so the slots recycle themselves.
 *
 *   THE TILEMAP TURNS THE TORUS BACK INTO A PICTURE. Tile indices are
 *   arbitrary, so map cell (r, c) is pointed at tile ((py/8 + r) mod 24,
 *   (px/8 + c) mod 32) instead of at (r, c). That is 768 tilemap entries —
 *   1.5 KB, three DMAs, four frames — against 48 KB of tile data.
 *
 * PAN_STEP MUST STAY A MULTIPLE OF 8 or all of that evaporates: a pan that is
 * not a whole number of tiles cannot be expressed in the tilemap at all, and
 * every tile on screen would have to be rebuilt.
 *
 * What is left to pay for is the strip you have never seen: 32x192 is 6 KB, one
 * non-incremental FramebufferUpdateRequest, and 96 tiles to transpose. The
 * steady state stays incremental, so a still screen still sends nothing.
 *
 * Two coordinate rules fall out of this, and both are the kind that look
 * correct until you pan:
 *
 *   - INCOMING RECTANGLES ARE IN SERVER COORDINATES. They have to be translated
 *     by the pan origin and clipped to the window before they touch the shadow.
 *     While the server was exactly console-sized those were the same numbers,
 *     so the old code could — and did — treat one as the other.
 *
 *   - POINTER EVENTS GO OUT IN SERVER COORDINATES: pan origin plus cursor. Get
 *     that wrong and space clicks the right link until the first pan and the
 *     wrong one after it, which reads as a regression rather than an omission.
 *
 * ------------------------------------------------- when it is allowed to draw
 *
 * The one thing here that is genuinely counter-intuitive: AN APPLICATION ON
 * THIS MACHINE HAS NO VBLANK. NMI moves a link frame on every one of them while
 * an app owns the PPU, and at -O0 that runs from the top of the window into the
 * middle of the next frame. So every VRAM and OAM write in this file happens
 * inside a forced blank over scanlines 192-223 — the strip below the picture,
 * which is deliberately black — rather than in a vblank that does not exist. See
 * "the write window" below; it answers both of the bugs this app has had.
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

/* How far one WASD press moves the window, in server pixels.
 *
 * MUST BE A MULTIPLE OF 8, and 32 is deliberately four whole tiles. See
 * "panning" above: the retained part of the screen is moved by rewriting the
 * tilemap, and a tilemap can only express a whole-tile shift. A step of, say,
 * 20 would silently fall back to rebuilding all 768 tiles — seventeen seconds —
 * with nothing but the feel of it to tell you. */
#define PAN_STEP 32

/* VRAM, laid out so nothing overlaps — the same arithmetic `web` uses:
 *   $0000-$BFFF  768 image tiles, 8bpp
 *   $C000-$DFFF  cursor sprite (OBSEL base 3)
 *   $E000-$E7FF  BG1 tilemap
 *   $F000-$F03F  one all-zero tile, for the display below the picture
 */
#define VRAM_OBJ 0xC000
#define VRAM_MAP 0xE000
#define VRAM_BLANK 0xF000
/* 960 — and a tilemap entry's index field is 10 bits, so it reaches. */
#define TILE_BLANK (VRAM_BLANK / 64)

#define R8(a) (*(volatile __far uint8_t *)(uint32_t)(a))

/* RFB server -> client message types */
#define MSG_UPDATE 0
#define MSG_COLOURMAP 1
#define MSG_BELL 2
#define MSG_CUT 3

#define ENC_RAW 0
#define ENC_COPYRECT 1

/* How many tiles are staged in WRAM and offered to one write window.
 *
 * Eight, and eight is generous. console.h explains why byte-counting a budget
 * gives an answer ten times too big — the register setup costs about nine times
 * the transfer at -O0, so transfers per window are scarce, not bytes.
 * Then the window here is smaller again than the console's; see below.
 *
 * The binding constraint is the other way round anyway: the transpose runs at
 * about 1.3 frames per tile, so the display need only move ONE tile a frame to
 * stay ahead of the thing feeding it. Eight small transfers are also cheaper to
 * abandon halfway than one big coalesced one, which is what the first attempt
 * tried and what never fitted.
 *
 * It is also, not by accident, 8 x 32 words = exactly eight tilemap ROWS, which
 * is what write_tilemap() stages a band of the map in. */
#define BATCH 8

static char args[SNOS_ARGV_MAX];
static uint8_t fb[FB_SIZE];       /* linear shadow, bank $7F — a torus, see above */
static uint8_t dirty[NTILES];
static uint16_t stage[BATCH][32]; /* tiles as PPU words, built before a window */
static uint16_t stage_t[BATCH];   /* which tile each staged entry is */
static uint8_t hdr[32];
static uint8_t junk[64];          /* somewhere to drop bytes we are clipping away */
static uint8_t crow[FB_W];        /* one row, for CopyRect across the torus seam */
static uint16_t ch;
static uint16_t cx, cy;
static uint16_t srv_w, srv_h;
static int quit;

/* The pan origin: which server pixel the top-left of the console shows.
 *
 * px/py are absolute server coordinates and always multiples of 8. pxm/pym are
 * the same thing reduced modulo the shadow, which is what the torus arithmetic
 * actually uses — kept alongside rather than recomputed, because it is needed
 * per rectangle row and the 65816 has no divide either. */
static uint16_t px, py, pxm, pym;
static uint16_t maxpx, maxpy;
static int16_t pend_dx, pend_dy;  /* pan asked for but not yet safe to apply */
static int parsing;               /* 1 while a server message is half-read */
static int ready;                 /* 1 once the app owns the PPU and has set it up */

/* One pending non-incremental request, as a bounding box in SERVER coordinates.
 * Panning puts the newly exposed strip here; a CopyRect whose source has fallen
 * outside the window puts the part it could not satisfy here. */
static uint16_t rqx, rqy, rqw, rqh;
static int rq_pending;

static void say(const char *s) { sys_puts(s); }

/* Defined below, but called from rd(): the read loop services the cursor so a
 * long transfer stays responsive. */
static int poll_input(void);

static void vaddr(uint16_t w) {
  R8(0x2116) = (uint8_t)w;
  R8(0x2117) = (uint8_t)(w >> 8);
}

/* --------------------------------------------------------- the write window
 *
 * THERE IS NO USABLE VBLANK HERE, and that is measured, not assumed. It is the
 * most surprising thing about an application that paints and downloads at the
 * same time on this machine, so it is worth stating plainly:
 *
 *   NMI runs net_service on EVERY frame while an application owns the PPU. The
 *   kernel defers it only for the console — con_dirty_pending() returns 0 while
 *   gfx_owned, deliberately, and `vnc` is the very app whose link died in the
 *   handshake when it did not. NMI fires at the top of vblank, and at -O0 that
 *   handler runs until about scanline 100 of the NEXT frame.
 *
 * So a foreground that waits for vblank is handed a window that has already
 * gone. The first attempt at fixing the flicker did exactly that, and
 * `make test-vnc` caught it: it logs the scanline of every DMA, every one landed
 * between 104 and 122 — the middle of the picture — and the vblank check failed
 * on 96 batches out of 96. con_flush's ten transfers per window are ten only
 * because the kernel gets NMI out of its own way first, and an app cannot.
 *
 * So the app takes a window of its own. The picture is 192 lines of a 224-line
 * display, and the tilemap rows below it point at an all-zero tile — so
 * scanlines 192-223 are solid black whatever else is happening. A forced blank
 * asserted and released inside that strip covers nothing anybody can see, VRAM
 * and OAM are both writable for all of it, and NMI is nowhere near it.
 *
 * WIN_END leaves margin. One 64-byte transfer and its check cost about four
 * scanlines at -O0, and a blank still asserted when vblank arrived would be held
 * by NMI across the top hundred lines of the next frame — which is precisely the
 * artefact all of this exists to remove. */
#define WIN_TOP FB_H     /* 192: the first scanline below the picture */
#define WIN_END 210      /* offer no further transfer past here */

/* The PPU's V counter. Read $213F to reset the low/high toggle, $2137 to latch,
 * then $213D twice. */
static uint16_t vcount(void) {
  uint16_t v;
  R8(0x213F);
  R8(0x2137);
  v = (uint16_t)R8(0x213D);
  return (uint16_t)(v | ((uint16_t)(R8(0x213D) & 1) << 8));
}

/* Park the beam at the TOP of the strip — leaving whichever window we may
 * already be standing in — and then blank. Entering from above is what makes "at
 * least one transfer always lands" true, and therefore what makes the sweep in
 * flush_dirty finite. */
static void win_open(void) {
  uint16_t guard = 0;
  /* Both waits are bounded, for the reason pad_latch's are: a spin that can
   * never finish hangs the machine, and ~4000 iterations is around ten frames of
   * them. If NMI ever grew to swallow the strip as well, this would paint in the
   * wrong place — visibly — rather than stop. */
  while (vcount() >= WIN_TOP && ++guard < 4000u) ;
  guard = 0;
  while (vcount() < WIN_TOP && ++guard < 4000u) ;
  R8(0x2100) = 0x8F;
}

static void win_close(void) { R8(0x2100) = 0x0F; }

static int win_room(void) {
  uint16_t v = vcount();
  return v >= WIN_TOP && v < WIN_END;
}

/* One DMA from the WRAM staging buffer into VRAM, channel 0.
 *
 * Channel 0 is free: con_flush owns it but is a no-op while an application holds
 * the PPU, and the keyboard panel's HDMA (channels 3/4/5) is disabled for the
 * same reason. Nothing in NMI does DMA at all, so there is no one to collide
 * with mid-setup. */
static void dma_tiles(uint16_t vword, const uint16_t *src, uint16_t nwords) {
  uint32_t s = (uint32_t)src;
  uint16_t bytes = (uint16_t)(nwords * 2);
  R8(0x2116) = (uint8_t)vword;
  R8(0x2117) = (uint8_t)(vword >> 8);
  R8(0x4300) = 0x01;                   /* A->B, mode 1: low,high -> $2118,$2119 */
  R8(0x4301) = 0x18;
  R8(0x4302) = (uint8_t)s;
  R8(0x4303) = (uint8_t)(s >> 8);
  R8(0x4304) = (uint8_t)(s >> 16);     /* source bank, $7F */
  R8(0x4305) = (uint8_t)bytes;
  R8(0x4306) = (uint8_t)(bytes >> 8);
  R8(0x420B) = 0x01;
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

/* Consume and throw away n bytes.
 *
 * A server wider than the console sends rectangles that hang off the side of
 * the window, and every byte of one still has to come off the wire in order or
 * the stream desynchronises. The old code returned failure — that is, hung up —
 * on a rectangle it could not place, which was invisible only because the
 * server was exactly console-sized and no such rectangle existed. */
static int skipn(uint16_t n) {
  while (n) {
    uint16_t k = (n > (uint16_t)sizeof junk) ? (uint16_t)sizeof junk : n;
    if (!rd(junk, k)) return 0;
    n = (uint16_t)(n - k);
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
  /* KEEP THE REAL SIZE. It is not the console's size any more, and everything
   * about panning is arithmetic between the two. */
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

/* ------------------------------------------------------------- what to ask for
 *
 * A FramebufferUpdateRequest names a rectangle, so the window IS the request.
 * Two kinds go out:
 *
 *   incremental, over the whole window — the steady state; a still screen
 *   answers it with nothing at all, which is the property the whole design
 *   leans on.
 *
 *   non-incremental, over one bounding box — "I have never seen this, send it
 *   whatever you think I know". That is how a pan pays for only the strip it
 *   exposed, and how a CopyRect whose source scrolled out of the window repairs
 *   the part it could not copy. */
static int req(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int incremental) {
  uint8_t m[10];
  m[0] = 3;
  m[1] = (uint8_t)(incremental ? 1 : 0);
  put16(m + 2, x);
  put16(m + 4, y);
  put16(m + 6, w);
  put16(m + 8, h);
  return wr(m, 10);
}

/* Merge a rectangle into the pending non-incremental request.
 *
 * A bounding box rather than a region list. In every case that actually occurs
 * — a pan strip, or the one band a scroll could not copy — there is a single
 * rectangle and the box is exact; the pathological case costs bandwidth, not
 * correctness, and a region library on a 65816 would cost more than it saved. */
static void need_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  uint16_t x1, y1;
  if (w == 0 || h == 0) return;
  if (!rq_pending) {
    rqx = x; rqy = y; rqw = w; rqh = h;
    rq_pending = 1;
    return;
  }
  x1 = (uint16_t)(rqx + rqw);
  y1 = (uint16_t)(rqy + rqh);
  if ((uint16_t)(x + w) > x1) x1 = (uint16_t)(x + w);
  if ((uint16_t)(y + h) > y1) y1 = (uint16_t)(y + h);
  if (x < rqx) rqx = x;
  if (y < rqy) rqy = y;
  rqw = (uint16_t)(x1 - rqx);
  rqh = (uint16_t)(y1 - rqy);
}

/* Ask for the pending rectangle if there is one, otherwise for whatever has
 * changed in the window.
 *
 * ALWAYS SEND SOMETHING. An earlier version tracked whether poll_input() had
 * already sent a request this pass and skipped the loop's own, on the grounds
 * that two outstanding requests were untidy. That is not how a server accounts
 * for them: TigerVNC keeps a `requested` REGION, unions every request into it,
 * and CLEARS IT ENTIRELY when it sends an update. So the pan's strip request
 * consumed the window request that was outstanding behind it, and after the
 * last pan the client had nothing outstanding at all — at which point the
 * server has no reason to ever speak again.
 *
 * The symptom was a screen that panned perfectly and then stopped tracking the
 * page: end to end, a click navigated the browser (the web server logged the
 * hit) and the console went on showing the old page, repainting only the strips
 * that later pans fetched non-incrementally. An extra request costs ten bytes;
 * a missing one costs the session. */
static int send_request(void) {
  if (rq_pending) {
    rq_pending = 0;
    return req(rqx, rqy, rqw, rqh, 0);
  }
  return req(px, py, FB_W, FB_H, 1);
}

/* SERVER coordinates, not console ones: pan origin plus cursor. */
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

/* Move the cursor sprite — WHILE BLANKING, which is the whole of the second bug.
 *
 * OAM is not like VRAM. A write to $2104 during active display does not land
 * where OAMADDR points: sprite evaluation is using the OAM address counter
 * itself, so the byte goes wherever the renderer happens to be. The nine writes
 * below therefore did nothing at all, and the cursor sat where it was drawn.
 *
 * That is precisely why ESC worked and the arrow keys did not, and it is worth
 * spelling out because it looked exactly like a lost keystroke and sent me
 * hunting through Mesen's bindings, kbdbridge's key names and the kernel's d-pad
 * mapping — all three of which were fine. ESC has an effect that is not an OAM
 * write; an arrow key has nothing else. The harness settled it in one run: after
 * a RIGHT, OAM byte 0 was unchanged at 128 while the app's own pointer event
 * reported x=136. The key arrived, `cx` moved, and only the OAM write was lost.
 *
 * The fix is the same window the tiles use, for the same reason: below the
 * picture, forced blank, OAM writable. One frame of latency per move, and only
 * when something actually moved. */
static void place_cursor(void) {
  win_open();
  R8(0x2102) = 0;
  R8(0x2103) = 0;
  R8(0x2104) = (uint8_t)cx;
  R8(0x2104) = (uint8_t)(cy ? cy - 1 : 0);
  R8(0x2104) = 0x00;
  R8(0x2104) = 0x30;
  R8(0x2102) = 0x00;
  R8(0x2103) = 0x01;
  R8(0x2104) = (uint8_t)((cx > 255) ? 0x01 : 0x00);
  win_close();
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

/* ------------------------------------------------------------- the tilemap
 *
 * Which tile map cell (r, c) shows. WITHOUT panning this was the identity —
 * cell (r, c) shows tile r*32+c — and that identity is exactly what let the
 * rest of the app treat a shadow offset and a screen position as the same
 * thing. They are not the same thing any more.
 *
 * The shadow holds server pixel (sx, sy) at (sx mod 256, sy mod 192) for ever,
 * so the tile holding the top-left of the window is whichever tile the pan
 * origin lands in, and the row and column both wrap. Rows past the picture keep
 * pointing at the all-zero tile, which is what keeps scanlines 192-223 black
 * and therefore what keeps the write window usable. */
static uint16_t map_entry(uint16_t r, uint16_t c) {
  uint16_t ty, tx;
  if (r >= TILES_H) return TILE_BLANK;
  ty = (uint16_t)((pym >> 3) + r);
  while (ty >= TILES_H) ty = (uint16_t)(ty - TILES_H);
  tx = (uint16_t)((pxm >> 3) + c);
  while (tx >= TILES_W) tx = (uint16_t)(tx - TILES_W);
  return (uint16_t)((ty << 5) + tx);
}

/* Rewrite the picture rows of the tilemap for the current pan origin.
 *
 * THIS IS THE WHOLE OF A PAN, as far as everything already on screen is
 * concerned. 1.5 KB of tilemap instead of 48 KB of tile data, and not one pixel
 * of the shadow moves.
 *
 * The staging buffer is BATCH x 32 words, which is eight tilemap rows exactly,
 * and tilemap rows are contiguous in VRAM — so a band is one 512-byte DMA and
 * the whole map is three of them. Building a band costs about half a frame
 * outside the window; the DMA costs about a scanline and a half inside it. */
static void write_tilemap(void) {
  uint16_t r0, r, c;
  uint16_t *out;
  for (r0 = 0; r0 < TILES_H; r0 = (uint16_t)(r0 + BATCH)) {
    out = &stage[0][0];
    for (r = 0; r < BATCH; r++)
      for (c = 0; c < 32; c++) *out++ = map_entry((uint16_t)(r0 + r), c);
    /* Re-assert VMAIN inside the same guard flush_batch uses, and for the same
     * reason: anything that ran in between may have left the increment mode
     * somewhere else, and a tilemap written with the address advancing after
     * the LOW byte is a screen of scattered garbage. */
    R8(0x2115) = 0x80;
    win_open();
    dma_tiles((uint16_t)(((VRAM_MAP >> 1) + (r0 << 5))), &stage[0][0],
              (uint16_t)(BATCH * 32u));
    win_close();
  }
}

static void setup_ppu(void) {
  uint16_t r, c;

  /* One tile of solid black, for the four displayed rows below the picture.
   *
   * They used to show tile 0 — the top-left tile of the desktop — repeated
   * thirty-two times across the bottom of the screen, which was its own small
   * eyesore. Making the strip genuinely black also makes it USEFUL: it is where
   * every VRAM and OAM write in this app now happens, under a forced blank that
   * covers only these lines. See "the write window" above. */
  vaddr(VRAM_BLANK >> 1);
  for (r = 0; r < 32; r++) {
    R8(0x2118) = 0;
    R8(0x2119) = 0;
  }

  /* The whole map, written directly: the display is still under the forced
   * blank main() asserted, so there is no window to wait for. At pan (0,0)
   * map_entry() is the identity this used to spell out inline. */
  vaddr(VRAM_MAP >> 1);
  for (r = 0; r < 32; r++) {
    for (c = 0; c < 32; c++) {
      uint16_t t = map_entry(r, c);
      R8(0x2118) = (uint8_t)t;
      R8(0x2119) = (uint8_t)(t >> 8);
    }
  }

  R8(0x2105) = 0x03;                             /* Mode 3, BG1 8bpp */
  R8(0x210D) = 0; R8(0x210D) = 0;
  R8(0x210E) = 0; R8(0x210E) = 0;
  /* BG1SC. The base field counts 1K-WORD units, and VRAM_MAP here is a BYTE
   * address — so it has to be halved first. view.c's version of this line looks
   * identical but its VRAM_MAP is already a word address, and copying the
   * formula without the units gave 0xE0 instead of 0x70: a tilemap base past the
   * end of VRAM. The PPU then read tile INDICES from the wrong place, selected
   * arbitrary tiles, and drew scattered noise over a picture whose tile data was
   * perfectly correct all along.
   *
   * That is why it survived every check I made. My verification decoded tile
   * data in assumed tilemap order, which bypasses the tilemap entirely — so it
   * confirmed the one thing that was never broken. Ricardo's register log found
   * it in a single run. */
  R8(0x2107) = (uint8_t)((((uint16_t)(VRAM_MAP >> 1)) >> 10) << 2);
  R8(0x210B) = 0x00;                             /* BG1 tiles at $0000 */
  R8(0x2101) = 0x03;                             /* OBSEL: 8x8, base $C000 */
  R8(0x212C) = 0x11;                             /* BG1 + OBJ */
  R8(0x212D) = 0x00;
  R8(0x2100) = 0x0F;
}

/* Rebuild one tile from the shadow into the staging buffer.
 *
 * This is the transpose: the shadow is linear bytes, the PPU wants eight
 * bitplanes, and a tile is 8 rows of 8 pixels producing 32 words in the order
 * (planes 0&1) x8, (planes 2&3) x8, (4&5) x8, (6&7) x8.
 *
 * `t` is a SHADOW tile, not a screen position — the tilemap is what decides
 * where it appears — so this is untouched by panning.
 *
 * It runs OUTSIDE any window, which is why the result goes to WRAM rather than
 * straight to $2118: measured, the transpose plus 64 register writes cost about
 * 1.3 FRAMES per tile at -O0, so a tile has never fitted in a window and never
 * will. Staged in WRAM it becomes one DMA, which does. */
static void build_tile(uint16_t t, uint16_t *out) {
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
    out[r] = (uint16_t)(p0 | ((uint16_t)p1 << 8));
    out[8 + r] = (uint16_t)(p2 | ((uint16_t)p3 << 8));
    out[16 + r] = (uint16_t)(p4 | ((uint16_t)p5 << 8));
    out[24 + r] = (uint16_t)(p6 | ((uint16_t)p7 << 8));
  }
}

/* Push a staged batch through one write window. Returns how many tiles landed.
 *
 * A tile is only marked clean once its transfer has been issued inside the
 * window, which is con_flush's rule and there for the same reason: a write the
 * hardware ignores leaves no trace except a stale square on screen for ever, so
 * the flag must follow the transfer rather than the intention. */
static uint16_t flush_batch(uint16_t n) {
  uint16_t i, done = 0;

  /* Re-assert VMAIN on every pass, not once at startup. Anything that runs
   * between two flushes — and sys_yield() reaches the console and the keyboard
   * panel — can leave the VRAM increment mode somewhere else, and the result is
   * scrambled bitplanes on whichever tiles were written afterwards: scattered
   * noise on an otherwise black screen. One register write is cheaper than
   * reasoning about who else might have touched it. */
  R8(0x2115) = 0x80;
  win_open();
  for (i = 0; i < n; i++) {
    if (!win_room()) break;
    dma_tiles((uint16_t)(stage_t[i] * 32u), stage[i], 32);
    dirty[stage_t[i]] = 0;
    done++;
  }
  win_close();
  return done;
}

/* Paint every dirty tile, without ever blanking anything that is on screen.
 *
 * The forced blank this replaces was the flicker. `R8(0x2100) = 0x8F` around the
 * whole flush is correct — VRAM takes writes only while blanking — but
 * flush_dirty runs once per RECEIVED RECTANGLE, and a rectangle is hundreds of
 * tiles at 1.5 frames each, so the WHOLE SCREEN went dark and light again for
 * every one of them. Measured against a server sending a screen as six bands:
 * six episodes of 207 frames, 1242 frames of black alternating with the picture.
 * That is exactly the "alternating between data and black" Ricardo saw.
 *
 * The cost is unchanged — the transpose was always the expensive part — and the
 * blanking now happens where nothing is displayed. */
static void flush_dirty(void) {
  uint16_t from = 0;
  int missed = 0;

  for (;;) {
    uint16_t t, n = 0;
    for (t = from; t < NTILES && n < BATCH; t++) {
      if (!dirty[t]) continue;
      build_tile(t, stage[n]);
      stage_t[n] = t;
      n++;
    }
    from = t;
    if (n && flush_batch(n) < n) missed = 1;
    if (from >= NTILES) {
      /* Normally one linear sweep and done. Tiles a window ran out on are still
       * dirty, so sweep again — and since every window lands at least one tile,
       * every sweep makes progress. */
      if (!missed) return;
      from = 0;
      missed = 0;
    }
  }
}

/* The one blank that is honest: VRAM still holds the console's font when an app
 * takes the PPU, so the tiles are zeroed before the display is turned back on.
 *
 * No transpose and no window to wait for — every tile is the same 64 zero bytes
 * and the display is not on yet — so this is a couple of frames. Painting 768
 * black tiles through flush_dirty instead took 1015 frames of blank screen at
 * startup, and every one of them was spent transposing zeroes. */
static void clear_tiles(void) {
  uint16_t n, i, t;
  for (n = 0; n < BATCH; n++)
    for (i = 0; i < 32; i++) stage[n][i] = 0;
  R8(0x2115) = 0x80;
  for (t = 0; t < NTILES; t = (uint16_t)(t + BATCH))
    dma_tiles((uint16_t)(t * 32u), stage[0], (uint16_t)(BATCH * 32u));
}

/* ------------------------------------------------------------ the torus
 *
 * Console position -> shadow address, for a window whose origin is (px, py).
 * Both wrap, because the shadow holds server pixel (sx, sy) at
 * (sx mod 256, sy mod 192) regardless of where the window is. That is the
 * property that makes a pan free for everything already on screen.
 *
 * shrow() returns a byte offset, not a row number, because FB_W is 256 and the
 * multiply is therefore a shift — and the 65816 has no multiply instruction, so
 * "* FB_W" per row would otherwise be a call. */
static uint16_t shrow(uint16_t vy) {
  uint16_t s = (uint16_t)(vy + pym);
  if (s >= FB_H) s = (uint16_t)(s - FB_H);
  return (uint16_t)(s << 8);
}

static uint16_t shcol(uint16_t vx) {
  uint16_t s = (uint16_t)(vx + pxm);
  if (s >= FB_W) s = (uint16_t)(s - FB_W);
  return s;
}

/* Mark the shadow tiles a console-space rectangle covers. Wraps with the
 * shadow, which is why this cannot just clamp the way it used to. */
static void mark(uint16_t vx, uint16_t vy, uint16_t w, uint16_t h) {
  uint16_t c0 = (uint16_t)(vx >> 3), r0 = (uint16_t)(vy >> 3);
  uint16_t nc = (uint16_t)(((vx + w + 7u) >> 3) - c0);
  uint16_t nr = (uint16_t)(((vy + h + 7u) >> 3) - r0);
  uint16_t co = (uint16_t)(c0 + (pxm >> 3)), ro = (uint16_t)(r0 + (pym >> 3));
  uint16_t r, c;
  for (r = 0; r < nr; r++) {
    uint16_t ty = (uint16_t)(ro + r), base;
    while (ty >= TILES_H) ty = (uint16_t)(ty - TILES_H);
    base = (uint16_t)(ty << 5);
    for (c = 0; c < nc; c++) {
      uint16_t tx = (uint16_t)(co + c);
      while (tx >= TILES_W) tx = (uint16_t)(tx - TILES_W);
      dirty[base + tx] = 1;
    }
  }
}

/* --------------------------------------------------------------- panning */

static void set_pan(uint16_t nx, uint16_t ny) {
  px = nx;
  py = ny;
  pxm = px;
  while (pxm >= FB_W) pxm = (uint16_t)(pxm - FB_W);
  pym = py;
  while (pym >= FB_H) pym = (uint16_t)(pym - FB_H);
}

/* Move the window, one axis at a time.
 *
 * One axis, even when both are pending, so that the exposed area is always a
 * single strip and the bounding box in need_rect() is exact. A diagonal step
 * would make it the whole window, which is the fourteen-second request this
 * exists to avoid; the other axis just lands on the next pass.
 *
 * The pixels do not move. Only the tilemap does, and only the strip that was
 * never on screen has to come down the wire. */
static void apply_pan(void) {
  uint16_t nx = px, ny = py, ox = px, oy = py, d;

  if (pend_dx) {
    int16_t dx = pend_dx;
    pend_dx = 0;
    if (dx > 0) {
      nx = (uint16_t)(px + (uint16_t)dx);
      if (nx > maxpx) nx = maxpx;
    } else {
      uint16_t m = (uint16_t)(-dx);
      nx = (px > m) ? (uint16_t)(px - m) : 0;
    }
  } else if (pend_dy) {
    int16_t dy = pend_dy;
    pend_dy = 0;
    if (dy > 0) {
      ny = (uint16_t)(py + (uint16_t)dy);
      if (ny > maxpy) ny = maxpy;
    } else {
      uint16_t m = (uint16_t)(-dy);
      ny = (py > m) ? (uint16_t)(py - m) : 0;
    }
  }
  if (nx == ox && ny == oy) return;

  set_pan(nx, ny);
  write_tilemap();

  /* The strip that has never been on this console. Everything else in the new
   * window is already in the shadow, at the address it was always at. */
  if (nx > ox) {
    d = (uint16_t)(nx - ox);
    if (d > FB_W) d = FB_W;
    need_rect((uint16_t)(nx + FB_W - d), ny, d, FB_H);
  } else if (nx < ox) {
    d = (uint16_t)(ox - nx);
    if (d > FB_W) d = FB_W;
    need_rect(nx, ny, d, FB_H);
  } else if (ny > oy) {
    d = (uint16_t)(ny - oy);
    if (d > FB_H) d = FB_H;
    need_rect(nx, (uint16_t)(ny + FB_H - d), FB_W, d);
  } else {
    d = (uint16_t)(oy - ny);
    if (d > FB_H) d = FB_H;
    need_rect(nx, ny, FB_W, d);
  }
}

/* Cursor, panning and quit, serviced while a transfer is in flight.
 *
 * This has to run from inside the read loop, not only between updates. A first
 * screen is 48 KB — half a minute even now, and eighty seconds when this was
 * written — and an application that only checks the keyboard between whole
 * updates ignores you for all of it. The
 * cursor is a sprite, so moving it mid-transfer costs four bytes of OAM and no
 * network traffic at all.
 *
 * A PAN, though, cannot happen mid-message. Rectangles are placed using the pan
 * origin, so moving the origin while one is half-read would put the rest of it
 * in the wrong place — and only sometimes, only on a large rectangle, which is
 * the worst kind of bug to own. So `parsing` gates it and the step is held until
 * the stream is at a message boundary.
 *
 * At a boundary it is applied HERE rather than deferred to the main loop, and
 * that is not an optimisation: a still screen leaves the app blocked in rd()
 * waiting for a message the server has no reason to send, and a pan deferred to
 * a loop that is never reached is a pan that never happens. Applying it here
 * also sends its own request, which is what unblocks the read.
 *
 * Returns 1 if the user asked to leave. */
static int poll_input(void) {
  int moved = 0, panned = 0;
  for (;;) {
    int c = (int)sys_pollkey();
    if (c == (int)SYS_ERR) break;
    if (c == 0x1B) {
      quit = 1;
      return 1;
    }
    if (c == SNOS_KEY_LEFT) {
      cx = (uint16_t)(cx >= 8 ? cx - 8 : 0);
      moved = 1;
    } else if (c == SNOS_KEY_RIGHT) {
      cx = (uint16_t)(cx < FB_W - 8 ? cx + 8 : FB_W - 1);
      moved = 1;
    } else if (c == SNOS_KEY_UP) {
      cy = (uint16_t)(cy >= 8 ? cy - 8 : 0);
      moved = 1;
    } else if (c == SNOS_KEY_DOWN) {
      cy = (uint16_t)(cy < FB_H - 8 ? cy + 8 : FB_H - 1);
      moved = 1;
    } else if (c == 'a' || c == 'A') {
      pend_dx = (int16_t)(pend_dx - PAN_STEP);
      panned = 1;
    } else if (c == 'd' || c == 'D') {
      pend_dx = (int16_t)(pend_dx + PAN_STEP);
      panned = 1;
    } else if (c == 'w' || c == 'W') {
      pend_dy = (int16_t)(pend_dy - PAN_STEP);
      panned = 1;
    } else if (c == 's' || c == 'S') {
      pend_dy = (int16_t)(pend_dy + PAN_STEP);
      panned = 1;
    } else if (c == ' ') {
      /* Press and release at the cursor, in SERVER coordinates. The server
       * moves its own pointer there as a side effect, which is why we do not
       * stream every cursor step: each one would cost a framebuffer update of
       * the area it left. */
      pointer((uint16_t)(px + cx), (uint16_t)(py + cy), 1);
      pointer((uint16_t)(px + cx), (uint16_t)(py + cy), 0);
    }
  }
  /* Drain the queue first, THEN place the sprite, once. Every key of a held
   * repeat would otherwise wait for its own window, and auto-repeat delivers
   * several per frame. */
  if (moved && ready) place_cursor();
  if (panned && ready && !parsing) {
    apply_pan();
    if (rq_pending) send_request();
  }
  return 0;
}

/* ------------------------------------------------------------------ updates */

/* CopyRect, translated through the torus.
 *
 * Both ends have to be inside the window: the shadow holds the window and
 * nothing else, so a source that has scrolled out of view simply is not here to
 * copy from. Whatever cannot be satisfied is handed to need_rect(), which turns
 * it into a small non-incremental request rather than a stale band nobody
 * notices until they read it.
 *
 * The row goes via crow[] rather than being copied in place. Source and
 * destination each wrap at their own point on the torus, so an in-place copy
 * would need overlap reasoning modulo 256; gathering the row and then scattering
 * it removes the question entirely. Row ORDER still matters, and that is the
 * one thing a scratch buffer does not fix — a scroll's source and destination
 * rows overlap, so the direction is chosen from the sign of the shift. */
static void copy_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                      uint16_t sx, uint16_t sy) {
  int32_t dxs = (int32_t)sx - (int32_t)x;
  int32_t dys = (int32_t)sy - (int32_t)y;
  uint16_t vx1 = (uint16_t)(px + FB_W), vy1 = (uint16_t)(py + FB_H);
  uint16_t ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
  int32_t lo, hi, k0, k1;

  /* A: the part of the destination that is on screen at all. */
  ax0 = (x < px) ? px : x;
  ay0 = (y < py) ? py : y;
  ax1 = ((uint16_t)(x + w) > vx1) ? vx1 : (uint16_t)(x + w);
  ay1 = ((uint16_t)(y + h) > vy1) ? vy1 : (uint16_t)(y + h);
  if (ax1 <= ax0 || ay1 <= ay0) return;

  /* B: the part of A whose SOURCE is on screen too. */
  lo = (int32_t)px - dxs;
  hi = (int32_t)vx1 - dxs;
  k0 = (int32_t)ax0; if (k0 < lo) k0 = lo;
  k1 = (int32_t)ax1; if (k1 > hi) k1 = hi;
  bx0 = (uint16_t)k0;
  bx1 = (uint16_t)((k1 > k0) ? k1 : k0);
  lo = (int32_t)py - dys;
  hi = (int32_t)vy1 - dys;
  k0 = (int32_t)ay0; if (k0 < lo) k0 = lo;
  k1 = (int32_t)ay1; if (k1 > hi) k1 = hi;
  by0 = (uint16_t)k0;
  by1 = (uint16_t)((k1 > k0) ? k1 : k0);

  if (bx1 > bx0 && by1 > by0) {
    uint16_t ncols = (uint16_t)(bx1 - bx0), nrows = (uint16_t)(by1 - by0);
    uint16_t sr0 = (uint16_t)((int32_t)by0 + dys);
    uint16_t sc0 = (uint16_t)((int32_t)bx0 + dxs);
    uint16_t i;
    for (i = 0; i < nrows; i++) {
      /* Source above the destination means the top rows are overwritten first,
       * so walk from the bottom. This is the only part of a scroll that a row
       * buffer does not make order-independent. */
      uint16_t j = (dys < 0) ? (uint16_t)(nrows - 1 - i) : i;
      uint16_t srow = shrow((uint16_t)((uint16_t)(sr0 + j) - py));
      uint16_t drow = shrow((uint16_t)((uint16_t)(by0 + j) - py));
      uint16_t ssc = shcol((uint16_t)(sc0 - px));
      uint16_t dsc = shcol((uint16_t)(bx0 - px));
      uint16_t n1, k;
      n1 = (uint16_t)(FB_W - ssc);
      if (n1 > ncols) n1 = ncols;
      for (k = 0; k < n1; k++) crow[k] = fb[srow + ssc + k];
      for (k = n1; k < ncols; k++) crow[k] = fb[srow + (k - n1)];
      n1 = (uint16_t)(FB_W - dsc);
      if (n1 > ncols) n1 = ncols;
      for (k = 0; k < n1; k++) fb[drow + dsc + k] = crow[k];
      for (k = n1; k < ncols; k++) fb[drow + (k - n1)] = crow[k];
    }
    mark((uint16_t)(bx0 - px), (uint16_t)(by0 - py), ncols, nrows);
    /* A minus B, as up to four strips. For a plain vertical scroll — the case
     * that actually happens — exactly one of them is non-empty and the bounding
     * box need_rect() keeps is therefore exact. */
    need_rect(ax0, ay0, (uint16_t)(ax1 - ax0), (uint16_t)(by0 - ay0));
    need_rect(ax0, by1, (uint16_t)(ax1 - ax0), (uint16_t)(ay1 - by1));
    need_rect(ax0, by0, (uint16_t)(bx0 - ax0), (uint16_t)(by1 - by0));
    need_rect(bx1, by0, (uint16_t)(ax1 - bx1), (uint16_t)(by1 - by0));
  } else {
    /* Nothing of the source survives on screen. */
    need_rect(ax0, ay0, (uint16_t)(ax1 - ax0), (uint16_t)(ay1 - ay0));
  }
}

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

    /* A rectangle larger than the desktop is not a rectangle we misplaced, it
     * is a stream we have lost sync with — and the arithmetic below would
     * overflow 16 bits trying to place it. */
    if (w > srv_w || h > srv_h || x > srv_w || y > srv_h) {
      say("\nserver sent a rectangle off its own desktop\n");
      return 0;
    }
    if (w == 0 || h == 0) continue;

    if (enc == ENC_RAW) {
      /* Rectangles arrive in SERVER coordinates. Clip to the window, and drop
       * whatever falls outside it — the shadow byte an off-window pixel would
       * land on belongs to a different part of the desktop, so writing it there
       * would corrupt something that IS on screen. */
      uint16_t vx1 = (uint16_t)(px + FB_W), vy1 = (uint16_t)(py + FB_H);
      uint16_t cx0 = (x < px) ? px : x;
      uint16_t cy0 = (y < py) ? py : y;
      uint16_t cx1 = ((uint16_t)(x + w) > vx1) ? vx1 : (uint16_t)(x + w);
      uint16_t cy1 = ((uint16_t)(y + h) > vy1) ? vy1 : (uint16_t)(y + h);
      int any = (cx1 > cx0) && (cy1 > cy0);
      uint16_t pre = any ? (uint16_t)(cx0 - x) : 0;
      uint16_t mid = any ? (uint16_t)(cx1 - cx0) : 0;
      uint16_t post = any ? (uint16_t)((uint16_t)(x + w) - cx1) : 0;

      /* Straight into the shadow, a row at a time. No intermediate buffer:
       * a single rectangle can be 26 KB and there is nowhere to put it. */
      for (row = 0; row < h; row++) {
        uint16_t sy = (uint16_t)(y + row);
        uint16_t base, sc, n1;
        if (!any || sy < cy0 || sy >= cy1) {
          if (!skipn(w)) return 0;
          continue;
        }
        if (pre && !skipn(pre)) return 0;
        base = shrow((uint16_t)(sy - py));
        sc = shcol((uint16_t)(cx0 - px));
        n1 = (uint16_t)(FB_W - sc);
        if (n1 > mid) n1 = mid;
        if (!rd(&fb[base + sc], n1)) return 0;
        if (n1 < mid && !rd(&fb[base], (uint16_t)(mid - n1))) return 0;
        if (post && !skipn(post)) return 0;
      }
      if (any) {
        mark((uint16_t)(cx0 - px), (uint16_t)(cy0 - py), mid,
             (uint16_t)(cy1 - cy0));
        /* Paint it NOW, not after the whole update. A server may split one
         * screen across several messages, so waiting meant painting a fraction
         * of the screen with stale VRAM — the console's own font — showing
         * through the rest. That was the corruption. It also turns an
         * eighty-second first screen into something you can watch arrive. */
        flush_dirty();
      }
    } else if (enc == ENC_COPYRECT) {
      uint16_t sx, sy;
      if (!rd(hdr, 4)) return 0;
      sx = be16(hdr);
      sy = be16(hdr + 2);
      copy_rect(x, y, w, h, sx, sy);
    } else {
      /* We advertised only Raw and CopyRect; anything else means the stream is
       * no longer parseable and guessing would make it worse. */
      say("\nserver used an encoding we did not ask for\n");
      return 0;
    }
  }
  return 1;
}

/* Read one server message, handling the ones that are not updates.
 *
 * `parsing` brackets everything after the type byte: while it is set, a pan is
 * recorded but not applied, because the pan origin is what places every
 * rectangle in the message being read. The one place it is CLEAR and the app is
 * blocked is the read of the type byte itself, which is exactly the boundary a
 * pan is safe at. */
static int pump(void) {
  int ok;
  if (!rd(hdr, 1)) return 0;
  parsing = 1;
  if (hdr[0] == MSG_UPDATE) {
    ok = read_update();
  } else if (hdr[0] == MSG_BELL) {
    ok = 1;
  } else if (hdr[0] == MSG_COLOURMAP) {
    uint16_t n;
    ok = 0;
    if (rd(hdr, 5)) {
      n = be16(hdr + 3);
      ok = 1;
      while (n--) if (!rd(hdr, 6)) { ok = 0; break; }
    }
  } else if (hdr[0] == MSG_CUT) {
    uint32_t n;
    ok = 0;
    if (rd(hdr, 7)) {
      n = ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[4] << 16) |
          ((uint32_t)hdr[5] << 8) | hdr[6];
      ok = 1;
      while (n) {
        uint16_t take = (n > sizeof hdr) ? (uint16_t)sizeof hdr : (uint16_t)n;
        if (!rd(hdr, take)) { ok = 0; break; }
        n -= take;
      }
    }
  } else {
    say("\nunknown message from the server\n");
    ok = 0;
  }
  parsing = 0;
  return ok;
}

/* ---------------------------------------------------------------- the loop */

int main(void) {
  uint16_t i;

  /* Assigned here, not at their declarations: an app is linked with
   * --no-auto-libraries, so an initialised static reads back as zero and a
   * non-zero initialiser is silently discarded. */
  cx = FB_W / 2;
  cy = FB_H / 2;
  quit = 0;
  px = 0; py = 0; pxm = 0; pym = 0;
  maxpx = 0; maxpy = 0;
  pend_dx = 0; pend_dy = 0;
  parsing = 0;
  ready = 0;
  rq_pending = 0;

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

  /* How far the window may travel. Rounded DOWN to a multiple of 8 so that
   * every pan origin stays on a tile boundary even at the far edge — the
   * tilemap cannot express anything else, and a half-tile origin would quietly
   * turn every pan into a full 768-tile rebuild. */
  maxpx = (srv_w > FB_W) ? (uint16_t)((srv_w - FB_W) & 0xFFF8u) : 0;
  maxpy = (srv_h > FB_H) ? (uint16_t)((srv_h - FB_H) & 0xFFF8u) : 0;

  say("connected: ");
  sys_putu(srv_w);
  sys_putc('x');
  sys_putu(srv_h);
  sys_putc('\n');
  if (maxpx || maxpy)
    say("WASD pans the window, arrows move the cursor, space clicks\n");
  else
    say("arrows move the cursor, space clicks\n");
  say("ESC quits\n");

  /* Start from a known screen: clear the shadow, and clear VRAM to match it, so
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
  for (i = 0; i < NTILES; i++) dirty[i] = 0;

  say("receiving the first screen - about 30s on this link\n");
  say("it paints as it arrives; ESC quits\n");
  sys_gfx(1);
  R8(0x2100) = 0x8F;
  /* Increment the VRAM address after the HIGH byte, so a low/high pair writes
   * one word. Without this the console's last setting applies, the address
   * advances after the LOW byte, and every word is split across two addresses —
   * which paints the console's own font back at you, scrambled. `view` has
   * always set this; copying its VRAM layout without its VMAIN was the bug. */
  R8(0x2115) = 0x80;
  /* Stop any HDMA before touching the display. The keyboard panel drives HDMA
   * on channels 3/4/5 and rewrites BG registers every scanline, which would
   * shred a Mode 3 picture. NOT the cause of the corruption this was added
   * for — Ricardo's log showed $420C was already 00 — but an app that takes the
   * display should not depend on somebody else's teardown. */
  R8(0x420C) = 0x00;
  load_palette();
  hide_sprites();
  load_cursor();
  clear_tiles();
  setup_ppu();
  ready = 1;
  /* After setup_ppu, so the display is already on when the cursor lands: OAM
   * writes are only reliable while blanking, and place_cursor waits for a vblank
   * to get that. Under the forced blank above it would work either way; doing it
   * here keeps one rule instead of two. */
  place_cursor();

  /* The first screen is the window, in full, and the server has no idea what we
   * already have — so it is the same non-incremental request a pan uses. */
  need_rect(px, py, FB_W, FB_H);

  while (!quit) {
    if (pend_dx || pend_dy) apply_pan();
    if (!send_request()) break;
    if (!pump()) break;
    /* Rectangles are painted as they arrive, inside read_update; this catches
     * anything a CopyRect left dirty, since that path marks without painting. */
    flush_dirty();
    /* Drain the keys that arrived while a screenful was being read. The cursor
     * moves locally with no round trip, which is the whole reason it is a
     * sprite — at 615 B/s a server-drawn cursor would lag by a second. */
    poll_input();
  }

  R8(0x2100) = 0x8F;
  sys_gfx(0);
  sys_net_close(ch);
  say("\n");
  return 0;
}
