/* Link layer: reliability, the control channel, and the channel buffers.
 *
 * Since the M11 cutover there is no stream protocol here. The daemon is a
 * slirp box, not a proxy: everything that used to be a stream record —
 * fetches, connects, listens — is now IPv4 the console composes itself
 * (kernel/ip.c + uIP), riding LINK_CH_IP as one more byte stream. What this
 * file still owns is the wire's reliability, the two-record control channel
 * (HELLO, and the NAT's inbound port-forwards), and the per-channel rings
 * that applications read and write — filled by uIP now, but the same rings,
 * which is why no application changed when the backing did.
 *
 * ------------------------------------------------------- why reliability
 *
 * The console's own TCP would survive a lossy cable, but the datagram stream
 * it rides shares the link with the control channel, which has no recovery
 * of its own — and a checksummed, retransmitting link is also what M7's real
 * cable needs regardless of what is inside the frames.
 *
 * So: checksum, sequence number, retransmit, and a window of frames in flight.
 *
 * WHY A WINDOW: the real link is a synchronous exchange — bits shift out while
 * other bits shift in — so a frame's acknowledgement cannot ride in the
 * exchange that carried it. The round trip is at least two polls, and a
 * stop-and-wait sender moves one fresh frame every two: half the wire. With
 * frames kept in flight every exchange can carry a fresh one, and the receivers
 * were always ready for that: every peer takes frames strictly in order and
 * acknowledges cumulatively. A window of 1 is stop-and-wait exactly as this
 * file did it for its first two years; the mailbox backend always runs at 1,
 * and `net tune window` puts the hardware back there
 * (docs/link-throughput.md).
 *
 * --------------------------------------------------- who touches what, when
 *
 * net_service() runs in the NMI handler. Everything else runs in the
 * foreground. They share state, so each shared index has exactly one writer:
 *
 *   iprx  head   NMI writes (frame_deliver), foreground reads   (producer)
 *   iprx  tail   foreground writes (net_ip_read), NMI reads     (consumer)
 *   iptx  head   foreground writes (net_ip_write), NMI reads
 *   iptx  tail   NMI writes (ack release), foreground reads
 *   txq   head   foreground writes, NMI reads
 *   txq   tail   NMI writes, foreground reads
 *
 * The per-channel rings left the NMI entirely at the cutover: uIP fills and
 * the application drains, both in the foreground. Single-producer/single-
 * consumer on each index above means no locking is needed and none is used.
 * Adding a second writer to any of these would break it silently.
 */
#include "net.h"
#include "link.h"
#include "proc.h"
#include "ip.h"
#include "snos.h" /* snos_frames, for the foreground-poll hot window */
#include "input.h"   /* kbd_push: LINK_CH_KBD lands in the input ring */
#include "mailbox.h" /* MB_IN_*: the ring frame_fits reserves against */
/* proc_lock (M10 review): the txq record appends and channel scan-and-claim
 * below are run BOTH by the shell directly and by background syscalls. A
 * context switch mid-record interleaves two control records and desyncs the
 * gateway's parser until a link restart; pinning the scheduler for the span
 * is the mutual exclusion. The NMI itself still runs — net_service and the
 * single-producer ring contracts are untouched. */

/* ---------------------------------------------------- control protocol ---
 * Messages are [type][len][payload...] inside channel 0's byte stream, and
 * only three requests remain — everything else the control channel once said
 * ($01 FETCH, $02 CLOSE, $03 IMAGE, $07 CONNECT, $08 LISTEN, $09 HANGUP,
 * $0A WEB) is retired with the stream gateway. Numbers reserved, never
 * reused: an old daemon fed a reused number would act on the wrong verb. */

/* Liveness. net_up() only proves the *bridge* claimed the mailbox — the daemon
 * behind it can be dead, stopped or wedged and the link still looks attached.
 * Nothing else in the protocol is unsolicited, so asking is the only way to
 * know — and the reply's version is what arms IP mode (>= 2). */
#define C2S_HELLO 0x04 /* [0] */
/* The NAT's inbound half. FWD means "hold a real socket on this port and
 * route the PACKETS to my stack" — the console is behind the NAT, so that
 * socket is the only thing the daemon contributes to an inbound connection.
 * Failure is reported as an S2C_ERR against the channel, so "port already in
 * use" surfaces through the path applications already handle. */
#define C2S_IPFWD 0x0B   /* [3][ch][port lo][port hi] */
#define C2S_IPUNFWD 0x0C /* [3][ch][port lo][port hi] */
/* How many frames the peer may keep in flight toward us: [1][window]. A Pico
 * that has not been asked, or an older one that does not know the verb, sends
 * stop-and-wait; so does every peer after a console restart. */
#define C2S_LINKWIN 0x0D
#define S2C_ERR 0x83   /* [2][ch][code] */
#define S2C_HELLO 0x84   /* [n][version][gateway ip as text...] */

volatile uint16_t net_stat_rx_frames;
volatile uint16_t net_stat_hello;
char net_text[NET_TEXT_MAX + 1];   /* gateway ip, or resolved addresses */
volatile uint16_t net_gw_version;
volatile uint16_t net_stat_bad_sum;
volatile uint16_t net_stat_resent;
/* Backpressure, counted. Every in-sequence data frame that frame_fits() turns
 * away because a channel's receive ring is full bumps this — which is the only
 * way to tell "the application is not draining" apart from every other reason a
 * transfer stalls. It cost 607 polls of a 32 s screen once (PLAN.md), and the
 * claim that the transpose fixed it was an inference from a total, not a
 * measurement. The increment sits in a branch that is not taken on a healthy
 * link, so it is free in the NMI budget. */
volatile uint16_t net_stat_refused;

static uint8_t ring[NET_CHANNELS][NET_RING];
/* Both ring indices are FOREGROUND-owned now: uIP fills (net_stack_rxput,
 * from ip_service) and the application drains (net_read). The NMI stopped
 * touching channel rings when the stream protocol went — the split-ownership
 * discipline survives only where the NMI still is: iprx/iptx and txq. */
static uint16_t ring_head[NET_CHANNELS];
static uint16_t ring_tail[NET_CHANNELS];
static uint8_t ch_state[NET_CHANNELS];
/* Which channels an application opened, so they can be reclaimed when it
 * leaves. Channels are REUSED, so a leak is not merely untidy: the next app
 * handed the same channel inherits whatever the dead one left in its ring, and
 * reads another program's bytes as its own. The break key makes that reachable
 * today, since killing a download mid-transfer leaks by construction. */
static uint8_t ch_app[NET_CHANNELS];
/* Channels bound to a port. They differ from every other channel in that they
 * come back: a connection ending returns them to LISTEN rather than freeing
 * them, so a server is a loop and the port survives between requests. */
static uint8_t ch_listen[NET_CHANNELS];
/* M11: this channel's bytes belong to the local stack, not to the gateway.
 * The NMI must never pack its txring (uIP consumes it in the foreground) and
 * the gateway is never told about it. */
static uint8_t ch_stack[NET_CHANNELS];
/* The port a stack-backed listener bound, so its forward can be withdrawn
 * when the channel closes — the daemon holds a real socket on that port and
 * nothing else knows which one. */
static uint16_t ch_lport[NET_CHANNELS];
/* Withdrawals that did not fit in the control queue at close time. The old
 * code simply skipped them and cleared ch_lport anyway, so nothing could
 * ever retry: the daemon held the real socket until it was restarted, and
 * the next run of the same application found its own port taken by its
 * previous life — the exact failure net_close's comment warns about. */
static uint16_t unfwd_port[NET_CHANNELS];

/* Channel-lifecycle mutations (claim/close/hangup) touch ch_state, which the
 * NMI also writes (S2C_ERR, and the fail-everything sweep on a peer restart).
 * Masking NMI at the source for those few stores makes the whole mutation
 * atomic against net_service() — the same idiom as pad_pressed(), and cheap
 * because lifecycle events are per-connection. */
#define NET_R8(a) (*(volatile uint8_t *)(a))
#define NET_NMI_OFF() (NET_R8(0x4200) = 0x01)
#define NET_NMI_ON() (NET_R8(0x4200) = 0x81)

/* Outbound stream bytes, per channel — the application's send queue, which
 * uIP consumes segment by segment (net_stack_txcopy) and releases only on
 * acknowledgement (net_stack_txdrop). Both indices foreground-owned, like the
 * receive rings above.
 *
 * THIS IS THE SEGMENT SIZE. uIP keeps one segment in flight and fills it with
 * whatever the ring holds, up to the MSS (1460), so a ring of 512 sent 511-byte
 * segments: half the MSS, and every one paid a whole round trip -- the far
 * end's ack, an ip_service pass, the application refilling the ring -- before
 * the next could leave. 1024 doubles the bytes per round trip for 4 KB of WRAM
 * over eight channels (docs/link-throughput.md); 2048 would reach the MSS but
 * does not fit. Smaller than the receive ring because the shapes differ: a
 * download arrives in bulk against an app that reads it when it reads it,
 * while what the SNES sends it generates itself, and can generate less of. */
