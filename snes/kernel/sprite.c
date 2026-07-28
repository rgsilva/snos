#include "sprite.h"
#include "font8x8_basic.h"

#define R8(a) (*(volatile uint8_t *)(a))

/* Sprite tiles live at VRAM word $6000. $2101's name-base field counts in units
 * of 8K words, so base 3 = $6000 — which sits in the gap above the BG1 font
 * ($3000-$4FFF) and below the top of VRAM. */
#define SPR_TILE_BASE 0x6000
#define SPR_NAME_BASE 3

#define TILE_PAD 128   /* solid panel fill */
#define TILE_BORDER 129 /* solid frame */

/* Two palettes rather than two sets of tiles: selecting a key is a palette
 * change on one sprite, not a re-upload of its graphics. */
#define PAL_NORMAL 0
#define PAL_SELECTED 1

#define OAM_SPRITES 128
#define OAM_BYTES (OAM_SPRITES * 4 + 32)

static uint8_t oam[OAM_BYTES];

static void vaddr(uint16_t a) {
  R8(0x2116) = (uint8_t)a;
  R8(0x2117) = (uint8_t)(a >> 8);
}

static void vdata(uint16_t v) {
  R8(0x2118) = (uint8_t)v;
  R8(0x2119) = (uint8_t)(v >> 8);
}

static uint8_t rev(uint8_t b) { /* font8x8 is LSB-left, the SNES wants MSB-left */
  b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
  b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
  b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
  return b;
}

static void cgram(uint16_t idx, uint16_t colour) {
  R8(0x2121) = (uint8_t)idx;
  R8(0x2122) = (uint8_t)colour;
  R8(0x2122) = (uint8_t)(colour >> 8);
}

#define BLACK 0x0000u
#define GREEN 0x03E0u
#define WHITE 0x7FFFu
#define DIMGREEN 0x01A0u

void spr_init(void) {
  int ch, y, i;

  /* Force blank first. VRAM and CGRAM are only writable during blanking, and
   * this runs after con_init has already turned the display on — without this
   * the tile upload is dropped and the palette writes corrupt CGRAM, which
   * shows up as the entire screen turning the backdrop colour. */
  R8(0x2100) = 0x8F;

  R8(0x2101) = (uint8_t)SPR_NAME_BASE; /* 8x8/16x16, tiles at $6000 */

  /* Glyph tiles, 4bpp and fully OPAQUE: colour 1 where the glyph is, colour 2
   * everywhere else. Colour 0 would be transparent and let the text underneath
   * show through the keyboard, which is exactly what we are trying to stop. */
  vaddr(SPR_TILE_BASE);
  for (ch = 0; ch < 128; ch++) {
    for (y = 0; y < 8; y++) {
      uint8_t g = rev((uint8_t)font8x8_basic[ch][y]);
      vdata((uint16_t)(g | ((uint16_t)(uint8_t)~g << 8))); /* plane0 | plane1 */
    }
    for (y = 0; y < 8; y++) vdata(0); /* planes 2 and 3 unused */
  }
  /* Solid fill (colour 2) and solid frame (colour 3). */
  for (y = 0; y < 8; y++) vdata(0xFF00);
  for (y = 0; y < 8; y++) vdata(0);
  for (y = 0; y < 8; y++) vdata(0xFFFF);
  for (y = 0; y < 8; y++) vdata(0);

  /* Sprite palettes start at CGRAM 128. Palette 0 is a normal key, palette 1 is
   * the selected one with colours 1 and 2 swapped. */
  cgram(128 + 1, GREEN);   /* key glyph */
  cgram(128 + 2, BLACK);   /* key cap / panel fill */
  cgram(128 + 3, DIMGREEN);/* frame: present, but not shouting over the keys */
  cgram(144 + 1, BLACK);   /* selected key: the same two colours, swapped */
  cgram(144 + 2, GREEN);
  cgram(144 + 3, DIMGREEN);

  for (i = 0; i < OAM_SPRITES; i++) {
    oam[i * 4 + 0] = 0;
    oam[i * 4 + 1] = 240; /* parked off the bottom of the screen */
    oam[i * 4 + 2] = 0;
    oam[i * 4 + 3] = 0;
  }
  for (i = 0; i < 32; i++) oam[OAM_SPRITES * 4 + i] = 0; /* all small, X < 256 */

  R8(0x2100) = 0x0F; /* display back on */
}

void spr_enable(int on) {
  /* Sprites must go on BOTH the main and the sub screen, exactly like the two
   * text layers. Mode 5 builds its 512 dots by taking the odd pixels from the
   * main screen and the even ones from the sub screen, so anything enabled on
   * only one of them is drawn with every other pixel column missing — the
   * panel comes out striped. This is the same trap documented for the BGs in
   * docs/console-notes.md. */
  R8(0x212C) = (uint8_t)(on ? 0x13 : 0x03);
  R8(0x212D) = (uint8_t)(on ? 0x13 : 0x03);
}

void spr_panel(const uint8_t *cells, int w, int h, int x0, int y0) {
  int cx, cy, i = 0;
  for (cy = 0; cy < h; cy++) {
    for (cx = 0; cx < w; cx++) {
      uint8_t c = cells[cy * w + cx];
      int tile, pal = PAL_NORMAL;
      if (i >= OAM_SPRITES) break;
      if (c == SPR_BORDER) {
        tile = TILE_BORDER;
      } else if (c == SPR_PAD) {
        tile = TILE_PAD;
      } else {
        tile = c & 0x7F;
        if (c & SPR_SELECTED) pal = PAL_SELECTED;
      }
      oam[i * 4 + 0] = (uint8_t)(x0 + cx * 8);
      oam[i * 4 + 1] = (uint8_t)(y0 + cy * 8);
      oam[i * 4 + 2] = (uint8_t)tile;
      /* vhoopppN: priority 3 so the panel sits in front of both text layers */
      oam[i * 4 + 3] = (uint8_t)(0x30 | (pal << 1));
      i++;
    }
  }
  for (; i < OAM_SPRITES; i++) oam[i * 4 + 1] = 240;
}

void spr_commit(void) {
  uint32_t src = (uint32_t)oam;
  while (R8(0x4212) & 0x80) {
  }
  while (!(R8(0x4212) & 0x80)) {
  }
  R8(0x2102) = 0; /* OAM address 0 */
  R8(0x2103) = 0;
  /* Channel 2: channels 0 and 1 belong to the console's tilemap DMA. */
  R8(0x4320) = 0x00; /* A->B, one register */
  R8(0x4321) = 0x04; /* $2104, OAM data */
  R8(0x4322) = (uint8_t)src;
  R8(0x4323) = (uint8_t)(src >> 8);
  R8(0x4324) = (uint8_t)(src >> 16);
  R8(0x4325) = (uint8_t)OAM_BYTES;
  R8(0x4326) = (uint8_t)(OAM_BYTES >> 8);
  R8(0x420B) = 0x04;
}
