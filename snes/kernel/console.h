/* Mode 5 hires text console: 64 cols x 28 rows, 8px font, hardware-scroll
 * ring. Own module — the keyboard/ and basic-rom/ projects stay untouched. */
#ifndef CONSOLE_H
#define CONSOLE_H

void con_init(void);
void con_putc(int c);
void con_puts(const char *s);
void con_flush(void);          /* copy dirty rows to VRAM during vblank */

#endif
