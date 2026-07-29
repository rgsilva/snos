/* SNOS WEB — a web browser, for a given value of browser.
 *
 *     web URL
 *
 *     arrow keys   move the cursor
 *     space        click whatever is under it
 *     W / S        window up / down
 *     A / D        back / forward
 *     ESC          quit
 *
 * The console does not parse a byte of HTML and never will: Wikipedia's SNES
 * article is 735 KB of markup against a 64 KB app arena, which is not a tuning
 * problem. What the SNES does here is what it is genuinely good at — display a
 * bitmap and move a sprite over it — while a browser on the far side of the
 * link does the part a 3.58 MHz 65816 cannot. That is the same division of
 * labour that makes TLS and JPEG decoding possible on this machine.
 *
 * So this is honestly a remote frame buffer, not a browser. The interesting
 * claim is not that the SNES understands the web; it is that the SNES is the
 * right shape of hardware to *show* it.
 *
 * ------------------------------------------------------------------ v1 notes
 *
 * Every action costs a whole window — about 49 KB, which is 80 seconds at the
 * emulator's 615 B/s and around 2.5 at the cable's 20 KB/s. There is no partial
 * update because the console has no model of the page: it has pixels. v2 sends
 * unique tiles plus a tilemap and keeps a cache, at which point a scroll costs
 * only the tiles that are genuinely new — measured at about 22% of a window,
 * because a rendered page is mostly a font and the glyphs are already resident.
 *
 * The window is 192 lines rather than 224. A full-screen 8bpp image is 57 KB of
 * tiles and the sprite tile base can only sit on a 16 KB boundary, every one of
 * which lands inside an image that large. Giving up 32 scanlines buys a cursor
 * with no overlap and no corrupted block on screen. v2's 4-colour format makes
 * tiles 16 bytes instead of 64 and frees over 30 KB, so this is temporary.
 */
#include "snos_abi.h"

#define IMG_HDR 16
#define IMG_PAL 512
#define TILE_BYTES 64
#define TILE_WORDS 32

#define SCR_TILES_W 32
#define SCR_TILES_H 24 /* 192 lines: see the note above about the sprite base */

/* VRAM, chosen so nothing overlaps:
 *   $0000-$BFFF  768 image tiles, 8bpp
 *   $C000-$DFFF  sprite tiles (OBSEL base 3)
 *   $E000-$E7FF  BG1 tilemap
 */
#define VRAM_TILES 0x0000
#define VRAM_OBJ 0xC000
#define VRAM_MAP 0xE000

#define DMA_CHUNK 0x2000
#define CACHE "~WEB.IMG"

#define R8(a) (*(volatile __far uint8_t *)(uint32_t)(a))

static char args[SNOS_ARGV_MAX];
static uint8_t buf[IMG_PAL];
static uint16_t tw, th;
/* Cursor position, in SNES pixels. NOT initialised here: apps link with
 * --no-auto-libraries, so the C startup that copies initialised data out of ROM
 * never runs and an initialiser silently reads as zero. The symptom was a
 * cursor parked at (0,255) — off-screen — with the rest of its OAM entry
 * perfectly correct. */
static uint16_t cx, cy;

static void say(const char *s) { sys_puts(s); }

static void vaddr(uint16_t w) {
  R8(0x2116) = (uint8_t)w;
  R8(0x2117) = (uint8_t)(w >> 8);
}

static void vdata(uint16_t v) {
  R8(0x2118) = (uint8_t)v;
  R8(0x2119) = (uint8_t)(v >> 8);
}

/* ------------------------------------------------------------------ cursor */

/* An arrow, 8x8, 4bpp. Two colours: white body, black outline, so it stays
 * visible over both a white page and a dark image. Drawn as a sprite because a
 * sprite costs nothing to move — no repaint, no tile churn, no DMA. Moving a
 * cursor by rewriting the bitmap would cost a window per step. */
static const uint8_t ARROW[8] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0x90, 0x10,
};
static const uint8_t ARROW_EDGE[8] = {
    0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFC, 0xF8, 0x38,
};

static void load_cursor(void) {
  int i;
  /* 4bpp: planes 0,1 interleaved for 8 rows, then planes 2,3. Plane 0 is the
   * body, plane 1 the outline, giving palette entries 1 and 2. */
  vaddr(VRAM_OBJ >> 1);
  for (i = 0; i < 8; i++)
    vdata((uint16_t)(ARROW[i] | ((uint16_t)(ARROW_EDGE[i] & ~ARROW[i]) << 8)));
  for (i = 0; i < 8; i++) vdata(0);

  /* Sprite palettes start at CGRAM 128, 16 entries each. The image owns all
   * 256 entries, so entries 129/130 are overwritten here — two pixels of a
   * 49 KB picture, in exchange for a cursor that is always visible. */
  R8(0x2121) = 129;
  R8(0x2122) = 0xFF;
  R8(0x2122) = 0x7F; /* white */
  R8(0x2122) = 0x00;
  R8(0x2122) = 0x00; /* black */
}

