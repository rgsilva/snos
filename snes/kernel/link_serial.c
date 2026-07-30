/* Real link: synchronous serial over controller port 1. Stub until M7.
 *
 * ------------------------------------------------------------ the design
 *
 * The SNES supplies the clock, so this is a synchronous exchange and the peer
 * can never push — every frame moves because we asked for it. That is why the
 * whole stack polls, and why nothing above here had to be designed around
 * asynchronous arrival.
 *
 * Port 1, and not by preference: port 2's IO bit is wired to the PPU's
 * light-pen input and latches the H/V counters as a side effect of being
 * driven. Signals, per fullsnes:
 *
 *   $4016 write bit 0   STROBE  (shared by both ports)
 *   $4016 read          clocks port 1 and returns its two data lines
 *   $4201 bit 6         IOBit out, port 1   (open collector - see below)
 *   $4213 bit 6         IOBit in,  port 1
 *
 * Full duplex costs nothing: the same `STA $4201` / `LDA $4016` pair drives a
 * bit out and clocks bits in simultaneously. Roughly 70 master cycles per bit
 * for an unrolled loop, which is ~38 KB/s in theory and ~20 KB/s in practice —
 * byuu measured both on real hardware.
 *
 * A 64-byte frame is 512 bits, about 35,800 master cycles. Vblank is roughly
 * 51,800, so one frame per vblank fits with room to spare and never disturbs
 * the display. Unlike a game, though, SNOS is idle during active display, so
 * the idle loop can donate time on top and push well past the vblank-only
 * figure when the foreground has nothing to do.
 *
 * ---------------------------------------------------------------- hazards
 *
 * $4201 is OPEN COLLECTOR with a documented slow rising edge. It needs an
 * external pull-up on the far end; an emulator will not punish its absence and
 * real hardware will.
 *
 * $4016/$4017 sit in the 12-master-cycle XSlow region, unlike $4201/$4213 at 6.
 * The bit loop's timing budget has to account for that asymmetry.
 */
#include "link.h"

void link_serial_init(void) {}

/* Nothing is wired up yet, and saying so is what makes the runtime backend
 * choice safe: with no cable and no host script, link_present() is false and
 * the network layer reports "no gateway" instead of hanging on a dead port. */
int link_serial_present(void) { return 0; }

/* Split into halves to match link.h, and the split costs this backend nothing.
 *
 * A synchronous cable cannot receive before it transmits — the bits cross on the
 * wire — so link_serial_recv() hands back whatever the LAST exchange clocked in
 * and link_serial_send() performs the exchange. That is exactly what a single
 * link_serial_poll() would have done, with the same one-frame-per-exchange
 * behaviour and the same intrinsic two-poll round trip. Only the mailbox backend
 * gets the one-poll cycle, and only because its two directions are separate
 * memory accesses rather than one shift register. */
int link_serial_recv(uint8_t *rx) {
  (void)rx;
  return 0;
}

void link_serial_send(const uint8_t *tx) { (void)tx; }
