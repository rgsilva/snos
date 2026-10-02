/* Kernel console layer — a thin wrapper over console.c.
 *
 * (console.c came from snespdp11 and was long described here as "reused
 * verbatim and deliberately not modified". It is neither any more: the flush
 * path has been reworked for this project. Saying otherwise sent at least one
 * reader looking for a constraint that no longer exists.)
 *
 * Two things console.c does not do that the kernel needs:
 *
 *  1. con_flush() moves up to con_flush_rows() dirty rows per call and blocks for
 *     vblank while doing it, so anything printing more than that in one go is
 *     left partly unpainted until something calls flush again. kcon_sync()
 *     loops until the screen is actually clean.
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
#include "input.h"
#include "proc.h"

void kcon_init(void) { con_init(); }

/* Completed lines not yet pushed to VRAM.
 *
 * This used to flush on EVERY newline, which is why the row budget never
 * mattered: one row was dirty when the flush ran, so a flush capable of moving
 * four moved one and paid a whole vblank to do it. Measured, `help` painted one
 * line every two frames — thirty frames for fifteen lines.
 *
 * Waiting for a full batch keeps the reason the original flushed at all: a
 * running application still shows its output, just in groups of CON_FLUSH_ROWS
 * lines instead of one. Anything that stops to wait for input drains
 * completely, because kbd_idle flushes every frame while it waits.
 *
 * The batch size is con_flush_rows() and not a number of its own: batching
 * MORE than a flush can move leaves the surplus sitting unpainted until the
 * next batch trips, which is its own version of the bug this replaced. */
static uint8_t pending_rows;

void kcon_putc(int c) {
  con_putc(c);
  mb_putc((char)c);
  if (c == '\n' && ++pending_rows >= con_flush_rows()) {
    pending_rows = 0;
    con_flush();
  }
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

/* 32-bit decimal. The SRAM volume is 254 KB, so a downloaded file's size does
 * not fit in the 16-bit kcon_putu and would silently wrap past 65535. */
void kcon_putu32(uint32_t v) {
  char buf[11];
  int i = 0;
  if (v == 0) {
    kcon_putc('0');
    return;
  }
  while (v && i < 11) {
    buf[i++] = (char)('0' + (uint16_t)(v % 10ul));
    v /= 10ul;
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
  /* Everything queued is going out now, so the batch counter starts again —
   * otherwise the next line printed could trip a flush that has nothing to do. */
  pending_rows = 0;
  for (guard = 0; guard < 64; guard++) {
    con_flush();
  }
}

/* Change the text mode: the `mode` command, and the switch around an app
 * that needs 64 columns.
 *
 * A colour change is a palette write and leaves the text alone. A width
 * change rebuilds the console from nothing -- the ring keeps no record of
 * where lines broke, so there is no honest way to re-wrap scrollback -- and
 * the keyboard panel has to come down first: hiding it is what turns the
 * HDMA split off and gives the text all 28 rows back, and it goes back up
 * afterwards laid out for the new width. Pinned against a context switch
 * because a background mirror reading the shadow mid-rebuild would see half
 * of each. */
void kcon_mode(int cols, int colour) {
  int osk;
  con_set_colour(colour);
  if (cols == con_cols() && !con_gfx_owned()) return;
  osk = osk_visible();
  if (osk) osk_hide();
  proc_lock();
  con_set_cols(cols);
  proc_unlock();
  if (osk) osk_show();
  kcon_sync();
}

/* Put the console back the way the shell had it once a foreground program is
 * gone, however it went. An app killed while it held the PPU (SYS_GFX) never
 * released it: con_flush stayed a no-op and the screen never repainted again,
 * so the reclaim is unconditional about that, and about the keyboard lock. */
void kcon_reclaim(int cols) {
  osk_lock(0);
  kcon_mode(cols, con_colour());
}
