/* Emulated link: a mailbox in WRAM that mirrors the physical exchange.
 *
 * The fidelity is the point. This does NOT hand the network stack a friendly
 * byte pipe — it exchanges one 64-byte frame per poll, exactly like the cable
 * will, so sequence numbers, checksums and retransmission are all genuinely
 * exercised against it. A convenient fake here would mean discovering the
 * reliability bugs at M7 with a soldering iron in hand.
 *
 * It also lets a test do something no real cable can be made to do on demand:
 * drop a frame, or corrupt a checksum, deterministically.
 */
#include "link.h"

static uint16_t last_host_seq;

void link_wram_init(void) {
  /* Zero the mailbox so a power-on WRAM pattern cannot be mistaken for a host
   * claiming the link. WRAM comes up as garbage, and this exact class of bug
   * (uninitialised flags read by the NMI handler) already cost this project a
   * day in M3 — see the exec-state comment in main.c. */
  LINK_MB_MAGIC = 0;
  LINK_MB_SNES_SEQ = 0;
  LINK_MB_HOST_SEQ = 0;
  last_host_seq = 0;
}

int link_wram_present(void) { return LINK_MB_MAGIC == LINK_MAGIC; }

int link_wram_poll(const uint8_t *tx, uint8_t *rx) {
  int got = 0;
  int i;

  /* Read first, then write: see the comment on link_poll in link.h. */
  if (LINK_MB_HOST_SEQ != last_host_seq) {
    for (i = 0; i < LINK_FRAME; i++) rx[i] = LINK_MB_RX[i];
    last_host_seq = LINK_MB_HOST_SEQ;
    got = 1;
  }

  for (i = 0; i < LINK_FRAME; i++) LINK_MB_TX[i] = tx[i];
  LINK_MB_SNES_SEQ = (uint16_t)(LINK_MB_SNES_SEQ + 1);

  return got;
}
