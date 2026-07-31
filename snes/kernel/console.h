/* Mode 5 hires text console: 64 cols x 28 rows, 8px font, hardware-scroll
 * ring. Reused from snespdp11; the SNOS additions live at the bottom of
 * console.c and leave the original stream interface untouched.
 *
 * Characters $80-$FF are the inverse-video twins of $00-$7F (see glyph_row in
 * console.c). Mode 5 gives us exactly two BG layers and both are carrying text
 * columns, so inverse glyphs are how highlighting happens — there is no spare
 * layer to do it with.
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
 * being wrong here costs a frame rather than a row. */
#define CON_FLUSH_ROWS 4
void con_flush(void); /* copy up to CON_FLUSH_ROWS dirty rows during vblank */

/* SNOS additions: random access, for the on-screen keyboard panel and the
 * shell cursor. Row/column are SCREEN coordinates; the scroll ring is resolved
 * internally so callers never see it. */
int con_rows(void);
int con_cols(void);
void con_getpos(int *r, int *c);
void con_putat(int r, int c, int ch);
int con_getat(int r, int c);
void con_row_save(int r, uint8_t *dst);
void con_row_restore(int r, const uint8_t *src);
void con_clear(void);

/* Keyboard panel. It lives in a SECOND pair of tilemaps held still by an HDMA
 * split, so text scrolls underneath it and it never moves. Box-drawing glyphs
 * are available as characters $01-$06 (h, v, and the four corners). */
void con_set_text_rows(int n);
int con_text_rows(void); /* usable height; shrinks when the keyboard shows */
void con_caret(int on);
void con_split_enable(int panel_top_row);
void con_split_disable(void);
int con_panel_row(int screen_row, const uint8_t *chars); /* 1 = landed in vblank */
void con_invalidate_all(void);
void con_gfx(int acquire);

/* True while con_flush still has VRAM work to do. The NMI handler must not
 * spend the vblank on background work while this is set — see console.c. */
int con_dirty_pending(void);

#define CON_BOX_H 1
#define CON_BOX_V 2
#define CON_BOX_TL 3
#define CON_BOX_TR 4
#define CON_BOX_BL 5
#define CON_BOX_BR 6

#endif
