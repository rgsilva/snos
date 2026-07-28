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
void con_flush(void); /* copy one dirty row to VRAM during vblank */

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

#endif
