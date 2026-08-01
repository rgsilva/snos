/* SNOS VIEW — display a .IMG picture.
 *
 * The point of this application is how little it does. It decodes nothing,
 * scales nothing and quantises nothing: whoever MADE the .IMG did all of
 * that, so viewing is a copy into VRAM and a handful of register writes. A
 * 65816 cannot decode a JPEG in any tolerable time, and it does not have to
 * — since the uIP cutover the conversion is not even on the link's path: a
 * ready-made .IMG downloads over plain HTTP like any other file (M12's
 * render service is the eventual factory for them).
 *
 * -------------------------------------------------------------- the display
 *
 * Mode 3 gives BG1 eight bits per pixel, which is what makes 256 colours
 * possible at all. There is no linear framebuffer on this machine, so the
 * picture is faked the traditional way: every tile is unique and the tilemap
 * simply counts. A full 32x28 screen of 8bpp tiles is 57 KB of the 64 KB of
 * VRAM, so the console's font and both its tilemaps cannot survive alongside
 * it — SYS_GFX hands the PPU over and rebuilds the console afterwards.
 *
 * The picture is centred on TILE boundaries, so the tilemap is a plain counter
 * over a rectangle and nothing is ever transposed here.
 *
 * ------------------------------------------------------------------ loading
 *
 * Tile data goes from cartridge SRAM into VRAM by DMA without passing through
 * WRAM or through this program: SRAM is on the A-bus, VRAM answers on the
 * B-bus. The obvious loop of SYS_GETC_F would move the same 43 KB at about
 * 30 KB/s — a second and a half of watching it happen.
 */
#include "snos_abi.h"

#define IMG_HDR 16
#define IMG_PAL 512
#define TILE_WORDS 32 /* 8bpp: 64 bytes per tile */

#define VRAM_TILES 0x0000
#define VRAM_MAP 0x7400 /* clear of the largest picture, 32x28 tiles */

#define SCR_TILES_W 32
#define SCR_TILES_H 28

#define DMA_CHUNK 0x2000

#define R8(a) (*(volatile __far uint8_t *)(uint32_t)(a))

static uint8_t buf[IMG_PAL];
static char name[24];
static uint16_t tw, th;

/* ---------------------------------------------------------------- helpers */

static void say(const char *s) { sys_puts(s); }

static void vaddr(uint16_t w) {
  R8(0x2116) = (uint8_t)w;
  R8(0x2117) = (uint8_t)(w >> 8);
}

static void vdata(uint16_t v) {
  R8(0x2118) = (uint8_t)v;
  R8(0x2119) = (uint8_t)(v >> 8);
}

