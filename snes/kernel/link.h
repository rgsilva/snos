/* The wire: one fixed-size frame exchanged per poll, in both directions.
 *
 * This is the ONLY thing the network stack knows about how bytes physically
 * reach the gateway. Everything above it — reliability, multiplexing, streams,
 * HTTP — is written against link_poll() and cannot tell the difference between
 * a bit-banged controller port and a Lua script writing into WRAM.
 *
 * That indifference is deliberate and it is the whole M6/M7 strategy. If the
 * emulated backend exchanges exactly one frame per poll with the same header,
 * the same sequence numbers and the same checksum as the real cable, then
 * everything above it is genuinely exercised now, and M7 replaces roughly a
 * hundred lines of bit-banging rather than discovering a stack's worth of bugs.
 *
 * ----------------------------------------------------------- frame layout
 *
 *   [0] seq       sender's frame sequence number
 *   [1] ack       highest sequence the sender has correctly received
 *   [2] flags     LINK_F_DATA when this frame carries new records
 *                 LINK_F_SYN  the sender has restarted; adopt its sequence
 *   [3] checksum  8-bit sum of all 64 bytes, computed with this byte zero
 *   [4..63]       records, packed: [channel][length][payload...]
 *                 padded out with channel LINK_CH_PAD
 *
 * Records are packed rather than one-channel-per-frame so three trickling IRC
 * channels share a frame instead of taking three, while a bulk download still
 * gets the whole frame because nothing else has anything to send. Greedy fill
 * does the right thing in both cases without a scheduler.
 *
 * WHY THE SNES ALWAYS POLLS: on a real cable the SNES supplies the clock, so
 * the peer physically cannot push. Both backends therefore poll, and the
 * emulated one gains nothing by cheating.
 */
#ifndef SNOS_LINK_H
#define SNOS_LINK_H

#include <stdint.h>

#define LINK_FRAME 64
#define LINK_HDR 4
#define LINK_PAYLOAD (LINK_FRAME - LINK_HDR) /* 60 bytes of records */

#define LINK_SEQ 0
#define LINK_ACK 1
#define LINK_FLAGS 2
#define LINK_SUM 3

#define LINK_F_DATA 0x01u /* this frame carries records worth accepting */

/* "I have restarted; my sequence numbers begin again here."
 *
 * Without this the link deadlocks permanently the first time either end is
 * restarted alone. The peer comes back at seq 0 while we are still waiting for
 * seq 200; we reject every frame and never ack, it is stop-and-wait so it
 * retransmits the same frame forever, and nothing short of rebooting the SNES
 * recovers. Reproduced exactly: start gateway (works), stop it, start it again
 * (dead forever, including across a reload of the bridge script).
 *
 * The receiver adopts the sender's sequence ONCE per episode — a flag that is
 * honoured on every frame would re-deliver retransmissions as duplicates, since
 * the ordinary seq check is what discards those. */
#define LINK_F_SYN 0x02u

#define LINK_CH_CTRL 0  /* commands and their replies */
#define LINK_CH_PAD 0xFF /* filler; stop parsing records here */

/* The emulated backend's mailbox, mirroring the physical exchange exactly.
 * Carved out of HiRAM in calypsi/linker.scm. The host claims the link by
 * writing the magic; the kernel zeroes it at boot, so on real hardware nothing
 * ever does and the serial backend is used instead. One ROM, both worlds. */
#define LINK_MB 0x7EE000ul
#define LINK_MAGIC 0x4B4Cu /* "LK" */

#define LINK_MB_MAGIC (*(volatile __far uint16_t *)(LINK_MB + 0x00))
#define LINK_MB_SNES_SEQ (*(volatile __far uint16_t *)(LINK_MB + 0x02))
#define LINK_MB_HOST_SEQ (*(volatile __far uint16_t *)(LINK_MB + 0x04))
#define LINK_MB_TX ((volatile __far uint8_t *)(LINK_MB + 0x10))
#define LINK_MB_RX ((volatile __far uint8_t *)(LINK_MB + 0x50))

void link_init(void);
int link_present(void);

/* Exchange one frame. Reads first, then writes — which is not arbitrary: it is
 * what lets stop-and-wait run at one frame per poll instead of one per two.
 * Reading first means an ack that arrived this poll is already in hand when the
 * outgoing frame is built, so the next sequence can go out immediately.
 *
 * Returns 1 if `rx` was filled, 0 if the peer had nothing new. */
int link_poll(const uint8_t *tx, uint8_t *rx);

/* Backends. link.c picks one at runtime. */
void link_wram_init(void);
int link_wram_present(void);
int link_wram_poll(const uint8_t *tx, uint8_t *rx);

void link_serial_init(void);
int link_serial_present(void);
int link_serial_poll(const uint8_t *tx, uint8_t *rx);

#endif
