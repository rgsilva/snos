/* Backend selection, at runtime rather than build time.
 *
 * The kernel zeroes the link mailbox at boot and then uses the serial backend
 * unless something has written the magic word into it. On real hardware nothing
 * ever does; under the emulator the host script does. So one ROM image works in
 * both worlds, which matters more than it sounds: the binary that gets tested
 * is then literally the binary that ships, with no variant matrix and no
 * "works in the emulator" build to keep honest.
 */
#include "link.h"

static uint8_t backend; /* 0 = none decided yet, 1 = wram, 2 = serial */

void link_init(void) {
  link_wram_init();
  link_serial_init();
  backend = 0;
}

int link_present(void) {
  /* Re-checked rather than latched, because the host script can claim the link
   * at any point after boot — you start the ROM, then start the script. */
  if (link_wram_present()) {
    backend = 1;
    return 1;
  }
  if (link_serial_present()) {
    backend = 2;
    return 1;
  }
  backend = 0;
  return 0;
}

int link_recv(uint8_t *rx) {
  if (backend == 0 && !link_present()) return 0;
  if (backend == 1) return link_wram_recv(rx);
  if (backend == 2) return link_serial_recv(rx);
  return 0;
}

/* No backend re-selection here. link_recv() runs first on every poll and has
 * already done it; probing again would read the mailbox magic a second time,
 * and the bridge hangs a frame delivery off that read. */
void link_send(const uint8_t *tx) {
  if (backend == 1) link_wram_send(tx);
  else if (backend == 2) link_serial_send(tx);
}