static void build_tilemap(void) {
  uint16_t xoff = (uint16_t)((SCR_TILES_W - tw) / 2);
  uint16_t yoff = (uint16_t)((SCR_TILES_H - th) / 2);
  uint16_t blank = (uint16_t)(tw * th); /* one past the picture's own tiles */
  uint16_t r, c;

  vaddr(VRAM_MAP);
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

/* --------------------------------------------------------------- the show */

static int show(const char *file) {
  uint16_t i, got, words;
  uint32_t total, done;

  snos_set_ptr(file);
  if (sys_open(0) == SYS_ERR) {
    say("no such file: ");
    say(file);
    say("\n");
    return 0;
  }

  if (sys_read_f(buf, IMG_HDR) != IMG_HDR || buf[0] != 'S' || buf[1] != 'I' ||
      buf[2] != 'M' || buf[3] != 'G') {
    sys_close();
    say("not a picture (no SIMG header)\n");
    say("fetch one with: get URL NAME\n");
    return 0;
  }
  tw = (uint16_t)(buf[8] | (buf[9] << 8));
  th = (uint16_t)(buf[10] | (buf[11] << 8));
  if (tw == 0 || th == 0 || tw > SCR_TILES_W || th > SCR_TILES_H) {
    sys_close();
    say("picture does not fit the screen\n");
    return 0;
  }

  /* Take the PPU before touching a register. That hides the keyboard, which
   * also switches off the HDMA split — it rewrites the BG tilemap-base and
   * scroll registers every scanline and would shred a Mode 3 picture. */
  sys_gfx(1);
  R8(0x2100) = 0x8F; /* forced blank: VRAM and CGRAM accept writes only then */

  /* Palette into CGRAM, exactly as stored. */
  if (sys_read_f(buf, IMG_PAL) == IMG_PAL) {
    R8(0x2121) = 0;
    for (i = 0; i < IMG_PAL; i++) R8(0x2122) = buf[i];
  }

  /* Tiles: storage straight to VRAM, no detour through RAM. */
  R8(0x2115) = 0x80; /* word address, increment after the high byte */
  vaddr(VRAM_TILES);
  total = (uint32_t)tw * th * 64ul;
  done = 0;
  while (done < total) {
    uint32_t left = total - done;
    got = sys_read_vram((uint16_t)(left > DMA_CHUNK ? DMA_CHUNK : left));
    if (got == 0) break;
    done += got;
  }
  sys_close();

  /* One blank tile past the picture, for the pillarbox margins. */
  words = (uint16_t)(tw * th * TILE_WORDS);
  vaddr(words);
  for (i = 0; i < TILE_WORDS; i++) vdata(0);

  build_tilemap();

  R8(0x2105) = 0x03;                             /* Mode 3: BG1 is 8bpp */

  /* Zero the scroll registers. The console leaves them wherever its own scroll
   * ring put them, and nothing else resets them — so a picture viewed after the
   * shell had scrolled appeared shifted vertically by exactly that amount, and
   * a picture viewed on a fresh screen looked perfect. Write-twice registers,
   * hence the pairs. */
  R8(0x210D) = 0; R8(0x210D) = 0;                /* BG1HOFS */
  R8(0x210E) = 0; R8(0x210E) = 0;                /* BG1VOFS */
  R8(0x210F) = 0; R8(0x210F) = 0;                /* BG2HOFS */
  R8(0x2110) = 0; R8(0x2110) = 0;                /* BG2VOFS */
  R8(0x2107) = (uint8_t)((VRAM_MAP >> 10) << 2); /* BG1SC, 32x32 */
  R8(0x210B) = 0x00;                             /* BG1 tiles at $0000 */
  R8(0x212C) = 0x01;                             /* main screen: BG1 only */
  R8(0x212D) = 0x00; /* Mode 3 is not hires, so the sub screen stays empty */
  R8(0x2100) = 0x0F; /* display on */
  return 1;
}

static void restore(void) {
  R8(0x2100) = 0x8F; /* blank first, so the console rebuild is never seen */
  sys_gfx(0);
}

/* ---------------------------------------------------------------- picking */

static int pick(char *out) {
  uint16_t count = 0, sel = 0, i;
  int c, redraw = 1;

  for (i = 0; i < 64; i++) {
    snos_set_ptr(name);
    if (sys_dir(i) == 0) break;
    count++;
  }
  if (count == 0) {
    say("no files\n");
    return 0;
  }

  sys_osk(0); /* the d-pad arrives as arrow codes only while it is hidden */
  for (;;) {
    if (redraw) {
      redraw = 0;
      sys_cls();
      say("Pictures - Up/Down, Enter views, Esc quits\n\n");
      for (i = 0; i < count; i++) {
        snos_set_ptr(name);
        sys_dir(i);
        say(i == sel ? "> " : "  ");
        say(name);
        say("\n");
      }
    }
    sys_yield();
    c = (int)sys_pollkey();
    if (c == (int)SYS_ERR) continue;
    if (c == SNOS_KEY_UP && sel) {
      sel--;
      redraw = 1;
    } else if (c == SNOS_KEY_DOWN && sel + 1 < count) {
      sel++;
      redraw = 1;
    } else if (c == '\n' || c == '\r') {
      int j = 0;
      snos_set_ptr(name);
      sys_dir(sel);
      while (name[j] && j < 22) {
        out[j] = name[j];
        j++;
      }
      out[j] = 0;
      return 1;
    } else if (c == 0x1B) {
      return 0;
    }
  }
}

/* ------------------------------------------------------------------- main */

int main(void) {
  char arg[SNOS_ARGV_MAX];
  char chosen[24];

  snos_getargs(arg, (uint16_t)sizeof arg);

  if (arg[0] == 0) {
    if (!pick(chosen)) return 0;
  } else {
    int i = 0;
    while (arg[i] && i < 22) {
      chosen[i] = arg[i];
      i++;
    }
    chosen[i] = 0;
  }

  if (!show(chosen)) return 1;

  /* Any key leaves, consistent with the rest of the system being
   * keyboard-first. Wait for buttons to be released first, or the press that
   * launched the viewer would dismiss it before it was ever seen. */
  while (sys_pad() != 0) sys_waitvbl();
  for (;;) {
    if (sys_pollkey() != SYS_ERR) break;
    if (sys_pad() != 0) break;
    sys_waitvbl();
  }

  restore();
  say("viewed ");
  say(chosen);
  say("\n");
  return 0;
}
