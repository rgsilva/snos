/* SNOS input layer.
 *
 * PLAN.md §1 locks this abstraction in from M1: everything above it sees ASCII
 * from kbd_getchar(), and never touches $4218 or an OSK table directly. That is
 * what lets the same shell and the same apps be driven by
 *
 *   - the on-screen keyboard          (now — the only thing a stock SNES has),
 *   - the Mesen Lua harness           (now — how the tests type, with no human),
 *   - a real USB keyboard over the    (M7 — the Pico bridge protocol already
 *     controller port                  proven in ../keyboard-test-rom),
 *
 * without any of them knowing the difference.
 */
#ifndef SNOS_INPUT_H
#define SNOS_INPUT_H

#include <stdint.h>

/* Button bits (the auto-joypad read layout) live in snos_abi.h, because apps
 * need them too via SYS_PAD. */
#include "snos_abi.h"

void pad_latch(void); /* called from the NMI handler, once per frame */
uint16_t pad_held(void);
uint16_t pad_pressed(void); /* rising edges since the last call; consumes them */

void kbd_init(void);
int kbd_getchar_nonblock(void); /* ASCII, or -1 if nothing pending */
int kbd_getchar(void);          /* blocks, keeping the screen and OSK alive */
void kbd_idle(void);            /* one frame of housekeeping while waiting */

/* Line editor. Returns the length; `buf` is NUL-terminated. Echoes as it goes
 * and shows an inverse-video caret. */
int kbd_readline(char *buf, int max);

/* On-screen keyboard. Visible by default — on a stock SNES it is the only way
 * to type, so hiding it by default would leave a new user stuck. */
void osk_show(void);
void osk_lock(int on); /* while an app owns the PPU, SELECT must not un-hide */
void osk_hide(void);
int osk_visible(void);
void osk_redraw(void);   /* repaint one dirty panel row, if any */
int osk_poll(void);      /* consume pad edges; returns ASCII or -1 */

#endif
