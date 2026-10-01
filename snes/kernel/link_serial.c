/* Real link: synchronous serial over controller port 2, to the Pico W adapter
 * (docs/pico-adapter.md). Built only with LINK_HW=1; the default ROM keeps
 * this backend absent so the emulator build is exactly what the suite tests.
 *
 * The console supplies the clock, so the peer can never push: every frame
 * moves because we asked for it, which is why the whole stack polls. One
 * link_serial_xfer() (kernel/linkxfer.s) is one exchange — LATCH, then
 * LINK_FRAME bytes out on LATCH and in on DATA1, MSB first, one bit per
 * $4017 read — and it costs ~181 master cycles a bit with DRAM refresh:
 * ~112k for a 64-byte frame with its quiet lead-in, 82 scanlines, a third of
 * a video frame. The NMI takes one exchange every vblank; kbd_idle may add
 * one more a frame where it still fits before the next (input.c, and
 * link_serial_fg_fits below; docs/link-throughput.md).
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
#include "snos.h"  /* snos_frames: which exchanges share a video frame */
#include "input.h" /* beam_line */

volatile uint8_t link_serial_hdma; /* link.h; written by console.c */

#ifdef LINK_HW

void link_serial_xfer(const uint8_t *tx, uint8_t *rx, uint16_t n);
uint8_t net_checksum(const uint8_t *f, uint16_t n);

#define PROBE_EVERY 60u
#define MISS_LIMIT 120u

void link_serial_kick(void);

static uint8_t last_rx[LINK_FRAME];
static uint8_t probe[LINK_FRAME];
static uint8_t have_rx;
static uint8_t up;
static uint16_t misses;
static uint16_t probe_wait;

/* Exchanges per video frame (`net tune exch`, docs/link-throughput.md). The
 * NMI's exchange is the first; kbd_idle's spin may add more while a transfer
 * is live, each only where link_serial_fg_fits allows. xfer_frame/xfer_count
 * count this frame's exchanges -- snos_frames moves at the top of the NMI --
 * so they also say which exchanges had no auto-joypad read before them. */
#define EXCH_MAX 3u
static uint8_t exch_max = 2;
static uint16_t xfer_frame;
static uint8_t xfer_count;

/* Every exchange comes through here, and returns 0 if it did not happen.
 *
 * THE LAST WORD ON VBLANK. What comes before this check -- net_service's C,
 * receiving and building -- varies, up to ~40 lines; what comes after it
 * does not: the kick, the quiet lead-in, the 512 clocks, then the receive
 * check and the way back out of net_service to net_service_fg's unmask.
 * NMI masked when vblank begins is the hazard (input.c, fg_poll_fits),
 * clocks or none, so an exchange is not started past XFER_LAST (and short
 * of 229). Traced on the hardware ROM (test/hwport.lua), check to unmask is
 * 89.0 lines, 4.8 of them after the last clock -- and 91.9 while the
 * keyboard's HDMA split takes its cycles from every line. So the gate is
 * 131, or 128 under HDMA: checked anywhere on that line, the unmask lands by
 * 221.0 or 220.9, four lines clear of 225, and test-hwspeed/-hwresponse fail
 * under two. It was 135 while the margin was believed to end at the last
 * clock, which unmasks as late as 224.9 (227.9 under HDMA). 131 cost the
 * speedtest nothing; 128 for all would cost its duplex phase 5%, hence two.
 * The frame built for a refused exchange is lost as if on the wire, and the
 * window resends it. The NMI's own exchange begins near line 230 and is
 * never refused.
 *
 * One that follows another in the same video frame has had no auto-joypad
 * read before it, and that read's short burst is what makes the adapter
 * restart and stage the answer it computed to the last exchange
 * (pico/port.c); without it the answer waits for the exchange after. So such
 * an exchange is kicked first: a short burst of our own (linkxfer.s), which
 * the adapter takes exactly as it takes the auto-read. */
#define XFER_LAST 131u
#define XFER_LAST_HDMA 128u
static int xfer(const uint8_t *tx) {
  uint8_t line = beam_line();
  if (line > (link_serial_hdma ? XFER_LAST_HDMA : XFER_LAST) && line < 229u) return 0;
  if (xfer_frame != snos_frames) {
    xfer_frame = snos_frames;
    xfer_count = 0;
  } else if (xfer_count != 0) {
    link_serial_kick();
  }
  if (xfer_count < 255u) xfer_count++;
  link_serial_xfer(tx, last_rx, LINK_FRAME);
  return 1;
}

/* May kbd_idle start a foreground exchange with the beam on `line` (the low
 * eight bits of the V counter, input.c)? While this frame has had fewer than
 * exch_max exchanges, the link is up, and the poll will most likely reach its
 * exchange by XFER_LAST: net_service's C takes ~30 lines before the clocks
 * and up to ~40, measured on the hardware ROM. Or it may start after this
 * frame's NMI and auto-joypad read are both done (229 on; 256-261 read as
 * 0-5, inside the early zone and just as safe). xfer() makes the final call;
 * this one only keeps it from having to refuse often, because a refused
 * exchange costs a resend. */
#define FG_LAST 100u
int link_serial_fg_fits(uint16_t line) {
  if (!up || exch_max < 2u) return 0;
  if (xfer_frame == snos_frames && xfer_count >= exch_max) return 0;
  return line <= FG_LAST || line >= 229u;
}

/* `net tune exch`: 0 asks, 1..EXCH_MAX sets; returns what is in force. One
 * is the NMI's exchange alone, as the hardware build ran before. */
int link_serial_exch(int value) {
  if (value > 0) exch_max = (uint8_t)(value > (int)EXCH_MAX ? EXCH_MAX : value);
  return exch_max;
}

/* An idle or absent adapter reads as all zeros, and 64 zero bytes checksum
 * to zero, so a frame that sums correctly is not yet a frame. Two bytes
 * tell them apart: every peer puts a record or padding at LINK_HDR, and a
 * record's channel may be 0 (control) but its length never is, while
 * padding is LINK_CH_PAD. A real frame never has both zero; a dead line
 * always does. This was a scan of all 64 bytes -- ~47k master cycles at
 * -O0, inside the NMI, on every exchange (docs/link-throughput.md). */
static int frame_ok(const uint8_t *f) {
  if (f[LINK_SUM] != net_checksum(f, LINK_FRAME)) return 0;
  return (f[LINK_HDR] | f[LINK_HDR + 1]) != 0;
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
  if (!xfer(probe)) return 0;
  if (!frame_ok(last_rx)) return 0;
  up = 1;
  misses = 0;
  have_rx = 1;
  return 1;
}

/* One MVN: both frames are kernel far data in bank $7E (link.h). The byte
 * loop it replaces cost 34k master cycles a poll, a tenth of a frame. */
int link_serial_recv(uint8_t *rx) {
  if (!have_rx) return 0;
  have_rx = 0;
  link_mb_move(rx, last_rx, LINK_FRAME);
  return 1;
}

void link_serial_send(const uint8_t *tx) {
  if (!xfer(tx)) {
    have_rx = 0;
    return;
  }
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
int link_serial_fg_fits(uint16_t line) {
  (void)line;
  return 0;
}
int link_serial_exch(int value) {
  (void)value;
  return 1;
}

#endif
