/* Real link: synchronous serial over controller port 2, to the Pico W adapter
 * (docs/pico-adapter.md). Built only with LINK_HW=1; the default ROM keeps
 * this backend absent so the emulator build is exactly what the suite tests.
 *
 * The console supplies the clock, so the peer can never push: every frame
 * moves because we asked for it, which is why the whole stack polls. One
 * link_serial_xfer() (kernel/netasm.s) is one exchange — LATCH, then
 * LINK_FRAME bytes out on IOBIT and in on DATA1, MSB first, one bit per
 * $4017 read — and it costs ~180 master cycles a bit, ~92k for a 64-byte
 * frame, a quarter of a video frame. That is why LINK_HW also sets
 * LINK_FRAME to 64 and turns the foreground poll off: the NMI's one exchange
 * per vblank is the budget (link.h, input.c).
 *
 * Presence is measured, not assumed. With nothing plugged in, $4017 reads a
 * constant and no frame ever checksums, so the backend probes once every
 * PROBE_EVERY polls rather than paying an exchange per frame for a cable
 * that is not there. Once a frame checksums it polls every time, and gives
 * up again after MISS_LIMIT bad exchanges in a row — a Pico reset shows up
 * as a few misses and then a fresh SYN, which net.c already handles.
 *
 * The split halves match link.h's contract for a shift-register backend:
 * link_serial_send() performs the exchange and keeps what came back;
 * link_serial_recv() hands that frame over on the NEXT poll. */
#include "link.h"
#include "net.h"

#ifdef LINK_HW

void link_serial_xfer(const uint8_t *tx, uint8_t *rx, uint16_t n);
uint8_t net_checksum(const uint8_t *f, uint16_t n);

#define PROBE_EVERY 60u
#define MISS_LIMIT 120u

static uint8_t last_rx[LINK_FRAME];
static uint8_t probe[LINK_FRAME];
static uint8_t have_rx;
static uint8_t up;
static uint16_t misses;
static uint16_t probe_wait;

/* An idle or absent adapter reads as all zeros, and 64 zero bytes checksum
 * to zero: a real frame always carries at least one non-zero byte (its pad
 * bytes are LINK_CH_PAD), so all-zero is "nobody there", never "valid". */
static int frame_ok(const uint8_t *f) {
  uint16_t i;
  uint8_t any = 0;
  if (f[LINK_SUM] != net_checksum(f, LINK_FRAME)) return 0;
  for (i = 0; i < LINK_FRAME; i++) any |= f[i];
  return any != 0;
}

void link_serial_init(void) {
  uint16_t i;
  /* The probe is an empty, unflagged frame: it tells the adapter nothing
   * except that a console is here, and the adapter's reply is what net.c
   * sees first (its SYN), exactly as if the peer had just restarted. */
  for (i = 0; i < LINK_FRAME; i++) probe[i] = LINK_CH_PAD;
  probe[LINK_SEQ] = 0;
  probe[LINK_ACK] = 0;
  probe[LINK_FLAGS] = 0;
  probe[LINK_SUM] = 0;
  probe[LINK_SUM] = net_checksum(probe, LINK_FRAME);
  have_rx = 0;
  up = 0;
  misses = 0;
  probe_wait = PROBE_EVERY - 1; /* probe on the first poll after init/reset */
}

int link_serial_present(void) {
  if (up) return 1;
  if (++probe_wait < PROBE_EVERY) return 0;
  probe_wait = 0;
  link_serial_xfer(probe, last_rx, LINK_FRAME);
  if (!frame_ok(last_rx)) return 0;
  up = 1;
  misses = 0;
  have_rx = 1;
  return 1;
}

int link_serial_recv(uint8_t *rx) {
  uint16_t i;
  if (!have_rx) return 0;
  have_rx = 0;
  for (i = 0; i < LINK_FRAME; i++) rx[i] = last_rx[i];
  return 1;
}

void link_serial_send(const uint8_t *tx) {
  link_serial_xfer(tx, last_rx, LINK_FRAME);
  if (frame_ok(last_rx)) {
    have_rx = 1;
    misses = 0;
    return;
  }
  have_rx = 0;
  if (++misses >= MISS_LIMIT) {
    up = 0;
    misses = 0;
  }
}

#else /* the emulator build: no cable, and saying so keeps net.c honest */

void link_serial_init(void) {}
int link_serial_present(void) { return 0; }
int link_serial_recv(uint8_t *rx) {
  (void)rx;
  return 0;
}
void link_serial_send(const uint8_t *tx) { (void)tx; }

#endif