static void place_cursor(void) {
  /* OAM entry 0: X, Y, tile, flags. Y is written one line high because the PPU
   * displays a sprite starting on the line AFTER the one in OAM. */
  R8(0x2102) = 0;
  R8(0x2103) = 0;
  R8(0x2104) = (uint8_t)cx;
  R8(0x2104) = (uint8_t)(cy - 1);
  R8(0x2104) = 0x00;                    /* tile 0 from the OBJ base */
  R8(0x2104) = 0x30;                    /* priority 3, palette 0 */
  /* The high table: two bits per sprite — X bit 8, and size. Everything past
   * sprite 0 is parked off-screen by setting its X high bit with no size bit,
   * which the rest of OAM does not otherwise guarantee. */
  R8(0x2102) = 0x00;
  R8(0x2103) = 0x01;
  R8(0x2104) = (uint8_t)((cx > 255) ? 0x01 : 0x00);
}

static void hide_other_sprites(void) {
  int i;
  R8(0x2102) = 0;
  R8(0x2103) = 0;
  for (i = 0; i < 128; i++) {
    R8(0x2104) = 0;
    R8(0x2104) = 0xF0; /* below the screen */
    R8(0x2104) = 0;
    R8(0x2104) = 0;
  }
}

/* ------------------------------------------------------------------ display */

static void build_tilemap(void) {
  uint16_t xoff = (uint16_t)((SCR_TILES_W - tw) / 2);
  uint16_t yoff = (uint16_t)((SCR_TILES_H - th) / 2);
  uint16_t blank = (uint16_t)(tw * th);
  uint16_t r, c;

  vaddr(VRAM_MAP >> 1);
  for (r = 0; r < 32; r++) {
    for (c = 0; c < 32; c++) {
      uint16_t t = blank;
      if (c >= xoff && c < (uint16_t)(xoff + tw) && r >= yoff &&
          r < (uint16_t)(yoff + th))
        t = (uint16_t)((r - yoff) * tw + (c - xoff));
      vdata(t);
    }
  }
}

/* Put the cached window on the screen. Returns 0 if it will not fit. */
static int show(void) {
  uint32_t total, done;
  uint16_t got, words, i;

  snos_set_ptr(CACHE);
  if (sys_open(0) == SYS_ERR) return 0;
  if (sys_read_f(buf, IMG_HDR) != IMG_HDR) {
    sys_close();
    return 0;
  }
  tw = (uint16_t)(buf[8] | ((uint16_t)buf[9] << 8));
  th = (uint16_t)(buf[10] | ((uint16_t)buf[11] << 8));
  if (tw == 0 || th == 0 || tw > SCR_TILES_W || th > SCR_TILES_H) {
    sys_close();
    return 0;
  }

  R8(0x2100) = 0x8F; /* forced blank: VRAM and CGRAM take writes only then */

  if (sys_read_f(buf, IMG_PAL) == IMG_PAL) {
    R8(0x2121) = 0;
    for (i = 0; i < IMG_PAL; i++) R8(0x2122) = buf[i];
  }

  R8(0x2115) = 0x80;
  vaddr(VRAM_TILES);
  total = (uint32_t)tw * th * TILE_BYTES;
  done = 0;
  while (done < total) {
    uint32_t left = total - done;
    got = sys_read_vram((uint16_t)(left > DMA_CHUNK ? DMA_CHUNK : left));
    if (got == 0) break;
    done += got;
  }
  sys_close();

  words = (uint16_t)(tw * th * TILE_WORDS);
  vaddr(words);
  for (i = 0; i < TILE_WORDS; i++) vdata(0);

  build_tilemap();
  hide_other_sprites();
  load_cursor();
  place_cursor();

  R8(0x2105) = 0x03; /* Mode 3: BG1 is 8bpp */
  R8(0x210D) = 0; R8(0x210D) = 0;
  R8(0x210E) = 0; R8(0x210E) = 0;
  R8(0x210F) = 0; R8(0x210F) = 0;
  R8(0x2110) = 0; R8(0x2110) = 0;
  R8(0x2107) = (uint8_t)((VRAM_MAP >> 10) << 2); /* BG1SC */
  R8(0x210B) = 0x00;                             /* BG1 tiles at $0000 */
  R8(0x2101) = 0x03;                             /* OBSEL: 8x8, base $C000 */
  R8(0x212C) = 0x11;                             /* main screen: BG1 + OBJ */
  R8(0x212D) = 0x00;
  R8(0x2100) = 0x0F; /* display on */
  return 1;
}

/* ------------------------------------------------------------- transferring */

/* Drain a window off the channel into the cache file.
 *
 * Through SRAM rather than straight to VRAM, deliberately: VRAM only takes
 * writes during blanking, so streaming 49 KB into it directly would mean eighty
 * seconds of black screen with no way to say anything. Buffering costs a second
 * and a half each way and buys a console that can print dots.
 */