#define NET_TXRING 1024
static uint8_t txring[NET_CHANNELS][NET_TXRING];
static uint16_t txring_head[NET_CHANNELS];
static uint16_t txring_tail[NET_CHANNELS];

/* Outbound control bytes. Small: the SNES says very little — a URL and the
 * occasional close. */
#define TXQ_SIZE 512
static uint8_t txq[TXQ_SIZE];
static uint16_t txq_head; /* foreground writes */
static uint16_t txq_tail; /* NMI writes */

/* ---- M11: the raw-IP byte stream ----------------------------------------
 *
 * One ring pair for the whole IP layer, not per anything: below ip.c the
 * datagrams are just bytes on one more link channel, and the multiplexing IP
 * itself provides (ports, connections) happens above, in the stack.
 *
 * Ownership mirrors the stream channels exactly:
 *   iprx head   NMI writes (frame_deliver), foreground reads   (producer)
 *   iprx tail   foreground writes (net_ip_read), NMI reads     (consumer)
 *   iptx head   foreground writes (net_ip_write), NMI reads
 *   iptx tail   NMI writes (ack release), foreground reads
 *
 * RX is larger than TX for the same reason NET_RING beats NET_TXRING: what
 * arrives must be buffered against a consumer that runs when it runs, while
 * what we send we can simply generate less of. 4 KB holds two MTU-sized
 * datagrams with room to spare (the MTU went 576 -> 1500 with the ring, see
 * uip-conf.h); when it fills, frame_fits refuses the frame and the link's
 * own backpressure does the rest — no datagram is ever half-delivered. */
#define IP_RXRING 4096
/* TX must hold ONE full-MTU datagram plus its 2-byte prefix, or a big
 * outbound segment can never be queued at all: net_ip_write is all-or-
 * nothing, so a ring smaller than the largest record does not backpressure
 * a writer, it wedges it — uIP would retry the same too-big segment forever.
 * One datagram is also all there ever is in flight (uIP is one-segment
 * stop-and-wait), so 2 KB is the fit, not a guess. */
#define IP_TXRING 2048
typedef char ip_rings_must_be_power_of_two[((IP_RXRING & (IP_RXRING - 1)) == 0 &&
                                            (IP_TXRING & (IP_TXRING - 1)) == 0)
                                               ? 1
                                               : -1];
static uint8_t iprx[IP_RXRING];
static uint16_t iprx_head; /* NMI writes */
static uint16_t iprx_tail; /* foreground writes */
static uint8_t iptx[IP_TXRING];
static uint16_t iptx_head; /* foreground writes */
static uint16_t iptx_tail; /* NMI writes */
/* Set from the HELLO reply (version >= 2). Cleared on a peer restart: the new
 * gateway process must claim IP support again before we send it packets. */
static volatile uint8_t ip_mode;
/* Peer-restart handoff, NMI -> foreground — the ch_reuse idiom. The daemon
 * dying mid-datagram leaves a partial length-prefixed datagram in the ring,
 * and every byte after it would be parsed one off forever. The NMI may not
 * clear the foreground-owned tail, so it records the boundary and raises the
 * flag; net_ip_read applies the discard and reports it once. */
static volatile uint8_t iprx_resync;
static volatile uint16_t iprx_mark;

/* Inbound control reassembly. Sized for a URL-length reply plus slack. */
#define CMSG_MAX 288
static uint8_t cmsg[CMSG_MAX];
static uint16_t cmsg_got;
static uint8_t cmsg_type;
static uint8_t cmsg_len;
static uint8_t cmsg_state; /* 0 want type, 1 want len, 2 collecting */
/* LINK_CH_KBDPAD: little-endian words, assembled across records because the
 * peer's packer splits one when it lands on a frame's last byte. The words
 * are kept here, where they arrive, and input.c says what they mean -- and
 * so net.c still links without input.c, as the Pico's host tests build it. */
static uint8_t kbdpad_lo, kbdpad_half;
volatile uint16_t net_kbdpad, net_kbdpad_rose, net_kbdpad_rose_at;

/* ------------------------------------------------------- the send window
 *
 * Frames tx_base..tx_next-1 are in flight, each in its own slot of txwin
 * (sequence & NET_WINDOW_MASK); tx_cursor is the next of them to put on the
 * wire again, or tx_next when none is owed. Each slot records how many control
 * and IP bytes its frame carried, because an acknowledgement releases exactly
 * those: recomputing from the queues' occupancy at ack time would be wrong, the
 * foreground enqueues more while frames are in flight and the queues would be
 * advanced past bytes never sent. txq_out and iptx_out are the bytes framed and
 * not yet acknowledged -- where the next frame starts reading.
 *
 * txwin_fill is each slot's first byte beyond its last frame's records.
 * Everything at and past it is already LINK_CH_PAD -- session_init establishes
 * it, every build_frame restores it -- so padding only covers the span the last
 * frame dirtied. The full-width pad loop it replaced was ~4,200 cycles a poll,
 * nearly all of build_frame's cost, inside NMI.
 *
 * Only the hardware build keeps more than one slot: the emulator build only
 * ever has the mailbox, which holds one frame per direction. */
#ifdef LINK_HW
#define NET_WINDOW_MAX 8
#define NET_WINDOW_DEFAULT 4
#else
#define NET_WINDOW_MAX 1
#define NET_WINDOW_DEFAULT 1
#endif
#define NET_WINDOW_MASK (NET_WINDOW_MAX - 1)
typedef char net_window_power_of_two[(NET_WINDOW_MAX & NET_WINDOW_MASK) == 0 ? 1 : -1];
static uint8_t txwin[NET_WINDOW_MAX][LINK_FRAME];
static uint16_t txwin_fill[NET_WINDOW_MAX];
static uint16_t txwin_ctrl[NET_WINDOW_MAX];
static uint16_t txwin_ip[NET_WINDOW_MAX];
static uint16_t txq_out, iptx_out;
static uint8_t tx_base, tx_next, tx_cursor;
static uint8_t peer_syn_seen;
static uint8_t syn_out;
static uint8_t rx_resync;
static uint8_t rxframe[LINK_FRAME];
static uint8_t rx_next;     /* the sequence we are waiting to receive */
/* Inbound IP held back after OUR restart, from a peer that did not restart
 * with us. Adopting its sequence is right, but its first frames were built
 * for the console that is gone and may be the middle of a datagram: ip.c
 * would read payload as a length prefix and misframe every datagram after.
 * Such a peer starts its streams again once it has seen our SYN (pico/link.c,
 * tools/gateway.py), so IP waits for proof that it has -- an ack inside our
 * own send window, on a frame after the first -- and the frames before that
 * lose only their IP. Nothing is lost by waiting: the peer has no flows
 * toward a console that has not yet had a HELLO answered. */
static uint8_t rx_hold;
/* Peer restarts adopted, NMI -> foreground: ip.c asks the new peer for its
 * HELLO at once when this moves, since a HELLO already queued for the old
 * one was discarded with everything else (net_service). */
volatile uint8_t net_peer_restarts;

/* `net tune` (shell.c): the window this end keeps in flight on the cable and
 * asks the peer to keep toward it (C2S_LINKWIN, sent from the foreground by
 * net_tune_announce). One byte, stored by the foreground and read by the NMI,
 * so there is no torn value to guard against. window_told is net_stat_hello as
 * of the last announcement: every HELLO reply is a peer that may have
 * restarted, and a restarted Pico is back at a window of 1. */
static volatile uint8_t net_window = NET_WINDOW_DEFAULT;
static uint16_t window_told;

/* The NMI handler calls net_service() from the moment interrupts are enabled,
 * which is before net_init() has run. Until then the link mailbox is still
 * uninitialised WRAM, and treating that garbage as a host claiming the link is
 * exactly the class of bug that cost this project a day in M3. Kernel statics
 * are zeroed by the C startup, so this is false until net_init says otherwise. */
static volatile uint8_t net_ready;

/* Foreground-poll hot window: the frame count until which extra polls are
 * worth running. Stamped by net_service() whenever a received frame carries
 * data — one write, in the poll itself, so it needs no locking: the NMI is
 * the only writer, and the foreground only compares it against snos_frames.
 * Three frames of slack keeps the window open across the gaps inside a bulk
 * transfer (peer turnaround, an ip_service pass) without keeping an idle
 * session polling: once nothing has arrived for three frames, the spin goes
 * back to the NMI's 60 Hz and the bridge files stop growing any faster. */
#define NET_HOT_FRAMES 3u
static volatile uint16_t net_hot_until;


