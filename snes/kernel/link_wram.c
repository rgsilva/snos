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

void link_wram_reset(void) { last_host_seq = LINK_MB_HOST_SEQ; }

int link_wram_present(void) { return LINK_MB_MAGIC == LINK_MAGIC; }

/* The two halves of a poll. See the comment on link_recv in link.h for why they
 * are separate; the ORDER they must be called in — receive, then transmit — is
 * exactly the order link_wram_poll used, and it is what the Lua bridge's
 * flow control depends on.
 *
 * Both copies are an MVN block move (kernel/netasm.s) rather than the obvious
 * byte loop because this runs inside NMI: at -O0 the loop cost ~103 cycles a
 * byte, and 128 bytes of it per poll was most of a handler that ran for 55% of
 * every frame. MVN does ~7. */
int link_wram_recv(uint8_t *rx) {
  if (LINK_MB_HOST_SEQ == last_host_seq) return 0;
  link_mb_move(rx, (const uint8_t *)LINK_MB_RX, LINK_FRAME);
  last_host_seq = LINK_MB_HOST_SEQ;
  return 1;
}

void link_wram_send(const uint8_t *tx) {
  /* The frame is copied in full BEFORE the counter moves, and the bridge's
   * exchange callback hangs off the counter. That ordering is the reason a
   * callback can never see a half-written frame — do not reverse it. */
  link_mb_move((uint8_t *)LINK_MB_TX, tx, LINK_FRAME);
  LINK_MB_SNES_SEQ = (uint16_t)(LINK_MB_SNES_SEQ + 1);
}
