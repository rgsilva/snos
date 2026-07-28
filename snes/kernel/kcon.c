/* Kernel console layer — a thin wrapper over console.c (which is reused
 * verbatim from snespdp11 and deliberately not modified).
 *
 * Two things console.c does not do that the kernel needs:
 *
 *  1. con_flush() drains exactly ONE dirty row per call and blocks for vblank
 *     while doing it. That is right for a scrolling emulator display, but a
 *     shell that prints a 10-line help text would leave 9 rows unpainted until
 *     something else happened to call flush again. kcon_sync() loops until the
 *     screen is actually clean.
 *
 *  2. Number formatting. printf is Calypsi's biggest stack consumer and the
 *     kernel stack is 2816 bytes (PLAN.md §2), so the kernel never links it.
 *
 * Everything printed to the console is also mirrored to the test mailbox, so
 * the Lua harness can assert on output without screen-scraping VRAM.
 */
#include "snos.h"
#include "console.h"
#include "mailbox.h"

void kcon_init(void) { con_init(); }

void kcon_putc(int c) {
  con_putc(c);
  mb_putc((char)c);
  /* Push completed lines to the screen as they happen. Without this, output
   * only reaches VRAM when something calls con_flush — which the shell does
   * while waiting for input, but a running application does not. A BASIC
   * program printing in a loop would otherwise show nothing until it stopped. */
  if (c == '\n') con_flush();
}

void kcon_puts(const char *s) {
  while (*s) {
    kcon_putc(*s++);
  }
}

/* Unsigned decimal, no padding. Max 5 digits for uint16_t. */
void kcon_putu(uint16_t v) {
  char buf[6];
  int i = 0;
  if (v == 0) {
    kcon_putc('0');
    return;
  }
  while (v && i < 6) {
    buf[i++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  while (i) {
    kcon_putc(buf[--i]);
  }
}

void kcon_puthex16(uint16_t v) {
  static const char digits[] = "0123456789ABCDEF";
  int i;
  for (i = 12; i >= 0; i -= 4) {
    kcon_putc(digits[(v >> i) & 0xFu]);
  }
}

/* console.c drains one dirty row per call and waits a frame each time, so a
 * full 28-row repaint costs 28 frames (~0.5 s). That is invisible for a shell
 * but matters for tests, which must not assert before the screen settles. */
void kcon_sync(void) {
  int guard;
  for (guard = 0; guard < 64; guard++) {
    con_flush();
  }
}