/* kernel/netasm.s. The 8-bit sum of every frame byte except f[LINK_SUM], which
 * is what the C loop this replaces computed:
 *
 *   for (i = 0; i < LINK_FRAME; i++) if (i != LINK_SUM) s += f[i];
 *
 * In assembly because it is called twice per poll from inside NMI, and at -O0
 * each of those 64 far byte reads cost ~103 cycles. Two of them plus the frame
 * copies had net_service() running for 55% of every frame while an application
 * owned the PPU. n must be a non-zero multiple of 16; see the file for why. */
extern uint8_t net_checksum(const uint8_t *f, uint16_t n);

/* ...and here is that "must", made non-negotiable. The sum is unrolled sixteen
 * bytes at a time, so a LINK_FRAME that is not a multiple of 16 would quietly
 * sum only the whole blocks. The link would then die rather than corrupt — every
 * frame failing a checksum both ends compute differently — but it would look
 * like a dead cable, which is a bad way to find out. PLAN.md's item 3 raises
 * LINK_FRAME to 256, so this WILL be edited; a negative array size stops the
 * build if the new value does not fit the routine. */
typedef char net_checksum_needs_16_byte_frames[(LINK_FRAME % 16) == 0 ? 1 : -1];

/* kernel/netasm.s. One MVN: n bytes (n >= 1) out of bank $7E and into bank $7E
 * or bank $7F, at ~7 cycles a byte.
 *
 * It exists because the two ring copies below were the receive path's whole
 * cost. Measured on the vnc workload, ring_put ran for ~29,000 CPU cycles per
 * 124-byte frame — half a frame's worth of CPU, inside NMI, for one frame's
 * payload — and net_read cost the same again in the foreground. Not because a
 * byte loop is slow but because `ring[ch][ring_head[ch]]` rebuilds a 24-bit
 * address from a 2-D array and a volatile index for EVERY byte: ~230 cycles
 * each, worse than the mailbox loop the same file already replaced.
 *
 * A ring span can wrap, so each caller does at most two of these, split at the
 * end of the buffer. The destination bank restriction is checked in net_read;
 * see netasm.s for why it cannot simply be taken from the pointer. */
extern void ring_move(uint8_t __far *dst, const uint8_t *src, uint16_t n);

/* kernel/netasm.s: n bytes between any two banks, ~22 cycles a byte. */
extern void far_copy(uint8_t *dst, const uint8_t __far *src, uint16_t n);

static void session_init(void) {
  int j, k;
  txq_head = 0;
  txq_tail = 0;
  iprx_head = 0;
  iprx_tail = 0;
  iptx_head = 0;
  iptx_tail = 0;
  ip_mode = 0;
  iprx_resync = 0;
  /* Establish the pad invariant build_frame relies on: everything at and
   * beyond each slot's fill is LINK_CH_PAD. Statics are zeroed, and zero is a
   * valid channel number, not PAD. */
  for (k = 0; k < NET_WINDOW_MAX; k++) {
    for (j = 0; j < LINK_FRAME; j++) txwin[k][j] = LINK_CH_PAD;
    txwin_fill[k] = LINK_HDR;
    txwin_ctrl[k] = 0;
    txwin_ip[k] = 0;
  }
  txq_out = 0;
  iptx_out = 0;
  cmsg_got = 0;
  cmsg_state = 0;
  kbdpad_half = 0;
  tx_base = 0;
  tx_next = 0;
  tx_cursor = 0;
  rx_next = 0;
  peer_syn_seen = 0;
  /* Announce our own restart until the peer acknowledges a frame. On the
   * emulated backend the gateway also notices the bridge truncating its files,
   * but that is a property of files, not of the link — on real hardware this
   * flag is the only thing that tells the far end our sequence began again. */
  syn_out = 1;
  /* The mirror of syn_out. That flag tells the peer our sequence began again;
   * this one accepts that THEIRS did not. A gateway that has been running for
   * an hour is at seq 57 and has no reason to restart because we did, so we
   * adopt its sequence from the first frame rather than demanding it match a
   * counter we just zeroed. Announce and adopt are both needed — each alone
   * fixes one direction and leaves the other deadlocked. */
  rx_resync = 1;
  rx_hold = 0;
  window_told = 0xFFFFu; /* tell the peer once it has answered a HELLO */
  net_stat_rx_frames = 0;
  net_stat_hello = 0;
  net_text[0] = 0;
  net_gw_version = 0;
  net_stat_bad_sum = 0;
  net_stat_resent = 0;
  net_stat_refused = 0;
  /* rxframe only: txwin was JUST filled with LINK_CH_PAD to establish the
   * pad invariant, and zeroing it here silently defeated that — unused frame
   * bytes went out as 0x00, which both parsers happen to read as empty
   * control records rather than stopping at PAD. Benign by luck, wrong by
   * the invariant this file documents. */
  for (j = 0; j < LINK_FRAME; j++) rxframe[j] = 0;
  net_hot_until = snos_frames;
}

void net_init(void) {
  int i;
  for (i = 0; i < NET_CHANNELS; i++) {
    ring_head[i] = 0;
    ring_tail[i] = 0;
    ch_state[i] = NET_ST_FREE;
    ch_app[i] = 0;
    ch_listen[i] = 0;
    txring_head[i] = 0;
    txring_tail[i] = 0;
    ch_stack[i] = 0;
    ch_lport[i] = 0;
    unfwd_port[i] = 0;
  }
  session_init();
  link_init();
  net_ready = 1;
}

/* Keep handles owned until their callers observe ERROR and close them. */
static int fail_channels(void) {
  int i, n = 0;
  for (i = 1; i < NET_CHANNELS; i++) {
    if (ch_state[i] == NET_ST_FREE) continue;
    ch_state[i] = NET_ST_ERROR;
    n++;
  }
  return n;
}

/* A background ip_service may be suspended beneath the shell. Let it finish
 * before pinning the scheduler; net_ready then excludes the NMI's link work
 * without masking vblank for the whole rebuild. */
int net_reset(void) {
  int i, n;
  for (;;) {
    proc_lock();
    if (ip_reset() == 0) break;
    proc_unlock();
    kbd_idle();
  }
  NET_NMI_OFF();
  net_ready = 0;
  NET_NMI_ON();
  n = fail_channels();
  for (i = 1; i < NET_CHANNELS; i++) {
    ring_head[i] = ring_tail[i] = 0;
    txring_head[i] = txring_tail[i] = 0;
    ch_stack[i] = ch_listen[i] = 0;
    ch_lport[i] = unfwd_port[i] = 0;
  }
  session_init();
  link_reset();
  net_ready = 1;
  proc_unlock();
  return n;
}

int net_up(void) { return net_ready && link_present(); }

/* ------------------------------------------------------------- ring I/O */

static uint16_t ring_used(int ch) {
  return (uint16_t)((ring_head[ch] - ring_tail[ch]) & (NET_RING - 1));
}

static uint16_t ring_free(int ch) {
  return (uint16_t)(NET_RING - 1 - ring_used(ch));
}

/* Fill a channel's receive ring (uIP's side, via net_stack_rxput). Returns 0
 * if it would not fit — the caller's cue to close TCP's window rather than
 * lose bytes.
 *
 * Both this and net_read() advance their index ONCE, after the block move,
 * rather than per byte. That is what makes the copy a block move at all, and
 * the other side never sees a half-filled span. */
static int ring_put(int ch, const uint8_t *src, int n) {
  uint16_t h, first;
  if ((int)ring_free(ch) < n) return 0;
  if (n <= 0) return 1;               /* MVN would move 65536 for a count of 0 */
  h = ring_head[ch];
  /* The span may wrap, so it is up to two moves split at the end of the buffer.
   * ring_free() has already guaranteed both halves fit. */
  first = (uint16_t)(NET_RING - h);
  if (first > (uint16_t)n) first = (uint16_t)n;
  ring_move(&ring[ch][h], src, first);
  if (first < (uint16_t)n)
    ring_move(&ring[ch][0], src + first, (uint16_t)((uint16_t)n - first));
  ring_head[ch] = (uint16_t)((h + (uint16_t)n) & (NET_RING - 1));
  return 1;
}

/* ------------------------------------------------------------ IP ring I/O */

static uint16_t iprx_used(void) {
  return (uint16_t)((iprx_head - iprx_tail) & (IP_RXRING - 1));
}

/* NMI side, from frame_deliver. frame_fits has already reserved the space. */
static void iprx_put(const uint8_t *src, int n) {
  uint16_t h, first;
  if (n <= 0) return; /* MVN would move 65536 for a count of 0 */
  h = iprx_head;
  first = (uint16_t)(IP_RXRING - h);
  if (first > (uint16_t)n) first = (uint16_t)n;
  ring_move(&iprx[h], src, first);
  if (first < (uint16_t)n)
    ring_move(&iprx[0], src + first, (uint16_t)((uint16_t)n - first));
  iprx_head = (uint16_t)((h + (uint16_t)n) & (IP_RXRING - 1));
}

int net_ipmode(void) { return ip_mode != 0; }