static int fetch(uint16_t ch) {
  uint32_t got = 0, dots = 0;
  uint16_t deadline;
  int st, full = 0;

  deadline = (uint16_t)(sys_frames() + 3600u);
  while ((st = (int)sys_net_state(ch)) == NET_ST_OPENING) {
    if ((int16_t)(sys_frames() - deadline) >= 0) {
      say("\ntimed out waiting for the page\n");
      return 0;
    }
    if (sys_pollkey() != SYS_ERR) {
      say("\ncancelled\n");
      return 0;
    }
    sys_yield();
  }
  if (st != NET_ST_OPEN && st != NET_ST_EOF) {
    say("\nthe gateway could not render that (state ");
    sys_putu((uint16_t)st);
    say(")\n");
    return 0;
  }

  snos_set_ptr(CACHE);
  if (sys_open(1) == SYS_ERR) {
    say("\ncannot write the page cache\n");
    return 0;
  }

  for (;;) {
    uint16_t n = sys_net_read(ch, buf, 128), i;
    if (n > 0 && n != SYS_ERR) {
      for (i = 0; i < n; i++) {
        if (sys_putc_f(buf[i]) == SYS_ERR) {
          full = 1;
          break;
        }
      }
      if (full) break;
      got += n;
      /* One dot per 4 KB. The transfer is the whole cost of using this, so a
       * still screen would be indistinguishable from a hang. */
      while (dots < (got >> 12)) {
        sys_putc('.');
        dots++;
      }
      continue;
    }
    st = (int)sys_net_state(ch);
    if (st != NET_ST_OPEN && st != NET_ST_OPENING) break;
    if (sys_pollkey() != SYS_ERR) {
      say("\ncancelled\n");
      sys_close();
      return 0;
    }
    sys_yield();
  }
  sys_close();
  if (full) {
    say("\nno room on the volume for a page\n");
    return 0;
  }

  /* Say WHY, always. This used to end with a bare `return got > ...`, so a
   * gateway that failed after the status had already been sent — an error
   * arriving mid-transfer, which is exactly what a flaky link produces —
   * dropped straight out of the application with no message at all. The user
   * saw `[web exited 0]` under `opening <url>` and had nothing to go on. */
  if (st == NET_ST_ERROR) {
    say("\nthe gateway failed part way through\n");
    return 0;
  }
  if (got <= IMG_HDR + IMG_PAL) {
    say("\nthe page arrived empty (");
    sys_putu((uint16_t)got);
    say(" bytes)\n");
    return 0;
  }
  return 1;
}

/* ---------------------------------------------------------------- the loop */

int main(void) {
  uint16_t ch;
  int running = 1;

  cx = 128;
  cy = 96;

  snos_getargs(args, sizeof args);
  if (args[0] == 0) {
    say("usage: web URL\n");
    return 1;
  }
  if (!sys_net_up()) {
    say("no gateway attached\n");
    return 1;
  }

  say("opening ");
  say(args);
  sys_putc('\n');

  ch = sys_web_open(args);
  if (ch == SYS_ERR) {
    say("no free channel\n");
    return 1;
  }

  while (running) {
    if (!fetch(ch)) break;
    sys_gfx(1); /* takes the PPU, and hides the keyboard's HDMA split */
    if (!show()) {
      sys_gfx(0);
      say("that page did not fit the screen\n");
      break;
    }

    /* Cursor and commands. Nothing here repaints: the sprite moves by writing
     * four bytes of OAM, so the page underneath is untouched. */
    for (;;) {
      int c = (int)sys_pollkey();
      uint8_t op = 0xFF, a = 0, b = 0;

      if (c == (int)SYS_ERR) {
        sys_yield();
        continue;
      }
      if (c == 0x1B) { /* ESC */
        running = 0;
        break;
      }
      if (c == SNOS_KEY_LEFT) { if (cx >= 8) cx -= 8; else cx = 0; }
      else if (c == SNOS_KEY_RIGHT) { if (cx < 248) cx += 8; else cx = 255; }
      else if (c == SNOS_KEY_UP) { if (cy >= 8) cy -= 8; else cy = 0; }
      else if (c == SNOS_KEY_DOWN) { if (cy < 184) cy += 8; else cy = 191; }
      else if (c == ' ') { op = WEB_OP_CLICK; a = (uint8_t)cx; b = (uint8_t)cy; }
      else if (c == 'w' || c == 'W') { op = WEB_OP_SCROLL; a = 0xFF; } /* -1 */
      else if (c == 's' || c == 'S') { op = WEB_OP_SCROLL; a = 1; }
      else if (c == 'a' || c == 'A') { op = WEB_OP_BACK; }
      else if (c == 'd' || c == 'D') { op = WEB_OP_FORWARD; }
      else continue;

      if (op == 0xFF) {
        place_cursor();
        continue;
      }
      if (sys_web(ch, op, a, b) == SYS_ERR) continue;
      break; /* a fresh window is on its way */
    }

    if (running) {
      R8(0x2100) = 0x8F;
      sys_gfx(0); /* back to the console, so the dots are visible again */
    }
  }

  R8(0x2100) = 0x8F;
  sys_gfx(0);
  sys_net_close(ch);
  say("\n");
  return 0;
}
