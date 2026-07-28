/* Text console: 64 cols x 28 rows, Mode 5 hires, hardware-scroll ring.
 * Own module — keyboard/ and basic-rom/ stay untouched.
 *
 * Mode 5 is 512-dot hires: each tilemap cell is 16px wide and fetches an 8x8
 * tile PAIR (N, N+1). We split each cell into two text columns using two
 * layers, the emu8086/dosboot.asm technique (see snes/tools notes):
 *   - BG2 (2bpp) holds EVEN text columns: font tile pair = [glyph][blank]
 *   - BG1 (4bpp) holds ODD  text columns: font tile pair = [blank][glyph]
 * transparency + priority composite them, giving 64 readable 8px columns.
 *
 * Tilemaps (32x32 each): BG2 at word $0000, BG1 at word $0400. Row r -> +r*32.
 * A map entry for char c = (2c) | $2000(priority); it fetches tiles 2c,2c+1.
 * Fonts: BG2 chars at $1000 (2bpp), BG1 chars at $3000 (4bpp); both build
 * their [glyph][blank]/[blank][glyph] pairs from font8x8_basic. Color 1 =
 * green, shared via CGRAM[1] by both bit depths.
 *
 * The 32-row map is a ring: scroll bumps BG1/BG2 VOFS by 8 and clears the
 * wrapped row, so a scroll dirties one row, not 28.
 */
#include <stdint.h>
#include "console.h"
#include "font8x8_basic.h"

#define R8(a) (*(volatile uint8_t *)(a))

#define COLS 64
#define ROWS 28
#define RING 32
#define BG2_MAP 0x0000
#define BG1_MAP 0x0400
#define BG2_CHR 0x1000          /* 2bpp: 8 words/tile */
#define BG1_CHR 0x3000          /* 4bpp: 16 words/tile */

static uint8_t shadow[RING][COLS];
static uint8_t dirty[RING];
static uint8_t top, row, col, vofs_dirty;

static void vaddr(uint16_t a) { R8(0x2116) = (uint8_t)a; R8(0x2117) = (uint8_t)(a >> 8); }
static void vdata(uint16_t v) { R8(0x2118) = (uint8_t)v; R8(0x2119) = (uint8_t)(v >> 8); }