int net_ip_read(uint8_t *dst, int max) {
  uint16_t t, avail, n, first;
  /* Apply a pending peer-restart discard first, and SAY SO: the caller's
   * half-assembled datagram is garbage now, and silently handing it the new
   * stream's bytes would graft two streams together at a random offset. */
  if (iprx_resync) {
    iprx_resync = 0;
    iprx_tail = iprx_mark;
    return -1;
  }
  if (max <= 0) return 0;
  t = iprx_tail;
  avail = (uint16_t)((iprx_head - t) & (IP_RXRING - 1));
  if (avail == 0) return 0;
  n = ((uint16_t)max < avail) ? (uint16_t)max : avail;
  /* ip.c's buffers are either kernel far data (bank $7E by the linker
   * invariant ring_move already relies on) or kernel STACK locals — bank $00
   * below $2000, which is the hardware mirror of $7E0000-$7E1FFF, so
   * ring_move's hard-coded $7E reaches the identical bytes. That mirror is
   * why no bank test is needed here where net_read has one: the only caller
   * is the kernel, whose stack can never leave the mirrored window. */
  first = (uint16_t)(IP_RXRING - t);
  if (first > n) first = n;
  ring_move(dst, &iprx[t], first);
  if (first < n) ring_move(dst + first, &iprx[0], (uint16_t)(n - first));
  iprx_tail = (uint16_t)((t + n) & (IP_RXRING - 1));
  return (int)n;
}

int net_ip_wspace(void) {
  return (int)(IP_TXRING - 1 -
               (uint16_t)((iptx_head - iptx_tail) & (IP_TXRING - 1)));
}

/* Queue TWO spans as ONE record: the length prefix and its datagram, with a
 * single publishing store.
 *
 * Two separate net_ip_write calls could be torn by the NMI's peer-restart
 * handler — which discards the queue with `tail = head` — landing between
 * them: the prefix is thrown away and the bare packet published after it,
 * so the daemon reads the IP header's first two bytes as a length and every
 * datagram after that is misframed, with no resync signal in this direction.
 * One head store means a restart takes the whole record or none of it. */
int net_ip_write2(const uint8_t *a, int na, const uint8_t *b, int nb) {
  uint16_t h, first;
  if (!ip_mode || na <= 0 || nb <= 0) return 0;
  if (na + nb > net_ip_wspace()) return 0;
  h = iptx_head;
  first = (uint16_t)(IP_TXRING - h);
  if (first > (uint16_t)na) first = (uint16_t)na;
  ring_move(&iptx[h], a, first);
  if (first < (uint16_t)na)
    ring_move(&iptx[0], a + first, (uint16_t)((uint16_t)na - first));
  h = (uint16_t)((h + (uint16_t)na) & (IP_TXRING - 1));
  /* ...continuing at where the first span ended, still UNPUBLISHED. */
  first = (uint16_t)(IP_TXRING - h);
  if (first > (uint16_t)nb) first = (uint16_t)nb;
  ring_move(&iptx[h], b, first);
  if (first < (uint16_t)nb)
    ring_move(&iptx[0], b + first, (uint16_t)((uint16_t)nb - first));
  iptx_head = (uint16_t)((h + (uint16_t)nb) & (IP_TXRING - 1));
  return na + nb;
}

int net_ip_write(const uint8_t *src, int n) {
  uint16_t h, first;
  if (!ip_mode || n <= 0) return 0;
  if (n > net_ip_wspace()) return 0; /* all or nothing; see net.h */
  /* src may be a kernel stack local (bank $00): same $7E mirror argument as
   * net_ip_read's destination, and the same reason it is safe. */
  h = iptx_head;
  first = (uint16_t)(IP_TXRING - h);
  if (first > (uint16_t)n) first = (uint16_t)n;
  ring_move(&iptx[h], src, first);
  if (first < (uint16_t)n)
    ring_move(&iptx[0], src + first, (uint16_t)((uint16_t)n - first));
  iptx_head = (uint16_t)((h + (uint16_t)n) & (IP_TXRING - 1));
  return n;
}

/* ------------------------------------------------- stack-backed channels */

int net_stack_is(int ch) {
  return (ch > 0 && ch < NET_CHANNELS) ? ch_stack[ch] : 0;
}

int net_stack_claim(void) {
  int ch;
  proc_lock();
  for (ch = 1; ch < NET_CHANNELS; ch++)
    if (ch_state[ch] == NET_ST_FREE) break;
  if (ch >= NET_CHANNELS) {
    proc_unlock();
    return -1;
  }
  /* Bracketed against the NMI's ch_state writers (S2C_ERR, peer restart):
   * the claim must not interleave with a sweep that is failing channels. */
  NET_NMI_OFF();
  ring_head[ch] = 0;
  ring_tail[ch] = 0;
  txring_head[ch] = 0;
  txring_tail[ch] = 0;
  ch_listen[ch] = 0;
  ch_stack[ch] = 1;
  ch_state[ch] = NET_ST_OPENING;
  NET_NMI_ON();
  proc_unlock();
  return ch;
}

void net_stack_setstate(int ch, int st) {
  if (ch > 0 && ch < NET_CHANNELS && ch_state[ch] != NET_ST_FREE)
    ch_state[ch] = (uint8_t)st;
}

int net_stack_rxfree(int ch) {
  if (ch <= 0 || ch >= NET_CHANNELS) return 0;
  return (int)ring_free(ch);
}

int net_stack_rxput(int ch, const uint8_t *p, int n) {
  if (ch <= 0 || ch >= NET_CHANNELS) return 0;
  return ring_put(ch, p, n);
}

int net_stack_txused(int ch) {
  if (ch <= 0 || ch >= NET_CHANNELS) return 0;
  return (int)((txring_head[ch] - txring_tail[ch]) & (NET_TXRING - 1));
}

int net_stack_txcopy(int ch, uint8_t *dst, int max) {
  uint16_t t, avail, n, first;
  if (ch <= 0 || ch >= NET_CHANNELS || max <= 0) return 0;
  avail = (uint16_t)((txring_head[ch] - txring_tail[ch]) & (NET_TXRING - 1));
  if (avail == 0) return 0;
  n = ((uint16_t)max < avail) ? (uint16_t)max : avail;
  t = txring_tail[ch];
  first = (uint16_t)(NET_TXRING - t);
  if (first > n) first = n;
  ring_move(dst, &txring[ch][t], first);
  if (first < n) ring_move(dst + first, &txring[ch][0], (uint16_t)(n - first));
  return (int)n;
}

void net_stack_txdrop(int ch, int n) {
  if (ch <= 0 || ch >= NET_CHANNELS || n <= 0) return;
  txring_tail[ch] =
      (uint16_t)((txring_tail[ch] + (uint16_t)n) & (NET_TXRING - 1));
}

int net_read(int ch, uint8_t __far *dst, int max) {
  uint16_t t, avail, n, first;
  if (ch < 0 || ch >= NET_CHANNELS) return 0;
  if (max <= 0) return 0;
  t = ring_tail[ch];
  avail = (uint16_t)((ring_head[ch] - t) & (NET_RING - 1));
  if (avail == 0) return 0;
  n = ((uint16_t)max < avail) ? (uint16_t)max : avail;

  /* MVN's destination bank is an instruction byte, so ring_move() can only
   * reach the two banks netasm.s spells out: $7F for an application's arena and
   * $7E for a kernel caller. An application is free to hand us a buffer on its
   * stack, which is in bank $00 — that is not an error, it is just not a block
   * move, so it keeps the byte loop below rather than being refused.
   *
   * Every application shipped today reads into a static, so that branch is not
   * reachable from the tests. It was checked by forcing it — `if (0 && ...)` —
   * and running test-net and test-vnc, which is 49152 pixels through the byte
   * loop; do that again if you touch either half. */
  if ((uint8_t)((uint32_t)dst >> 16) == 0x7Eu ||
      (uint8_t)((uint32_t)dst >> 16) == 0x7Fu) {
    first = (uint16_t)(NET_RING - t);
    if (first > n) first = n;
    ring_move(dst, &ring[ch][t], first);
    if (first < n)
      ring_move(dst + first, &ring[ch][0], (uint16_t)(n - first));
  } else {
    uint16_t i;
    for (i = 0; i < n; i++) dst[i] = ring[ch][(uint16_t)((t + i) & (NET_RING - 1))];
  }
  ring_tail[ch] = (uint16_t)((t + n) & (NET_RING - 1));
  return (int)n;
}

int net_state(int ch) {
  if (ch < 0 || ch >= NET_CHANNELS) return NET_ST_ERROR;
  /* Report the stream as open while there is still buffered data, so a caller
   * loops on state and never abandons bytes that already arrived. */
  if (ch_state[ch] == NET_ST_EOF && ring_used(ch) > 0) return NET_ST_OPEN;
  return ch_state[ch];
}

/* -------------------------------------------------------- outbound control */

static int txq_space(void) {
  return (int)(TXQ_SIZE - 1 - (uint16_t)((txq_head - txq_tail) & (TXQ_SIZE - 1)));
}

