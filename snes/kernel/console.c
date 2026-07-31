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
/* font8x8_basic.h is consumed by tools/mkfont.py at build time, not here. */

#define R8(a) (*(volatile uint8_t *)(a))

#define COLS 64
#define ROWS 28
#define RING 32
/* The scroll ring is indexed with & (RING - 1) — a _Mod16 library call per
 * printed character otherwise; ~200-400 cycles each, 1,664 of them in one
 * full-pane repaint. The mask requires a power of two, enforced here. */
typedef char con_ring_must_be_power_of_two[(RING & (RING - 1)) == 0 ? 1 : -1];
#define BG2_MAP 0x0000
#define BG1_MAP 0x0400
#define BG2_CHR 0x1000          /* 2bpp: 8 words/tile */
#define BG1_CHR 0x3000          /* 4bpp: 16 words/tile */

/* A SECOND pair of tilemaps, for the on-screen keyboard panel.
 *
 * The panel cannot live in the text tilemaps: those are scrolled by the
 * hardware scroll registers, so anything in them moves when text scrolls. It
 * gets its own maps and its own FIXED scroll value, and an HDMA split swaps
 * both over partway down the screen. Text scrolls above the split; the panel
 * below it never moves. */
#define BG2_PANEL 0x0800
#define BG1_PANEL 0x0C00

static uint8_t shadow[RING][COLS];
static uint8_t dirty[RING];
static uint8_t top, row, col, vofs_dirty;

/* How many screen rows the scrolling text area gets. Drops to make room for the
 * keyboard panel, so text never scrolls underneath it. */
static uint8_t text_rows = ROWS;

/* Set while the keyboard's HDMA split is active, so con_split_hide() knows
 * there is something to tear down. con_flush no longer consults it: HDMA
 * transfers happen during ACTIVE display, not vblank, so the split costs the
 * flush nothing — measured, the vblank ceiling is the same 10 transfers with
 * the keyboard up as with it down. */
static uint8_t split_on;

/* The attention cell (SYS_NOTIFY). Overlaid at FLUSH time so the kernel wins
 * the cell against any app's writes — nothing to fight over in the shadow. */
static uint8_t notify_on;

/* Set while an application owns the PPU (SYS_GFX). con_flush becomes a no-op:
 * a full-screen image needs all of VRAM, so anything the console wrote would
 * both corrupt the picture and be corrupted by it. */
static uint8_t gfx_owned;

/* Publish the scroll state to the test mailbox ($7EF00C). A test that wants to
 * assert "screen row 24 is blank" has to map screen rows to tilemap rows, and
 * only the console knows the scroll offset. Without this the harness can say
 * what text exists but not where it is — which is exactly the distinction the
 * keyboard-overlap bugs turned on. */
#define CON_STATE (*(volatile __far uint16_t *)0x7EF00Cul)

static void con_publish(void) {
    CON_STATE = (uint16_t)(top | ((uint16_t)text_rows << 8));
}

/* Where the caret was last drawn, as a RING row so it survives scrolling. */
static int caret_ring = -1;
static uint8_t caret_col;

/* Both font banks are baked at build time by tools/mkfont.py, which owns what
 * used to be glyph_row()/rev()/box_glyphs here: the box-drawing slots $01-$06,
 * the LSB-left -> MSB-left bit reversal, and the $80-$FF inverse-video twins
 * (with a two-colour palette, flipping plane 0 turns "green on black" into
 * "black on green" — the OSK's selection highlight and the shell's cursor,
 * without a second BG layer, which Mode 5 does not have spare). Change the
 * glyph logic there, not here. */
extern const uint8_t snos_font_bg2[8192];
extern const uint8_t snos_font_bg1[16384];

static void vaddr(uint16_t a) { R8(0x2116) = (uint8_t)a; R8(0x2117) = (uint8_t)(a >> 8); }
static void vdata(uint16_t v) { R8(0x2118) = (uint8_t)v; R8(0x2119) = (uint8_t)(v >> 8); }

