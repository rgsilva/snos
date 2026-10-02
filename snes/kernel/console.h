/* Text console: 28 rows of 8px glyphs on a hardware-scroll ring, 64 columns
 * in Mode 5 hires or 32 in Mode 0 (con_set_cols; the shell's `mode`). Reused
 * from snespdp11; the SNOS additions live at the bottom of console.c and
 * leave the original stream interface untouched.
 *
 * Characters $80-$FF are the inverse-video twins of $00-$7F (tools/mkfont.py).
 * Mode 5 gives us exactly two BG layers and both are carrying text columns,
 * so inverse glyphs are how highlighting happens — there is no spare layer to
 * do it with.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

void con_init(void);
void con_putc(int c);
void con_puts(const char *s);
/* How many rows one con_flush can move in a single vblank. Callers that decide
 * WHEN to flush need this too: flushing more often than every CON_FLUSH_ROWS
 * dirty rows pays a whole vblank to move less than a flush is capable of, and
 * flushing LESS often leaves rows sitting unpainted until the next batch. The
 * batch size in kcon.c and the budget in console.c are therefore the same
 * number, deliberately.
 *
 * MEASURED, not estimated. This was briefly 8, on the arithmetic that NTSC
 * vblank is ~51,800 master cycles and DMA moves a byte every 8, so the 128
 * bytes of a row across both layers leaves room for around 40. That counts the
 * bytes and forgets the code: dma_vram is eight far register writes compiled at
 * -O0, and the setup costs roughly nine times what the transfer does. Counting
 * $4212 inside the loop with the budget raised, a flush asking for 16 transfers
 * completed TEN before vblank ended — five rows, not forty. The rest painted
 * nothing, and `help` came out with rows missing and one row showing only its
 * even-numbered columns.
 *
 * 4 rows = 8 transfers, comfortably inside the measured 10, and it is the value
 * that was in service for the whole life of the console before the arithmetic
 * above talked it upwards. con_flush also stops on the vblank flag now, so
 * being wrong here costs a frame rather than a row.
 *
 * At 32 columns a row is ONE transfer (BG2 only), so the same window moves
 * more rows. Measured the same way on 2026-10-02 (Mesen, emulator ROM, link
 * idle): with the budget raised to 16 rows, each of 59 full flushes landed 8
 * or 9 before vblank ended. The same probe at 64 columns landed 4 or 5 rows
 * (8-10 transfers), which is the figure above reproduced, so 8 here sits
 * exactly where 4 sits there. con_flush_rows() is whichever applies to the
 * live mode, and it is what kcon.c batches by. */
#define CON_FLUSH_ROWS 4
#define CON_FLUSH_ROWS_NARROW 8
int con_flush_rows(void);
void con_flush(void); /* copy up to con_flush_rows() dirty rows during vblank */

/* Text colours: an index into console.c's palette, spelled the way `mode`
 * and the Makefile's CON_COLOUR spell them. */
#define CON_COLOUR_white 0
#define CON_COLOUR_green 1
#define CON_COLOUR_mint 2
#define CON_COLOUR_amber 3
#define CON_NCOLOURS 4

/* The power-on mode. The Makefile sets these (CON_COLS, CON_COLOUR); the
 * fallbacks are the emulator ROM's, which every screen-reading test assumes. */
#ifndef CON_COLS_DEFAULT
#define CON_COLS_DEFAULT 64
#endif
#ifndef CON_COLOUR_DEFAULT
#define CON_COLOUR_DEFAULT CON_COLOUR_green
#endif

/* Width, 32 or 64. Changing it rebuilds the console from nothing (con_init):
 * the ring keeps no record of where lines broke, so scrollback cannot be
 * re-wrapped honestly. It also takes the PPU back from an application that
 * held it. Kernel callers go through kcon_mode, which handles the keyboard
 * panel and the scheduler around it. */
void con_set_cols(int cols);
/* Colour only: a palette write at the next flush, the text stays put. */
void con_set_colour(int colour);
int con_colour(void);
const char *con_colour_name(int colour); /* 0 past the last */

/* SNOS additions: random access, for the on-screen keyboard panel and the
 * shell cursor. Row/column are SCREEN coordinates; the scroll ring is resolved
 * internally so callers never see it. */
int con_rows(void);
int con_cols(void); /* the live width, 32 or 64; the shadow is always 64 */
void con_getpos(int *r, int *c);
void con_putat(int r, int c, int ch);
int con_getat(int r, int c);
void con_row_save(int r, uint8_t *dst);
int con_gfx_owned(void); /* 1 while an app owns the PPU */
void con_row_restore(int r, const uint8_t *src);
void con_clear(void);

/* Keyboard panel. It lives in a SECOND pair of tilemaps held still by an HDMA
 * split, so text scrolls underneath it and it never moves. Box-drawing glyphs
 * are available as characters $01-$06 (h, v, and the four corners). */
void con_set_text_rows(int n);
int con_text_rows(void); /* usable height; shrinks when the keyboard shows */
void con_caret(int on);
/* The attention cell: while on, con_flush overlays an inverse '!' in the
 * top-right cell of screen row 0 (column 63, or 31 at 32 columns) — the
 * kernel wins that one cell over any application's write, which is what
 * makes it trustworthy. */
void con_notify(int on);
void con_split_enable(int panel_top_row);
void con_split_disable(void);
int con_panel_row(int screen_row, const uint8_t *chars); /* 1 = landed in vblank */
/* The panel rows not yet in VRAM, a bit per row from the panel's top. The
 * keyboard (input.c) sets and clears them; they live here because
 * con_dirty_pending must count them, and it runs in NMI, so a plain byte. */
extern uint8_t con_panel_dirty;
void con_invalidate_all(void);
void con_gfx(int acquire);

/* True while con_flush still has VRAM work to do. The NMI handler must not
 * spend the vblank on background work while this is set — see console.c. */
int con_dirty_pending(void);

/* Screen rows whose shadow changed since the previous call: *lo bits 0-15 =
 * rows 0-15, *hi bits 0-11 = rows 16-27. Read-and-clear — ONE consumer
 * (SYS_CONDIRTY, i.e. the screen mirror). A scroll or clear reports every
 * row, because the content of every row genuinely moved. */
void con_changes(uint16_t *lo, uint16_t *hi);

#define CON_BOX_H 1
#define CON_BOX_V 2
#define CON_BOX_TL 3
#define CON_BOX_TR 4
#define CON_BOX_BL 5
#define CON_BOX_BR 6

#endif