static void txq_put(uint8_t b) {
  txq[txq_head] = b;
  txq_head = (uint16_t)((txq_head + 1) & (TXQ_SIZE - 1));
}

/* One whole control record, published by a single store of txq_head -- the
 * net_ip_write2 rule, for the same reason. When the peer restarts, the NMI
 * throws away everything queued with `tail = head` (net_service), and a head
 * that moved byte by byte could be caught mid-record: the record's first
 * bytes discarded, its last ones published after them, and the new peer's
 * parser reading a length as a verb. Callers check txq_space() and hold
 * proc_lock(), as they always have. */
static void txq_record(const uint8_t *r, uint8_t n) {
  uint16_t h = txq_head;
  uint8_t i;
  for (i = 0; i < n; i++) {
    txq[h] = r[i];
    h = (uint16_t)((h + 1) & (TXQ_SIZE - 1));
  }
  txq_head = h;
}

/* C2S_IPFWD / C2S_IPUNFWD: [verb][3][ch][port lo][port hi]. */
static void txq_port_record(uint8_t verb, uint8_t ch, uint16_t port) {
  uint8_t r[5];
  r[0] = verb;
  r[1] = 3;
  r[2] = ch;
  r[3] = (uint8_t)(port & 0xFF);
  r[4] = (uint8_t)(port >> 8);
  txq_record(r, 5);
}

/* Called by the trap layer when an app — rather than the shell — opened one.
 * The tag says WHICH app (M10): 1 = the foreground app, 2.. = background
 * process id + 1. Reaping by tag is what stops a foreground app's exit from
 * tearing down a background process's connections — with one shared bit,
 * `edit` quitting would have closed `irc`'s server link. */
void net_own(int ch, uint8_t tag) {
  if (ch > 0 && ch < NET_CHANNELS) ch_app[(uint8_t)ch] = tag;
}

/* Close whatever the departing application did not. Returns how many, so the
 * caller can say so: silently cleaning up teaches nobody, and a count that is
 * usually zero is a useful thing to notice when it is not. */
int net_reap(uint8_t tag) {
  int i, n = 0;
  for (i = 1; i < NET_CHANNELS; i++) {
    if (ch_app[i] == tag && tag != 0) {
      if (ch_state[i] != NET_ST_FREE) {
        net_close(i);
        n++;
      }
      ch_app[i] = 0;
    }
  }
  return n;
}

/* Ask the far end to say something. The reply lands in ctrl_message and bumps
 * net_stat_hello; the caller watches that counter rather than blocking here,
 * because the link is serviced from NMI and this runs in the foreground. */
/* Room left in a channel's send queue. Callers check this rather than
 * discovering a short write, because a partial send of an HTTP header is worse
 * than none: the peer parses what arrived. */
int net_wspace(int ch) {
  if (ch <= 0 || ch >= NET_CHANNELS) return 0;
  return (int)(NET_TXRING - 1 -
               (uint16_t)((txring_head[ch] - txring_tail[ch]) & (NET_TXRING - 1)));
}

/* Queue bytes for the peer. Returns how many were taken — never partial past
 * what space allows, and never blocking; uIP segments the queue out whenever
 * ip_service next runs. */
int net_write(int ch, const uint8_t *src, int n) {
  int room;
  uint16_t h, first;
  if (ch <= 0 || ch >= NET_CHANNELS) return 0;
  /* EOF too, not just OPEN. EOF means the PEER stopped sending; the direction
   * we write in is still open, and refusing here is what made an echo server
   * spin forever on a client that had half-closed — which is every client that
   * sends a request and then waits, including every HTTP client. */
  if (ch_state[ch] != NET_ST_OPEN && ch_state[ch] != NET_ST_EOF) return 0;
  room = net_wspace(ch);
  if (n > room) n = room;
  if (n <= 0) return 0;            /* MVN would move 65536 for a count of 0 */
  /* Block moves, split at the wrap: `txring[ch][head]` rebuilt a 24-bit
   * address from a 2-D array and a volatile index for EVERY byte, the
   * ~230-cycles/byte shape measured twice in this file. src is either kernel
   * far data or a kernel stack local — bank $00 below $2000, the hardware
   * mirror of $7E0000 — so ring_move's fixed source bank reaches both. */
  h = txring_head[ch];
  first = (uint16_t)(NET_TXRING - h);
  if (first > (uint16_t)n) first = (uint16_t)n;
  ring_move(&txring[ch][h], src, first);
  if (first < (uint16_t)n)
    ring_move(&txring[ch][0], src + first, (uint16_t)((uint16_t)n - first));
  txring_head[ch] = (uint16_t)((h + (uint16_t)n) & (NET_TXRING - 1));
  return n;
}

/* net_write's twin for SYS_NET_WRITE: the bytes come from an application's
 * own bank, so they go straight into the ring with far_copy (netasm.s) --
 * MVN's banks are immediates. It replaced a copy through a 128-byte buffer
 * on the caller's stack, which also capped every write at 128 bytes; now a
 * write takes whatever net_wspace says the ring has room for. */
int net_write_far(int ch, const uint8_t __far *src, int n) {
  int room;
  uint16_t h, first;
  if (ch <= 0 || ch >= NET_CHANNELS) return 0;
  if (ch_state[ch] != NET_ST_OPEN && ch_state[ch] != NET_ST_EOF) return 0;
  room = net_wspace(ch);
  if (n > room) n = room;
  if (n <= 0) return 0;
  h = txring_head[ch];
  first = (uint16_t)(NET_TXRING - h);
  if (first > (uint16_t)n) first = (uint16_t)n;
  far_copy(&txring[ch][h], src, first);
  if (first < (uint16_t)n)
    far_copy(&txring[ch][0], src + first, (uint16_t)((uint16_t)n - first));
  txring_head[ch] = (uint16_t)((h + (uint16_t)n) & (NET_TXRING - 1));
  return n;
}

/* A plain TCP stream on the console's own stack. The channel it returns
 * behaves exactly as the gateway-backed ones once did — OPENING, OPEN, then
 * EOF or ERROR — which is why no application changed at the cutover. Requires
 * a daemon that claimed NAT support (HELLO >= 2); there is no other backing
 * any more. */
int net_connect(const char *hostport) {
  if (!ip_up()) return -1;
  return ip_connect(hostport);
}

/* End the current connection but keep the port.
 *
 * Refuses while the channel still has unsent bytes, and the caller retries.
 * That is not politeness: build_frame packs the control queue BEFORE channel
 * data, so a HANGUP queued alongside a half-sent response would overtake it and
 * the client would get a truncated page. Making the caller wait is the only
 * ordering guarantee available without a second queue. */
int net_hangup(int ch) {
  if (ch <= 0 || ch >= NET_CHANNELS) return -1;
  if (!ch_listen[ch] || !ch_stack[ch]) return -1;

  /* Refuse-until-drained protects a RESPONSE in flight — on a live
   * connection. On a DEAD one (ERROR: the peer reset, or the daemon
   * restarted) the queued bytes can never drain and never should: refusing
   * here wedged the caller's hangup loop forever, and rebinding the channel
   * with the corpse's bytes still queued served the NEXT client the middle
   * of a dead session's packet — found live, as a Minecraft client
   * disconnecting on garbage that was 25 chunks into somebody else's join. */
  if (ch_state[ch] != NET_ST_ERROR &&
      ((txring_head[ch] - txring_tail[ch]) & (NET_TXRING - 1)) != 0)
    return -1;

  /* The same refusal-until-drained rule the stream path had, but the queue
   * that matters is uIP's unacknowledged segment. The port stays bound; only
   * the connection ends, which is what makes a server a loop rather than a
   * fresh listen per request. A dead connection is abandoned (abort, port
   * kept) rather than closed gracefully — grace needs a living peer. */
  if (ch_state[ch] == NET_ST_ERROR) ip_abandon(ch);
  else if (ip_hangup(ch) < 0) return -1;
  proc_lock();
  NET_NMI_OFF();
  ring_head[ch] = 0;
  ring_tail[ch] = 0;
  txring_head[ch] = 0;
  txring_tail[ch] = 0;
  ch_state[ch] = NET_ST_LISTEN;
  NET_NMI_ON();
  proc_unlock();
  return 0;
}

int net_listen(uint16_t port) {
  int ch;
  if (!ip_up()) return -1;

  /* The stack's own listen. uIP binds the port on this side; the daemon is
   * asked for a matching real socket (the forward), because the console is
   * behind the NAT and cannot be dialled without one. The channel is LISTEN,
   * then OPEN per connection, then back to LISTEN on hangup. */
  ch = net_stack_claim();
  if (ch < 0) return -1;
  if (ip_listen(ch, port) < 0) {
    net_close(ch);
    return -1;
  }
  proc_lock();
  if (txq_space() < 5) {
    proc_unlock();
    net_close(ch);
    return -1;
  }
  txq_port_record(C2S_IPFWD, (uint8_t)ch, port);
  ch_listen[ch] = 1;
  ch_lport[ch] = port;
  ch_state[ch] = NET_ST_LISTEN;
  proc_unlock();
  return ch;
}