static void hdma_refresh(void);
#define DMA_CH 0
static void dma_vram(int ch, uint16_t vword, const uint16_t *src, uint16_t nwords);

static void set_vofs(void) {
    uint16_t v = (uint16_t)(((top * 8) - 1) & 0x3FF);
    R8(0x210E) = (uint8_t)v; R8(0x210E) = (uint8_t)(v >> 8);   /* BG1VOFS */
    R8(0x2110) = (uint8_t)v; R8(0x2110) = (uint8_t)(v >> 8);   /* BG2VOFS */
    /* When the split is active these direct writes are overwritten by HDMA
     * every frame, so the scroll value has to go into the table as well. */
    hdma_refresh();
}

void con_init(void) {
    int y, i;
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

    /* Fonts from ROM by DMA — cart ROM is on the A-bus, VRAM on the B-bus,
     * the same division that makes image loading fast. This used to be
     * 12,288 port writes plus a glyph_row() computation each, re-run on
     * EVERY app exit (con_gfx(0) lands here): roughly half a second of
     * blank screen per exit, for bytes that never change. */
    dma_vram(DMA_CH, BG2_CHR, (const uint16_t *)snos_font_bg2, 4096);
    dma_vram(DMA_CH, BG1_CHR, (const uint16_t *)snos_font_bg1, 8192);

    vaddr(BG2_MAP);                       /* clear both maps to space (0x20) */
    for (i = 0; i < 32 * 32; i++) vdata((0x20 << 1) | 0x2000);
    vaddr(BG1_MAP);
    for (i = 0; i < 32 * 32; i++) vdata((0x20 << 1) | 0x2000);
    vaddr(BG2_PANEL);
    for (i = 0; i < 32 * 32; i++) vdata((0x20 << 1) | 0x2000);
    vaddr(BG1_PANEL);
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
    if (row < text_rows - 1) { row++; return; }
    top = (uint8_t)((top + 1) & (RING - 1));
    {
        uint8_t r = (uint8_t)((top + text_rows - 1) & (RING - 1));
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
    if (c == '\b') {
        /* Step back onto the previous row when already at column 0. A typed
         * line longer than 64 columns wraps, and stopping at the left edge
         * meant the wrapped part could never be erased — you could delete
         * "mg" from "cover.img" and no further. */
        if (col) col--;
        else if (row) { row--; col = COLS - 1; }
        return;
    }
    if (c == 7) return;
    if (c < 0x20) return;
    r = (uint8_t)((top + row) & (RING - 1));
    shadow[r][col] = (uint8_t)c;
    dirty[r] = 1;
    if (++col >= COLS) newline();
}

void con_puts(const char *s) { while (*s) con_putc(*s++); }

/* A 64-word tilemap row is too many $2118 writes to fit one vblank at -O0, so
 * rows are built in WRAM and DMAd. Several rows go out per vblank: draining one
 * at a time meant a six-row repaint took six frames, which was visible as the
 * console lazily catching up with itself.
 *
 * How many depends on how much of vblank there is — see CON_FLUSH_ROWS in
 * console.h for the measurement. It is NOT about DMA channels: dma_vram writes
 * $420B and the CPU stalls until that single transfer finishes, so transfers
 * are always sequential and one channel does all of them. */
#define FLUSH_MAX CON_FLUSH_ROWS
static uint16_t rowbuf[FLUSH_MAX][32];
static uint16_t rowbuf1[FLUSH_MAX][32];

/* One channel for every transfer. Channels 3, 4 and 5 belong to the keyboard
 * split's HDMA, and channel 0 never does — and since each transfer runs to
 * completion before the next begins, one is all this needs. The old pair of
 * tables existed to ration a resource that was not actually scarce, and rationed
 * the keyboard split down to two rows a frame for no reason. */
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

/* Build the row buffers OUTSIDE vblank (slow C is fine there), then do only the
 * fast DMAs inside it. */
void con_flush(void) {
    uint8_t rows[FLUSH_MAX];
    if (gfx_owned) return;
    int budget, n = 0, i, cell;
    uint8_t r;

    /* The split no longer costs rows. It used to halve them because HDMA had
     * taken three of the eight channels; with a single channel doing every
     * transfer, HDMA's channels are irrelevant. */
    budget = FLUSH_MAX;

    /* A scroll changes what the hardware displays IMMEDIATELY, but the row
     * being revealed at the bottom still holds whatever was in that tilemap row
     * 32 lines ago until we DMA the cleared version over it. Updating the
     * scroll register in one vblank and the row contents in a later one shows
     * that stale row for a frame — the flicker on pressing ENTER. So when a
     * scroll is pending, the revealed row goes out first, in the same vblank as
     * the scroll register. */
    if (vofs_dirty) {
        uint8_t nr = (uint8_t)((top + text_rows - 1) & (RING - 1));
        if (dirty[nr]) rows[n++] = nr;
    }
    for (r = 0; r < RING && n < budget; r++) {
        if (!dirty[r]) continue;
        if (n > 0 && rows[0] == r) continue;   /* already queued above */
        rows[n++] = r;
    }
    if (n == 0 && !vofs_dirty) return;

    for (i = 0; i < n; i++) {
        uint8_t rr = rows[i];
        for (cell = 0; cell < 32; cell++)
            rowbuf[i][cell]  = (uint16_t)((shadow[rr][cell * 2]     << 1) | 0x2000);
        for (cell = 0; cell < 32; cell++)
            rowbuf1[i][cell] = (uint16_t)((shadow[rr][cell * 2 + 1] << 1) | 0x2000);
        /* The attention cell: screen row 0 is ring row `top`, column 63 is
         * BG1 cell 31. Applied to the outgoing buffer, never the shadow, so
         * the app's own content returns untouched when the flag drops. */
        if (notify_on && rr == top)
            rowbuf1[i][31] = (uint16_t)(((0x80 | '!') << 1) | 0x2000);
    }

    con_publish();
    while (R8(0x4212) & 0x80) ;
    while (!(R8(0x4212) & 0x80)) ;                    /* into vblank */
    if (vofs_dirty) { set_vofs(); vofs_dirty = 0; }
    for (i = 0; i < n; i++) {
        /* Stop the moment vblank ends, and leave the row DIRTY when it does.
         *
         * VRAM writes outside vblank are silently ignored, so a transfer that
         * runs past the window paints nothing and the old code still cleared
         * the dirty flag — the row was then never redrawn, at all, ever. That
         * is what made overrunning invisible: no assertion, no glitch, just a
         * row that stays blank or half-painted until something else happens to
         * dirty it again.
         *
         * CON_FLUSH_ROWS is sized so this never triggers in the normal case.
         * It is here so that when something changes the arithmetic — a costlier
         * NMI, a slower emulator, PAL's different window — the failure mode is
         * "this row goes out next frame" rather than "this row is lost". */
        if (!(R8(0x4212) & 0x80)) break;
        dma_vram(DMA_CH, (uint16_t)(BG2_MAP + rows[i] * 32), rowbuf[i], 32);
        dma_vram(DMA_CH, (uint16_t)(BG1_MAP + rows[i] * 32), rowbuf1[i], 32);
        /* Checked AFTER both halves: a pair that straddles the end of vblank
         * lands the BG2 half and drops the BG1 half, which is the "l  s   i e"
         * shape — every other column updated. The row is rebuilt whole next
         * time, so leaving it dirty repairs both halves. */
        if (!(R8(0x4212) & 0x80)) break;
        dirty[rows[i]] = 0;
    }
}

/* ---- random access --------------------------------------------------------
 * The console above is a pure stream device: text goes in, it scrolls. A shell
 * also needs to put a cursor where the caret is, and the keyboard panel needs
 * to address rows directly. These take SCREEN coordinates and resolve the
 * scroll ring themselves, so callers never see it.
 */

void con_notify(int on) {
    if (notify_on == (on ? 1 : 0)) return;
    notify_on = (uint8_t)(on ? 1 : 0);
    dirty[top] = 1; /* repaint screen row 0: draw the cell, or erase it */
}

int con_rows(void) { return ROWS; }
int con_text_rows(void) { return text_rows; }
int con_cols(void) { return COLS; }
void con_getpos(int *r, int *c) { *r = row; *c = col; }

void con_putat(int r_, int c_, int ch) {
    uint8_t r;
    if (r_ < 0 || r_ >= ROWS || c_ < 0 || c_ >= COLS) return;
    r = (uint8_t)((top + r_) & (RING - 1));
    shadow[r][c_] = (uint8_t)ch;
    dirty[r] = 1;
}

int con_getat(int r_, int c_) {
    if (r_ < 0 || r_ >= ROWS || c_ < 0 || c_ >= COLS) return ' ';
    return shadow[(top + r_) & (RING - 1)][c_];
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
    caret_ring = -1;
    vofs_dirty = 1;
}

/* ---- keyboard panel: a second tilemap, held still by an HDMA split -------
 *
 * The console scrolls BG1 and BG2 with the vertical scroll registers, so
 * anything drawn into the text tilemaps moves when text scrolls. That is what
 * made the old keyboard drift up through the display and corrupt it.
 *
 * The fix is the standard SNES one: an HDMA table rewrites the tilemap-base and
 * scroll registers partway down the frame. Above the split the hardware sees
 * the scrolling text maps; below it, the panel maps at a fixed scroll offset.
 * Text scrolls underneath a panel that cannot move, and neither knows about the
 * other.
 *
 * Three channels, because the registers are not contiguous:
 *   3 -> $2107/$2108  (BG1SC, BG2SC)   mode 1, two registers
 *   4 -> $210E        (BG1VOFS)        mode 2, one register written twice
 *   5 -> $2110        (BG2VOFS)        mode 2
 * Channels 0 and 1 stay reserved for the tilemap DMA in con_flush.
 */

#define SC_TEXT_BG1 ((BG1_MAP >> 10) << 2)
#define SC_TEXT_BG2 ((BG2_MAP >> 10) << 2)
#define SC_PANEL_BG1 ((BG1_PANEL >> 10) << 2)
#define SC_PANEL_BG2 ((BG2_PANEL >> 10) << 2)

/* The panel map is read with a fixed scroll, so panel row R appears at screen
 * row R and the panel can simply be drawn where it belongs. */
#define PANEL_VOFS 0x3FFu /* same -1 convention as set_vofs() */

/* Each table is: [lines][data..] [lines][data..] [lines][data..] 0.
 *
 * Every entry carries TWO data bytes — mode 1 writes two consecutive registers
 * ($2107 then $2108), mode 2 writes one register twice — and the table must be
 * NUL-terminated. Getting either wrong is not subtle in a fun way: one missing
 * byte left BG2's tilemap base reading garbage, so the panel rendered with
 * every second character missing (Mode 5 draws odd columns from BG1 and even
 * ones from BG2). Three entries plus terminator = 10 bytes. */
#define HDMA_TBL 10
static uint8_t hdma_sc[HDMA_TBL];
static uint8_t hdma_v1[HDMA_TBL];
static uint8_t hdma_v2[HDMA_TBL];
static uint8_t split_row;

void hdma_refresh(void);

static void hdma_build(void) {
    uint16_t tv = (uint16_t)(((top * 8) - 1) & 0x3FF);
    uint8_t above = (uint8_t)(split_row * 8);      /* scanlines of text */
    uint8_t first = above > 127 ? 127 : above;
    uint8_t second = (uint8_t)(above - first);

    uint8_t below = (uint8_t)(224 - above);
    int i;

    hdma_sc[0] = first;  hdma_sc[1] = SC_TEXT_BG1;  hdma_sc[2] = SC_TEXT_BG2;
    hdma_sc[3] = second; hdma_sc[4] = SC_TEXT_BG1;  hdma_sc[5] = SC_TEXT_BG2;
    hdma_sc[6] = below;  hdma_sc[7] = SC_PANEL_BG1; hdma_sc[8] = SC_PANEL_BG2;
    hdma_sc[9] = 0;

    hdma_v1[0] = first;  hdma_v1[1] = (uint8_t)tv;  hdma_v1[2] = (uint8_t)(tv >> 8);
    hdma_v1[3] = second; hdma_v1[4] = (uint8_t)tv;  hdma_v1[5] = (uint8_t)(tv >> 8);
    hdma_v1[6] = below;
    hdma_v1[7] = (uint8_t)PANEL_VOFS;
    hdma_v1[8] = (uint8_t)(PANEL_VOFS >> 8);
    hdma_v1[9] = 0;

    for (i = 0; i < HDMA_TBL; i++) hdma_v2[i] = hdma_v1[i];
}

static void hdma_channel(int ch, uint8_t mode, uint8_t breg, const uint8_t *tbl) {
    uint16_t base = (uint16_t)(0x4300 + ch * 0x10);
    uint32_t a = (uint32_t)tbl;
    R8(base + 0) = mode;
    R8(base + 1) = breg;
    R8(base + 2) = (uint8_t)a;
    R8(base + 3) = (uint8_t)(a >> 8);
    R8(base + 4) = (uint8_t)(a >> 16);
}

/* Re-emit the text half of the HDMA tables after a scroll. */
void hdma_refresh(void) {
    uint16_t tv;
    if (!split_on) return;
    tv = (uint16_t)(((top * 8) - 1) & 0x3FF);
    hdma_v1[1] = (uint8_t)tv; hdma_v1[2] = (uint8_t)(tv >> 8);
    hdma_v1[4] = (uint8_t)tv; hdma_v1[5] = (uint8_t)(tv >> 8);
    hdma_v2[1] = (uint8_t)tv; hdma_v2[2] = (uint8_t)(tv >> 8);
    hdma_v2[4] = (uint8_t)tv; hdma_v2[5] = (uint8_t)(tv >> 8);
}

/* Resize the scrolling text area, reconciling the rows that move in or out of
 * view. Both directions need work, and neither is optional:
 *
 * Shrinking (the keyboard opens): just clamping the cursor would leave the
 * lines below it stranded behind the panel, and the cursor would land on top of
 * existing text. Instead the whole view scrolls up by the difference, so the
 * cursor line ends up on the last visible row and nothing is lost — the text
 * moves up out of the keyboard's way, which is what it looks like it should do.
 *
 * Growing (the keyboard closes): the rows coming back into view still hold
 * whatever the ring left there — text from many screens ago, never cleared
 * because it was never visible. Revealing it puts ancient output at the bottom
 * of the screen, so those rows are blanked first. */
void con_set_text_rows(int n) {
    int i, j;
    if (n < 1) n = 1;
    if (n > ROWS) n = ROWS;

    if (n < text_rows) {
        if (row >= n) {
            uint8_t shift = (uint8_t)(row - (n - 1));
            top = (uint8_t)((top + shift) & (RING - 1));
            row = (uint8_t)(n - 1);
            vofs_dirty = 1;
        }
    } else if (n > text_rows) {
        for (i = text_rows; i < n; i++) {
            uint8_t r = (uint8_t)((top + i) & (RING - 1));
            for (j = 0; j < COLS; j++) shadow[r][j] = ' ';
            dirty[r] = 1;
        }
    }
    text_rows = (uint8_t)n;
    con_publish();
}

/* The caret belongs to the console, not to the line editor.
 *
 * The editor used to draw it with con_putat() and erase it at the cursor's
 * CURRENT position — but showing or hiding the keyboard scrolls the view
 * between those two moments, so the erase missed and left a stray block behind.
 * Remembering the ring row makes the erase land where the caret actually is, no
 * matter what the view did in between. */
void con_caret(int on) {
    if (caret_ring >= 0) {
        shadow[caret_ring][caret_col] = ' ';
        dirty[caret_ring] = 1;
        caret_ring = -1;
    }
    if (on) {
        caret_ring = (top + row) & (RING - 1);
        caret_col = col;
        shadow[caret_ring][caret_col] = 0x80 | ' ';
        dirty[caret_ring] = 1;
    }
}

void con_split_enable(int panel_top_row) {
    split_row = (uint8_t)panel_top_row;
    split_on = 1;
    con_set_text_rows(panel_top_row);

    hdma_build();
    hdma_channel(3, 0x01, 0x07, hdma_sc);
    hdma_channel(4, 0x02, 0x0E, hdma_v1);
    hdma_channel(5, 0x02, 0x10, hdma_v2);
    R8(0x420C) = 0x38; /* channels 3, 4, 5 */
}

void con_split_disable(void) {
    split_on = 0;
    R8(0x420C) = 0x00;
    con_set_text_rows(ROWS);
    /* HDMA left the registers wherever the last scanline put them. */
    R8(0x2107) = SC_TEXT_BG1;
    R8(0x2108) = SC_TEXT_BG2;
    vofs_dirty = 1;
    con_invalidate_all();
}

/* Does the display still owe VRAM a write?
 *
 * The NMI handler uses this to decide whether it may spend the vblank on
 * anything else. con_flush() runs in the FOREGROUND and does its DMA inside the
 * vblank window, but NMI fires at the top of that window and preempts it — so
 * any long-running NMI work consumes the window and the DMA silently lands
 * during active display, where VRAM writes are ignored. The symptom is not a
 * crash or a glitch: the screen simply stops updating while the mailbox shows
 * everything working perfectly. */
/* Hand VRAM to an application, or take it back and rebuild.
 *
 * Releasing re-runs con_init(), which reuploads the font and both tilemaps and
 * clears the shadow. Scrollback is lost, which is the honest outcome: the image
 * genuinely overwrote those bytes. */
void con_gfx(int acquire) {
    if (acquire) {
        gfx_owned = 1;
        return;
    }
    gfx_owned = 0;
    con_init();
}

int con_dirty_pending(void) {
    int i;
    /* An application owning the PPU owes VRAM nothing, so it must never look
     * busy. con_flush() is a no-op while gfx_owned, which means dirty rows can
     * never be cleared — and NMI defers the network on exactly this answer. Say
     * "busy" here and the link dies for the entire life of the application.
     *
     * That is not hypothetical: `vnc` printed two lines, took the PPU, and the
     * link stopped mid-handshake and never restarted. It took a thread dump of
     * the gateway to see it, because from the far side the console had simply
     * gone quiet. Every earlier app downloaded FIRST and displayed AFTER, so
     * nothing had ever needed the network while holding the screen. */
    if (gfx_owned) return 0;
    if (vofs_dirty) return 1;
    for (i = 0; i < RING; i++)
        if (dirty[i]) return 1;
    return 0;
}

void con_invalidate_all(void) {
    int i;
    for (i = 0; i < RING; i++) dirty[i] = 1;
}

/* Write one row of the PANEL tilemaps. Screen coordinates; the panel map is
 * unscrolled, so panel row R is simply tilemap row R.
 *
 * Returns whether the transfer finished inside vblank — the same rule
 * con_flush enforces, and for the same reason: NMI fires at the top of the
 * window and net_service can consume most of it, after which these DMAs land
 * during active display where VRAM writes are silently ignored. The caller
 * must keep the row dirty on 0 or it stays half-painted forever. */
int con_panel_row(int screen_row, const uint8_t *chars) {
    int cell;
    if (screen_row < 0 || screen_row >= ROWS) return 1;
    for (cell = 0; cell < 32; cell++) {
        rowbuf[0][cell]  = (uint16_t)((chars[cell * 2]     << 1) | 0x2000);
        rowbuf1[0][cell] = (uint16_t)((chars[cell * 2 + 1] << 1) | 0x2000);
    }
    while (R8(0x4212) & 0x80) ;
    while (!(R8(0x4212) & 0x80)) ;
    dma_vram(0, (uint16_t)(BG2_PANEL + screen_row * 32), rowbuf[0], 32);
    dma_vram(1, (uint16_t)(BG1_PANEL + screen_row * 32), rowbuf1[0], 32);
    return (R8(0x4212) & 0x80) ? 1 : 0;
}