static uint8_t rev(uint8_t b) {  /* font8x8 is LSB-left; SNES wants MSB-left */
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* Glyph bitmap row for character `ch`. Characters $80-$FF are the inverse-video
 * twins of $00-$7F: with a two-colour palette, flipping plane 0 turns "green on
 * black" into "black on green". That gives the OSK its selection highlight and
 * the shell its cursor without a second BG layer — which Mode 5 does not have
 * spare, since BG1 and BG2 are both carrying text columns. */
static uint16_t glyph_row(int ch, int y) {
    uint8_t g = rev((uint8_t)font8x8_basic[ch & 0x7F][y]);
    if (ch & 0x80) g = (uint8_t)~g;
    return g;
}

static void set_vofs(void) {
    uint16_t v = (uint16_t)(((top * 8) - 1) & 0x3FF);
    R8(0x210E) = (uint8_t)v; R8(0x210E) = (uint8_t)(v >> 8);   /* BG1VOFS */
    R8(0x2110) = (uint8_t)v; R8(0x2110) = (uint8_t)(v >> 8);   /* BG2VOFS */
}

void con_init(void) {
    int ch, y, i;
    R8(0x2100) = 0x8F;                    /* force blank */
    R8(0x2105) = 0x05;                    /* Mode 5 */
    R8(0x2107) = (uint8_t)((BG1_MAP >> 10) << 2);  /* BG1SC: map $0400, 32x32 */
    R8(0x2108) = (uint8_t)((BG2_MAP >> 10) << 2);  /* BG2SC: map $0000, 32x32 */
    R8(0x210B) = 0x13;                    /* BG1 chr $3000, BG2 chr $1000 */
    R8(0x212C) = 0x03;                    /* main: BG1+BG2 */
    R8(0x212D) = 0x03;                    /* sub:  BG1+BG2 */
    R8(0x2130) = 0x30; R8(0x2131) = 0x00; /* no color math */
    R8(0x2133) = 0x00;
    R8(0x2121) = 0;                       /* CGRAM: 0=black, 1=green */
    R8(0x2122) = 0x00; R8(0x2122) = 0x00;
    R8(0x2122) = 0xE0; R8(0x2122) = 0x03;
    R8(0x2115) = 0x80;                    /* VRAM inc after high byte */

    /* BG2 font at $1000, 2bpp: char c -> tile 2c = glyph, 2c+1 = blank */
    vaddr(BG2_CHR);
    for (ch = 0; ch < 256; ch++) {
        for (y = 0; y < 8; y++) vdata(glyph_row(ch, y));  /* glyph */
        for (y = 0; y < 8; y++) vdata(0);                 /* blank */
    }
    /* BG1 font at $3000, 4bpp: char c -> tile 2c = blank, 2c+1 = glyph */
    vaddr(BG1_CHR);
    for (ch = 0; ch < 256; ch++) {
        for (y = 0; y < 16; y++) vdata(0);                /* blank tile */
        for (y = 0; y < 8; y++) vdata(glyph_row(ch, y));  /* glyph plane0/1 */
        for (y = 0; y < 8; y++) vdata(0);                 /* glyph plane2/3 */
    }

    vaddr(BG2_MAP);                       /* clear both maps to space (0x20) */
    for (i = 0; i < 32 * 32; i++) vdata((0x20 << 1) | 0x2000);
    vaddr(BG1_MAP);
    for (i = 0; i < 32 * 32; i++) vdata((0x20 << 1) | 0x2000);

    for (y = 0; y < RING; y++) {
        for (i = 0; i < COLS; i++) shadow[y][i] = ' ';
        dirty[y] = 0;
    }
    top = 0; row = 0; col = 0;
    set_vofs();
    vofs_dirty = 0;
    R8(0x2100) = 0x0F;                    /* display on */
}

static void newline(void) {
    int i;
    col = 0;
    if (row < ROWS - 1) { row++; return; }
    top = (uint8_t)((top + 1) % RING);
    {
        uint8_t r = (uint8_t)((top + ROWS - 1) % RING);
        for (i = 0; i < COLS; i++) shadow[r][i] = ' ';
        dirty[r] = 1;
    }
    vofs_dirty = 1;
}

void con_putc(int c) {
    uint8_t r;
    c &= 0xFF;
    if (c == '\n') { newline(); return; }
    if (c == '\r') { col = 0; return; }
    if (c == '\b') { if (col) col--; return; }
    if (c == 7) return;
    if (c < 0x20) return;
    r = (uint8_t)((top + row) % RING);
    shadow[r][col] = (uint8_t)c;
    dirty[r] = 1;
    if (++col >= COLS) newline();
}

void con_puts(const char *s) { while (*s) con_putc(*s++); }

/* A 64-word tilemap row is too many $2118 writes to fit one vblank at -O0, so
 * build the row in a WRAM buffer and DMA it (channel 0) — a row lands in a few
 * hundred cycles, comfortably inside vblank. (The truncated-BG1 bug.) */
static uint16_t rowbuf[32];

/* ch = 0..7; re-triggering the SAME channel back-to-back drops the 2nd
 * transfer, so the two rows-per-cell use different channels. */
static uint16_t rowbuf1[32];
static void dma_vram(int ch, uint16_t vword, const uint16_t *src, uint16_t nwords) {
    uint16_t base = (uint16_t)(0x4300 + ch * 0x10);
    uint32_t s = (uint32_t)src;
    uint16_t bytes = (uint16_t)(nwords * 2);
    R8(0x2116) = (uint8_t)vword;
    R8(0x2117) = (uint8_t)(vword >> 8);
    R8(base + 0) = 0x01;                  /* A->B, mode 1 (l,h -> $2118,$2119) */
    R8(base + 1) = 0x18;                  /* B-bus = $2118 */
    R8(base + 2) = (uint8_t)s;
    R8(base + 3) = (uint8_t)(s >> 8);
    R8(base + 4) = (uint8_t)(s >> 16);    /* source bank */
    R8(base + 5) = (uint8_t)bytes;
    R8(base + 6) = (uint8_t)(bytes >> 8);
    R8(0x420B) = (uint8_t)(1 << ch);      /* start channel ch */
}

/* Refill the row buffers OUTSIDE vblank (slow C is fine here), then do only
 * the two fast DMAs inside vblank. One row per call — the console updates far
 * slower than con_flush is polled, so rows drain over successive calls. */
void con_flush(void) {
    int cell, found = -1;
    uint8_t r;

    /* A scroll changes what the hardware displays IMMEDIATELY, but the row
     * being revealed at the bottom still holds whatever was in that tilemap row
     * 32 lines ago until we DMA the cleared version over it. Updating the
     * scroll register in one vblank and the row contents in a later one shows
     * that stale row for a frame — the flicker on pressing ENTER.
     *
     * So when a scroll is pending, the revealed row jumps the queue and goes
     * out in the SAME vblank as the scroll register. */
    if (vofs_dirty) {
        uint8_t nr = (uint8_t)((top + ROWS - 1) % RING);
        if (dirty[nr]) found = nr;
    }
    if (found < 0)
        for (r = 0; r < RING; r++) if (dirty[r]) { found = r; break; }
    if (found < 0 && !vofs_dirty) return;

    if (found >= 0) {                                 /* build buffers (no vblank yet) */
        for (cell = 0; cell < 32; cell++)
            rowbuf[cell]  = (uint16_t)((shadow[found][cell * 2]     << 1) | 0x2000);
        for (cell = 0; cell < 32; cell++)
            rowbuf1[cell] = (uint16_t)((shadow[found][cell * 2 + 1] << 1) | 0x2000);
    }
    while (R8(0x4212) & 0x80) ;
    while (!(R8(0x4212) & 0x80)) ;                    /* into vblank */
    if (vofs_dirty) { set_vofs(); vofs_dirty = 0; }
    if (found >= 0) {
        dma_vram(0, (uint16_t)(BG2_MAP + found * 32), rowbuf, 32);
        dma_vram(1, (uint16_t)(BG1_MAP + found * 32), rowbuf1, 32);
        dirty[found] = 0;
    }
}

/* ---- additions for SNOS -------------------------------------------------
 * The console above is a pure stream device: text goes in, it scrolls. A shell
 * also needs random access (to draw the on-screen keyboard panel over the
 * bottom rows, and to put a cursor where the caret is), plus a way to put back
 * what the panel covered. All of these address the SCREEN row, translating
 * through the scroll ring themselves, so callers never see `top`.
 */

int con_rows(void) { return ROWS; }
int con_cols(void) { return COLS; }
void con_getpos(int *r, int *c) { *r = row; *c = col; }

void con_putat(int r_, int c_, int ch) {
    uint8_t r;
    if (r_ < 0 || r_ >= ROWS || c_ < 0 || c_ >= COLS) return;
    r = (uint8_t)((top + r_) % RING);
    shadow[r][c_] = (uint8_t)ch;
    dirty[r] = 1;
}

int con_getat(int r_, int c_) {
    if (r_ < 0 || r_ >= ROWS || c_ < 0 || c_ >= COLS) return ' ';
    return shadow[(top + r_) % RING][c_];
}

void con_row_save(int r_, uint8_t *dst) {
    int i;
    for (i = 0; i < COLS; i++) dst[i] = (uint8_t)con_getat(r_, i);
}

void con_row_restore(int r_, const uint8_t *src) {
    int i;
    for (i = 0; i < COLS; i++) con_putat(r_, i, src[i]);
}

void con_clear(void) {
    int y, i;
    for (y = 0; y < RING; y++) {
        for (i = 0; i < COLS; i++) shadow[y][i] = ' ';
        dirty[y] = 1;
    }
    top = 0; row = 0; col = 0;
    vofs_dirty = 1;
}