/* Emit any forward withdrawal that could not fit when its channel closed.
 * Foreground only (txq_head is foreground-owned), and called once per
 * ip_service pass — the port stays held by the daemon until this lands. */
void net_unfwd_retry(void) {
  int ch;
  for (ch = 1; ch < NET_CHANNELS; ch++) {
    if (unfwd_port[ch] == 0) continue;
    proc_lock();
    if (txq_space() >= 5) {
      txq_port_record(C2S_IPUNFWD, (uint8_t)ch, unfwd_port[ch]);
      unfwd_port[ch] = 0;
    }
    proc_unlock();
  }
}

int net_hello(void) {
  int rc = -1;
  if (!net_up()) return -1;
  proc_lock();
  if (txq_space() >= 2) {
    uint8_t r[2];
    r[0] = C2S_HELLO;
    r[1] = 0;
    txq_record(r, 2);
    rc = 0;
  }
  proc_unlock();
  return rc;
}

void net_close(int ch) {
  if (ch < 1 || ch >= NET_CHANNELS) return;
  proc_lock();
  /* The socket that matters is uIP's, and it is torn down here — BEFORE the
   * state goes FREE, so ip_close can still find the connection by channel.
   *
   * Withdraw the forward first: the daemon's socket outlives the channel
   * otherwise, and the next run of the same application finds its port
   * taken by its own previous life. */
  if (ch_listen[ch] && ch_lport[ch] != 0) {
    if (txq_space() >= 5) {
      txq_port_record(C2S_IPUNFWD, (uint8_t)ch, ch_lport[ch]);
    } else {
      unfwd_port[ch] = ch_lport[ch];   /* net_unfwd_retry sends it later */
    }
  }
  if (ch_stack[ch]) ip_close(ch);
  ch_stack[ch] = 0;
  ch_lport[ch] = 0;
  NET_NMI_OFF();
  ch_state[ch] = NET_ST_FREE;
  ring_head[ch] = 0;
  ring_tail[ch] = 0;
  ch_app[ch] = 0;
  ch_listen[ch] = 0;
  txring_head[ch] = 0;
  txring_tail[ch] = 0;
  NET_NMI_ON();
  proc_unlock();
}

/* --------------------------------------------------------- inbound control */

static void ctrl_message(void) {
  uint8_t ch;
  switch (cmsg_type) {
  case S2C_HELLO: {
    /* The version, then the gateway's own address as text. The SNES has no IP,
     * so anything it ever serves is reached at the gateway's — which makes this
     * the answer to "what do I point a browser at?" and worth having in the one
     * command people already run. */
    uint8_t i, n;
    net_gw_version = (cmsg_got >= 1) ? cmsg[0] : 0;
    n = (uint8_t)(cmsg_got > 1 ? cmsg_got - 1 : 0);
    if (n > NET_TEXT_MAX) n = NET_TEXT_MAX;
    for (i = 0; i < n; i++) net_text[i] = (char)cmsg[1 + i];
    net_text[n] = 0;
    /* Version 2 = the daemon does user-mode NAT and will route LINK_CH_IP
     * records (M11). Gated on the reply, not assumed: the Lua test gateways
     * and any older daemon stay at 1 and never see an IP record. */
    ip_mode = (uint8_t)(net_gw_version >= 2 ? 1 : 0);
    net_stat_hello++;                 /* last: the shell watches this to wake */
    break;
  }
  case S2C_ERR:
    /* The daemon could not hold the socket a forward asked for ("address
     * already in use"), reported against the listening channel. */
    if (cmsg_got >= 1) {
      ch = cmsg[0];
      /* The FREE guard is not optional: an ERR for a channel the app already
       * closed (abort + reap, then the daemon's queued error lands) would
       * otherwise park a FREE channel in ERROR, where nothing can ever
       * allocate or reap it again -- each occurrence permanently eats one of
       * the channels. */
      if (ch < NET_CHANNELS && ch_state[ch] != NET_ST_FREE)
        ch_state[ch] = NET_ST_ERROR;
    }
    break;
  default:
    break;
  }
}

static void ctrl_bytes(const uint8_t *p, int n) {
  int i;
  for (i = 0; i < n; i++) {
    uint8_t b = p[i];
    if (cmsg_state == 0) {
      cmsg_type = b;
      cmsg_state = 1;
    } else if (cmsg_state == 1) {
      cmsg_len = b;
      cmsg_got = 0;
      cmsg_state = (uint8_t)(cmsg_len ? 2 : 0);
      if (cmsg_state == 0) ctrl_message();
    } else {
      if (cmsg_got < CMSG_MAX) cmsg[cmsg_got] = b;
      cmsg_got++;
      if (cmsg_got >= cmsg_len) {
        ctrl_message();
        cmsg_state = 0;
      }
    }
  }
}

/* ------------------------------------------------------------- the service */

/* Can every record in this frame be delivered? Checked BEFORE anything is
 * consumed, because a frame is acked as a unit: accepting half of one and then
 * refusing the rest would lose data on the retransmit. */
static int frame_fits(const uint8_t *f) {
  int o = LINK_HDR;
  uint16_t need_ip = 0;
  uint16_t need_kbd = 0;
  while (o + 2 <= LINK_FRAME) {
    uint8_t ch = f[o];
    uint8_t len = f[o + 1];
    if (ch == LINK_CH_PAD) break;
    if (o + 2 + len > LINK_FRAME) break;
    if (ch == LINK_CH_KBD) need_kbd = (uint16_t)(need_kbd + len);
    /* Only IP records need room reserved; control bytes feed a state machine
     * with no backpressure of its own, and any other channel number is
     * something this kernel no longer speaks — stepped over by its length,
     * which is the forward-compatibility rule both sides have always had.
     * Held IP (rx_hold) is dropped, so it needs no room. */
    if (ch == LINK_CH_IP && !rx_hold) need_ip = (uint16_t)(need_ip + len);
    o += 2 + len;
  }
  /* The tail read here is foreground-owned and may be stale; stale only
   * UNDERSTATES free space, so the worst case is one refused frame that the
   * peer resends — the ordinary backpressure path, never corruption. */
  if (need_ip > (uint16_t)(IP_RXRING - 1 - iprx_used())) return 0;
  /* Keyboard bytes go to the input ring, which the foreground drains; a full
   * ring refuses the frame and the peer resends, same as IP. */
  if (need_kbd > (uint16_t)(MB_IN_RING_SIZE - (uint16_t)(MB_IN_WINDEX - MB_IN_RINDEX)))
    return 0;
  return 1;
}

/* Runs in net_service: in the NMI, or with it masked (net_service_fg). That
 * is the writer's side of pad_read's masked read-and-clear, the side
 * pad_latch is on, so no reader can come between the two stores. */
static void kbdpad_word(uint16_t v) {
  uint16_t rose = (uint16_t)(v & ~net_kbdpad);
  if (rose) {
    /* Presses nobody read in time are not carried into a fresh one. */
    if ((uint16_t)(snos_frames - net_kbdpad_rose_at) > NET_KBDPAD_TAP) net_kbdpad_rose = 0;
    net_kbdpad_rose |= rose;
    net_kbdpad_rose_at = snos_frames;
  }
  net_kbdpad = v;
}

static void kbdpad_bytes(const uint8_t *p, uint8_t n) {
  uint8_t i;
  for (i = 0; i < n; i++) {
    if (kbdpad_half) kbdpad_word((uint16_t)(kbdpad_lo | ((uint16_t)p[i] << 8)));
    else kbdpad_lo = p[i];
    kbdpad_half = (uint8_t)!kbdpad_half;
  }
}

static void frame_deliver(const uint8_t *f) {
  int o = LINK_HDR;
  while (o + 2 <= LINK_FRAME) {
    uint8_t ch = f[o];
    uint8_t len = f[o + 1];
    if (ch == LINK_CH_PAD) break;
    if (o + 2 + len > LINK_FRAME) break;
    if (ch == LINK_CH_CTRL) ctrl_bytes(&f[o + 2], len);
    else if (ch == LINK_CH_IP) {
      if (!rx_hold) iprx_put(&f[o + 2], len); /* frame_fits reserved the space */
    }
    else if (ch == LINK_CH_KBD) {
      uint8_t i;
      for (i = 0; i < len; i++) kbd_push(f[o + 2 + i]);
    }
    /* Held like IP: a stream cut mid-word would pair every word after it
     * wrongly. The peer sends its whole word again once it has seen us. */
    else if (ch == LINK_CH_KBDPAD) {
      if (!rx_hold) kbdpad_bytes(&f[o + 2], len);
    }
    /* Anything else is a record type this kernel no longer speaks: skipped. */
    o += 2 + len;
  }
}

