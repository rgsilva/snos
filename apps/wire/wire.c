/* wire: validate the port-2 cable and the adapter's framing, nothing else.
 *
 * Once per video frame it exchanges one 64-byte frame with the Pico through
 * link_serial_xfer (the same routine the kernel's serial backend uses, linked
 * here as its own copy): byte 0 is a counter, bytes 1..63 the pattern
 * "1234567890" repeated. The Pico answers with its own counter and
 * "ABCDEFGHIJ" repeated. Every second one line goes to the screen with the
 * counts and the start of the last reply; the Pico prints the mirror image on
 * its UART. Both counters moving and both "bad" columns staying at zero is the
 * verdict. B exits (START+SELECT works too, as everywhere).
 *
 * The exchange is placed right after the vblank tick: the NMI has just run and
 * waited out the auto-joypad read, so the port is quiet and the 2.5 ms of
 * clocking ends long before the next vblank. */
#include "snos_abi.h"

#define WIRE_FRAME 64u

void link_serial_xfer(const uint8_t *tx, uint8_t *rx, uint16_t n);

static const char TX_PAT[] = "1234567890";
static const char RX_PAT[] = "ABCDEFGHIJ";
/* The Pico's counter byte has its top bit set (so DATA1 parks high). */

static uint8_t tx[WIRE_FRAME];
static uint8_t rx[WIRE_FRAME];
static uint8_t want[WIRE_FRAME]; /* the reply we expect, built once: no
                                    modulo in the per-frame compare */
static uint8_t inv[WIRE_FRAME];  /* the same reply if the console reads
                                    DATA1 inverted: reported, not hidden */

static void putpad(uint16_t v) {
  if (v < 10000) sys_putc(' ');
  if (v < 1000) sys_putc(' ');
  if (v < 100) sys_putc(' ');
  if (v < 10) sys_putc(' ');
  sys_putu(v);
}

int main(void) {
  uint16_t frames = 0, ok = 0, bad = 0, okinv = 0, tick = 0;
  uint16_t i;

  sys_puts("wire: port 2 <-> Pico, one 64-byte frame per vblank\n");
  sys_puts("out: counter + 1234567890...   in: counter + ABCDEFGHIJ...\n");
  sys_puts("B exits. Columns: frames, ok, bad, ok-if-inverted, pico counter, reply\n");

  for (i = 1; i < WIRE_FRAME; i++) {
    tx[i] = (uint8_t)TX_PAT[(i - 1) % 10];
    want[i] = (uint8_t)RX_PAT[(i - 1) % 10];
    inv[i] = (uint8_t)~want[i];
  }

  for (;;) {
    int good = 1, goodinv = 1;
    tx[0] = (uint8_t)frames;
    /* Prefilled so "never written" (0xEE everywhere, 238 in the counter
     * column) cannot be mistaken for "read as zero". */
    for (i = 0; i < WIRE_FRAME; i++) rx[i] = 0xEE;
    sys_waitvbl();
    link_serial_xfer(tx, rx, WIRE_FRAME);
    frames++;
    for (i = 1; i < WIRE_FRAME; i++) {
      if (rx[i] != want[i]) good = 0;
      if (rx[i] != inv[i]) goodinv = 0;
    }
    if (good) ok++; else bad++;
    if (goodinv) okinv++;

    if (++tick == 60) {
      tick = 0;
      putpad(frames); sys_putc(' ');
      putpad(ok); sys_putc(' ');
      putpad(bad); sys_putc(' ');
      putpad(okinv); sys_putc(' ');
      putpad(rx[0]); sys_putc(' ');
      for (i = 1; i <= 24; i++)
        sys_putc((rx[i] >= 0x20 && rx[i] < 0x7F) ? rx[i] : '.');
      sys_putc('\n');
    }
    if (sys_pad() & PAD_B) break;
  }
  sys_puts("wire: done\n");
  return 0;
}
