/* The wire: one fixed-size frame exchanged per poll, in both directions.
 *
 * This is the ONLY thing the network stack knows about how bytes physically
 * reach the gateway. Everything above it — reliability, multiplexing, streams,
 * HTTP — is written against link_recv()/link_send() and cannot tell the
 * difference between a bit-banged controller port and a Lua script writing into
 * WRAM.
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
 *   [3] checksum  8-bit sum of all LINK_FRAME bytes, computed with this
 *                 byte zero
 *   [4..]         records to the end of the frame, packed:
 *                 [channel][length][payload...], padded with LINK_CH_PAD
 *                 (sizes as symbols, not numbers: the 64->128 change left
 *                 this comment lying for a week)
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

/* 256 on the emulated mailbox. The hardware build (make LINK_HW=1) bit-bangs
 * every frame through the controller port at ~180 master cycles a bit, so it
 * takes 64: one exchange per vblank fits the NMI budget. The Pico firmware
 * (pico/link.h) carries the same constant; the two must agree. */
#ifdef LINK_HW
#define LINK_FRAME 64
#else
#define LINK_FRAME 256
#endif
#define LINK_HDR 4
#define LINK_PAYLOAD (LINK_FRAME - LINK_HDR) /* record bytes per frame */

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

/* Raw IP datagrams (M11). Deliberately OUTSIDE the stream-channel range
 * 0..NET_CHANNELS-1: every parser on both sides has always skipped a record
 * whose channel it does not know by its length byte, so a pre-M11 gateway
 * that never claims IP support simply never sees one of these, and an old
 * kernel fed one steps over it. The record payload is a byte-stream slice;
 * datagram boundaries inside that stream are a 2-byte little-endian length
 * prefix per datagram (kernel/ip.c) — link-local convention, not IP. */
#define LINK_CH_IP 0x10

/* Keyboard bytes from the hardware adapter (docs/pico-adapter.md): ASCII plus
 * the SNOS_KEY_* codes, pushed straight into the input ring. Peer -> console
 * only; an older kernel steps over it by length like any unknown channel. */
#define LINK_CH_KBD 0x11

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
/* DERIVED from LINK_FRAME, never a literal. The two buffers used to be 0x10 and
 * 0x50 -- exactly LINK_FRAME apart at 64 -- so raising the frame size silently
 * overlapped them, and four separate files carried the same literal. A frame
 * size that disagrees with a buffer offset is not a build error: it is a link
 * that carries the first 64 bytes and garbage after them. */
#define LINK_MB_RX ((volatile __far uint8_t *)(LINK_MB + 0x10 + LINK_FRAME))

void link_init(void);
void link_reset(void);
int link_present(void);
/* The cable rather than the mailbox: only the cable can keep frames in flight. */
int link_is_serial(void);

/* A POLL IS TWO HALVES, AND THE GAP BETWEEN THEM IS WORTH A ROUND TRIP.
 *
 * link_recv() takes whatever has arrived; link_send() puts a frame on the wire.
 * They used to be one call, link_poll(tx, rx), and that shape forced a whole
 * extra poll into every exchange: the frame handed to it was necessarily built
 * from the PREVIOUS poll's receive, so the ack for a frame that arrived at poll
 * k could not leave until poll k+1. Splitting them lets net_service() receive,
 * process, build and send inside one poll, and the steady-state cycle drops
 * from 2 polls per data frame to 1.
 *
 * THE ORDER IS LOAD-BEARING — receive strictly before send, always. The Lua
 * bridge (tools/netbridge.lua) infers "the console has taken the frame I put in
 * the mailbox" from the console's write of the sequence counter, which is the
 * last thing link_send() does. That inference is only sound because nothing
 * between the two halves can touch the mailbox, and because the receive copy has
 * already happened by then. Reversing them, or sending twice per receive, breaks
 * the bridge's flow control silently and costs half the link's throughput.
 *
 * link_recv() returns 1 if `rx` was filled, 0 if the peer had nothing new.
 *
 * WHY A SYNCHRONOUS CABLE CAN STILL IMPLEMENT THIS. On the real wire bits shift
 * both ways at once, so a backend cannot literally receive before it transmits.
 * It does not have to: link_recv() means "hand me what has already arrived",
 * which for a shift-register backend is the frame the last exchange clocked in,
 * and link_send() then performs the exchange. That is precisely what link_poll()
 * did, so the serial backend loses nothing and gains no obligation. The one-poll
 * cycle is a property of the mailbox backend, where receive and transmit are
 * genuinely separate memory accesses — and the mailbox is the thing every test
 * and every iteration of the toolchain actually runs on. */
int link_recv(uint8_t *rx);
void link_send(const uint8_t *tx);

/* Backends. link.c picks one at runtime. */
void link_wram_init(void);
void link_wram_reset(void);
int link_wram_present(void);
int link_wram_recv(uint8_t *rx);
void link_wram_send(const uint8_t *tx);

void link_serial_init(void);
int link_serial_present(void);
int link_serial_recv(uint8_t *rx);
void link_serial_send(const uint8_t *tx);

/* Move n bytes within WRAM bank $7E, as one MVN (kernel/netasm.s).
 *
 * The emulated backend's two frame copies are the reason it exists: they run
 * inside NMI, and a C byte loop at -O0 costs ~103 cycles a byte there. Only the
 * EMULATED backend has them — link_serial.c shifts bits straight out of the
 * frame — so this is a test-rig speedup, which PLAN.md's performance section
 * says is exactly the point.
 *
 * BOTH ENDS MUST BE IN BANK $7E: MVN's bank operands are immediates. That holds
 * by construction here — linker.scm gives kernel far data one HiRAM region
 * inside $7E, and LINK_MB above is fixed at $7EE000 — and netasm.s explains
 * what to change if that ever stops being true. n must be at least 1. */
void link_mb_move(void *dst, const void *src, uint16_t n);

#endif