/* Build the frame with sequence tx_next into its slot, from whatever is queued
 * beyond the bytes already in flight. Returns 1 if it carries records. It is
 * not in flight until the caller commits it. */
static int build_frame(uint8_t *f, uint8_t k) {
  int o = LINK_HDR;
  int n;

  f[LINK_SEQ] = tx_next;
  f[LINK_ACK] = (uint8_t)(rx_next - 1);
  f[LINK_FLAGS] = syn_out ? LINK_F_SYN : 0;

  /* Control first, then IP, greedily until the frame is full. Control goes
   * first on purpose: a forward-withdrawal queued behind a datagram backlog
   * would leave the daemon's socket alive after the listener died. */
  n = (int)((txq_head - txq_tail - txq_out) & (TXQ_SIZE - 1));
  if (n > 0) {
    int room = LINK_FRAME - o - 2;
    uint16_t at = (uint16_t)((txq_tail + txq_out) & (TXQ_SIZE - 1));
    uint16_t first;
    if (n > room) n = room;
    if (n > 255) n = 255;
    f[o++] = LINK_CH_CTRL;
    f[o++] = (uint8_t)n;
    /* Block moves, split at the ring wrap — this runs inside NMI, and the
     * per-byte loop it replaces rebuilt a ring address every iteration at
     * the ~230-cycles/byte cost this file has already measured twice. */
    first = (uint16_t)(TXQ_SIZE - at);
    if (first > (uint16_t)n) first = (uint16_t)n;
    ring_move(&f[o], &txq[at], first);
    if (first < (uint16_t)n)
      ring_move(&f[o + first], &txq[0], (uint16_t)((uint16_t)n - first));
    o += n;
    f[LINK_FLAGS] |= LINK_F_DATA;
  }
  txwin_ctrl[k] = (uint16_t)(n > 0 ? n : 0);

  /* Then IP — which is everything except control now. The channel txrings
   * never reach this function: uIP consumes them in the foreground and what
   * leaves the wire is the resulting datagrams. */
  {
    int have = (int)((iptx_head - iptx_tail - iptx_out) & (IP_TXRING - 1));
    int room = LINK_FRAME - o - 2;
    uint16_t at = (uint16_t)((iptx_tail + iptx_out) & (IP_TXRING - 1));
    uint16_t first;
    txwin_ip[k] = 0;
    if (ip_mode && have > 0 && room > 0) {
      if (have > room) have = room;
      if (have > 255) have = 255;
      f[o++] = LINK_CH_IP;
      f[o++] = (uint8_t)have;
      /* Same block-move discipline as the control copy above; since the
       * cutover EVERY payload byte on the wire passes through here, so
       * this is the frame builder's whole variable cost. */
      first = (uint16_t)(IP_TXRING - at);
      if (first > (uint16_t)have) first = (uint16_t)have;
      ring_move(&f[o], &iptx[at], first);
      if (first < (uint16_t)have)
        ring_move(&f[o + first], &iptx[0], (uint16_t)((uint16_t)have - first));
      o += have;
      txwin_ip[k] = (uint16_t)have;
      f[LINK_FLAGS] |= LINK_F_DATA;
    }
  }

  /* Re-pad only what this slot's previous frame covered (see txwin_fill);
   * the checksum still sums the whole frame, so a hole in the invariant would
   * fail loudly on the far side rather than corrupt silently. */
  {
    uint16_t end = txwin_fill[k];
    uint16_t rec_end = (uint16_t)o;
    while (o < (int)end) f[o++] = LINK_CH_PAD;
    txwin_fill[k] = rec_end;
  }
  f[LINK_SUM] = 0;
  f[LINK_SUM] = net_checksum(f, LINK_FRAME);
  return (f[LINK_FLAGS] & LINK_F_DATA) != 0;
}

/* The mailbox holds one frame per direction: a second fresh frame before the
 * bridge has read the first would overwrite it unread. */
static uint8_t net_window_now(void) {
  return link_is_serial() ? net_window : 1;
}

int net_tune_get(int what) {
  if (what == NET_TUNE_WINDOW) return net_window;
  if (what == NET_TUNE_EXCH) return link_serial_exch(0);
  return -1;
}

int net_tune_set(int what, int value) {
  if (what == NET_TUNE_WINDOW) {
    if (value < 1) value = 1;
    if (value > NET_WINDOW_MAX) value = NET_WINDOW_MAX;
    net_window = (uint8_t)value;
    window_told = 0xFFFFu; /* tell the peer on the next pass */
    return value;
  }
  if (what == NET_TUNE_EXCH) return link_serial_exch(value < 1 ? 1 : value);
  return -1;
}

/* Ask the peer for the window we keep (C2S_LINKWIN), from the foreground
 * because txq's head is foreground-owned. Once per HELLO reply: a peer that
 * restarted has forgotten, and comes back answering HELLO. Only on the cable:
 * the emulator's gateways have no window to set. */
void net_tune_announce(void) {
  if (!link_is_serial() || net_gw_version == 0 || window_told == net_stat_hello) return;
  proc_lock();
  if (txq_space() >= 3) {
    uint8_t r[3];
    r[0] = C2S_LINKWIN;
    r[1] = 1;
    r[2] = net_window;
    txq_record(r, 3);
    window_told = net_stat_hello;
  }
  proc_unlock();
}

/* One poll: take what arrived, act on it, build the answer, put it on the wire.
 *
 * THE ORDER IS THE POINT, and it used to be receive-then-send-then-build. That
 * shape put the wire write BEFORE the frame was built, so the frame it wrote was
 * necessarily the one built at the end of the previous poll — and an ack for
 * something that arrived at poll k therefore left at poll k+1. Two polls per
 * data frame, half of them spent carrying a stale ack while the peer
 * retransmitted at us. Building between the two halves is the whole fix: the ack
 * for what just arrived goes out in the same poll it arrived on.
 *
 * Receive strictly before send is still an invariant, not a preference — see
 * link.h. Nothing may touch the link mailbox between the two calls. */
void net_service(void) {
  uint8_t n, k, flying, w;
  if (!net_ready) return;
  if (!net_up()) {
    /* No adapter -- unplugged, or 120 bad exchanges (link_serial.c) -- so no
     * keyboard either, and nothing will ever say its keys came up. */
    kbdpad_half = 0;
    if (net_kbdpad) kbdpad_word(0);
    return;
  }

  if (link_recv(rxframe)) {
    net_stat_rx_frames++;
    if (rxframe[LINK_SUM] != net_checksum(rxframe, LINK_FRAME)) {
      /* Corrupt. Do nothing at all — no ack, no state change — and the peer
       * retransmits. Silently ignoring is the correct response; anything
       * cleverer risks acting on bytes we know are wrong. */
      net_stat_bad_sum++;
    } else {
      if (rx_resync) {
        rx_resync = 0;
        rx_next = rxframe[LINK_SEQ];
        /* A peer announcing its own restart starts its streams afresh; one
         * that does not is mid-stream (rx_hold). */
        rx_hold = (uint8_t)((rxframe[LINK_FLAGS] & LINK_F_SYN) == 0);
      } else if (rx_hold &&
                 (uint8_t)(rxframe[LINK_ACK] - tx_base + 1) <=
                     (uint8_t)(tx_next - tx_base)) {
        /* It acks inside our window -- tx_base - 1 included, which is
         * "nothing of yours yet, but I have your sequence" -- so it has
         * adopted our restart, and what it sends now begins at a datagram.
         * Never on the first frame: that one was built before the peer could
         * have seen us, and its ack field can match by chance. */
        rx_hold = 0;
      }

      /* An ack releases every frame up to it — acknowledgements are
       * cumulative on every peer. Both sources a frame drew from advance by
       * what THAT frame carried, recorded when it was built. */
      {
        uint8_t n = (uint8_t)(rxframe[LINK_ACK] - tx_base + 1);
        if (n != 0 && n <= (uint8_t)(tx_next - tx_base)) {
          do {
            uint8_t k = (uint8_t)(tx_base & NET_WINDOW_MASK);
            txq_tail = (uint16_t)((txq_tail + txwin_ctrl[k]) & (TXQ_SIZE - 1));
            txq_out = (uint16_t)(txq_out - txwin_ctrl[k]);
            iptx_tail = (uint16_t)((iptx_tail + txwin_ip[k]) & (IP_TXRING - 1));
            iptx_out = (uint16_t)(iptx_out - txwin_ip[k]);
            txwin_ctrl[k] = 0;
            txwin_ip[k] = 0;
            tx_base++;
          } while (--n != 0);
          if ((uint8_t)(tx_cursor - tx_base) > (uint8_t)(tx_next - tx_base))
            tx_cursor = tx_base;
          syn_out = 0; /* it acked us, so it has our sequence base */
          /* The peer just acknowledged us: it is alive and mid-conversation,
           * so keep the foreground polling — the next frame we build (more of
           * this transfer, usually) should not wait for an NMI. */
          net_hot_until = (uint16_t)(snos_frames + NET_HOT_FRAMES);
        }
      }
      /* The peer restarted. Adopt its sequence base, once, and fail every
       * channel: whatever sockets those represented died with it, so an app
       * blocked on one gets an error instead of waiting forever. Acting on
       * every SYN frame rather than once per episode would re-deliver
       * retransmissions, because the seq check below is what discards those. */
      if ((rxframe[LINK_FLAGS] & LINK_F_SYN) != 0) {
        if (!peer_syn_seen) {
          peer_syn_seen = 1;
          rx_next = rxframe[LINK_SEQ];
          fail_channels();
          /* The IP layer dies with the daemon too. Mode drops until the NEW
           * process claims it in its own HELLO; unsent datagrams are for a
           * NAT table that no longer exists (tail skip: both stores are
           * NMI-owned here, the txring_tail precedent in ctrl_message); and
           * received bytes may end mid-datagram, which net_ip_read reports
           * to ip.c exactly once via the mark/flag handoff. */
          net_gw_version = 0;
          ip_mode = 0;
          /* The control stream broke mid-record just as surely as the IP
           * one did: a [type][len] whose payload never arrived would eat
           * the NEW daemon's HELLO reply as the dead record's body, and
           * mis-set the very version that decides ip_mode. The IP side got
           * an explicit resync for this; the control side had nothing. */
          cmsg_state = 0;
          cmsg_got = 0;
          /* And whatever its keyboard held died with it; it says again. */
          kbdpad_half = 0;
          kbdpad_word(0);
          iptx_tail = iptx_head;
          iptx_out = 0;
          for (n = 0; n < NET_WINDOW_MAX; n++) txwin_ip[n] = 0;
          iprx_mark = iprx_head;
          iprx_resync = 1;
          /* And our streams toward it must begin again at a record. The
           * frames in flight were cut for the dead peer: the first may be
           * the middle of a datagram or a control record, and the new peer
           * -- which adopted our sequence, not our stream -- would read it
           * as a start (restart_validation_test.py). So every one of them
           * goes out empty: the peer still takes its sequence number, but
           * no byte of it. Unsent control goes too, whole records only
           * (txq_record), and a HELLO among it is asked again at once
           * (net_peer_restarts, ip.c). What the peer took before we could
           * adopt -- a frame already on the wire -- is its side's problem,
           * and it solves it: it holds our IP until we ack inside its
           * window (pico/link.c, tools/gateway.py).
           *
           * Not while our own SYN is still out: then nothing we sent has
           * been acknowledged, our streams are still at their start, and
           * the new peer receives them whole -- HELLO included. */
          if (!syn_out) {
            for (k = tx_base; k != tx_next; k++)
              txwin[k & NET_WINDOW_MASK][LINK_HDR] = LINK_CH_PAD;
            for (n = 0; n < NET_WINDOW_MAX; n++) txwin_ctrl[n] = 0;
            txq_tail = txq_head;
            txq_out = 0;
          }
          rx_hold = 0;
          net_peer_restarts++;
        }
      } else {
        peer_syn_seen = 0;
      }

      if ((rxframe[LINK_FLAGS] & LINK_F_DATA) != 0) {
        /* Data is moving (a fresh frame OR a retransmission — either means
         * the peer has more to say), so invite foreground polls for a few
         * frames. Refused frames count too: the faster the retransmit is
         * answered once the ring drains, the shorter the stall. */
        net_hot_until = (uint16_t)(snos_frames + NET_HOT_FRAMES);
        if (rxframe[LINK_SEQ] == rx_next) {
          if (frame_fits(rxframe)) {
            frame_deliver(rxframe);
            rx_next++;
          } else {
            net_stat_refused++;
          }
          /* If it does not fit we leave rx_next alone, so the frame is not
           * acked and the peer resends it once the application has drained
           * enough of the ring. That is the backpressure path. */
        }
        /* A sequence we have already seen is a retransmission caused by our ack
         * going missing. Dropping it and re-acking below is the whole recovery.
         */
      }
    }
  }

  /* A fresh frame if the window has room and there is something to say;
   * otherwise go back to the oldest unacknowledged frame and resend from
   * there. Receivers take frames strictly in order, so after a loss
   * everything behind it must come again; a repeat the peer already has costs
   * only the exchange. At a window of 1 this is stop-and-wait exactly: build
   * when nothing is in flight, resend the one frame when something is. While
   * our SYN is unacknowledged the window is 1 — the receiver adopts the first
   * SYN frame it sees, and a second in flight could be adopted instead. */
  flying = (uint8_t)(tx_next - tx_base);
  w = syn_out ? 1 : net_window_now();
  /* With frames in flight and nothing new queued there is nothing to build:
   * a build that came back empty would only be thrown away for the resend
   * below, ~11k cycles of the NMI's at the end of every burst. */
  if (tx_cursor == tx_next && flying < w &&
      (flying == 0 || ((txq_head - txq_tail - txq_out) & (TXQ_SIZE - 1)) != 0 ||
       (ip_mode && ((iptx_head - iptx_tail - iptx_out) & (IP_TXRING - 1)) != 0))) {
    k = (uint8_t)(tx_next & NET_WINDOW_MASK);
    if (build_frame(txwin[k], k)) {
      txq_out = (uint16_t)(txq_out + txwin_ctrl[k]);
      iptx_out = (uint16_t)(iptx_out + txwin_ip[k]);
      tx_next++;
      tx_cursor = tx_next;
      /* We just put fresh data on the wire; poll for the ack rather than
       * waiting a frame for it. Deliberately NOT stamped on the resend path
       * below: against a dead peer the resends would otherwise keep the
       * window open forever, and the foreground would spin the bridge (and
       * grow its files) at full rate until someone restarted the gateway.
       * This way a dead peer stops stamping, the window closes in
       * NET_HOT_FRAMES, and retransmission falls back to the NMI's 60 Hz —
       * exactly the rate it has always run at. */
      net_hot_until = (uint16_t)(snos_frames + NET_HOT_FRAMES);
      link_send(txwin[k]);
      return;
    }
    if (flying == 0) {
      link_send(txwin[k]); /* nothing to say and nothing owed: the ack alone */
      return;
    }
  }
  if (tx_cursor == tx_next) tx_cursor = tx_base;
  /* Resend, with the ack field refreshed so a retransmission also carries our
   * latest receive state. */
  k = (uint8_t)(tx_cursor & NET_WINDOW_MASK);
  tx_cursor++;
  net_stat_resent++;
  txwin[k][LINK_ACK] = (uint8_t)(rx_next - 1);
  txwin[k][LINK_SUM] = 0;
  txwin[k][LINK_SUM] = net_checksum(txwin[k], LINK_FRAME);

  /* Last, and only now: the frame above carries this poll's ack. */
  link_send(txwin[k]);
}

/* ------------------------------------------------- foreground extra polls
 *
 * The NMI's one poll per frame pinned the whole link to 60 polls/s, and a
 * 536-byte TCP segment is several link frames plus ack turnarounds — which
 * is why bulk transfers crawled (PLAN.md: the moving `mc` client outran the
 * emulated link). The bridge was rebuilt around memory callbacks precisely
 * so it answers whenever the console asks, at whatever rate it asks
 * (tools/netbridge.lua); the console just never asked more than 60 times a
 * second. These two let every blocking loop (kbd_idle's frame-boundary spin)
 * ask as often as it likes while a transfer is live.
 *
 * On real hardware this is MORE faithful, not less: the cable is
 * SNES-clocked and a real peer answers in microseconds, so many exchanges
 * per video frame is what M7 will actually do. */

/* Would an extra poll do any good right now? Cheap enough for a spin loop:
 * two 16-bit reads and a compare, no mailbox access, no masking. The hot
 * window is the ONLY trigger, deliberately: every state that means "poll
 * more" (data arriving, our data acked, fresh data sent) stamps the window
 * from inside net_service, and every state that must NOT poll forever — a
 * dead gateway leaving a frame permanently unacked, queued datagrams that
 * can never drain — stops stamping it, so the polls always decay back to
 * the NMI's 60 Hz on their own. Reading these without masking is fine: a
 * stale read mis-answers one iteration of a loop that asks again. */
int net_fg_wanted(void) {
  if (!net_ready) return 0;
  return (int16_t)(net_hot_until - snos_frames) > 0;
}

/* One poll, from the foreground. Masking NMI for the span is the entire
 * safety argument: net_service() was written to be the NMI's and assumes it
 * cannot be preempted, so a foreground call must buy the same guarantee —
 * the same idiom as pad_pressed() and the lifecycle mutations above. The
 * caller is responsible for not doing this during vblank (input.c checks
 * $4212), because an NMI deferred past the top of vblank makes pad_latch
 * burn its guard spin — the exact hazard traps.c documents. */
void net_service_fg(void) {
  NET_NMI_OFF();
  net_service();
  NET_NMI_ON();
}
