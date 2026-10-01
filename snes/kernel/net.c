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
 * So: checksum, sequence number, retransmit. Stop-and-wait, which is the
 * simplest thing that is actually correct.
 *
 * WHAT STOP-AND-WAIT COSTS, precisely: the real link is a synchronous exchange
 * — bits shift out while other bits shift in — so a frame's acknowledgement
 * cannot possibly ride in the same exchange that carried it. The round trip is
 * intrinsically two polls, and a sender that will not advance without an ack
 * therefore moves one frame every two polls: ~30/second, ~1.7 KB/s. That is a
 * property of the wire, not of this code.
 *
 * A send window of 2 restores the full ~3.5 KB/s; 4 also absorbs a single lost
 * frame without stalling. The place to add it is tx_seq/awaiting_ack below,
 * which would become a small circular array of pending frames. Deliberately not
 * done yet: correctness first, and the real error rate is unmeasured until
 * there is a real cable to measure it on.
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
 * 512 rather than the 1024 of the receive ring, because the shapes differ: a
 * download arrives in bulk and has to be buffered against an app that reads it
 * slowly, whereas what the SNES sends is a request or a response it generates
 * itself and can simply generate less of when the queue is full. */
#define NET_TXRING 512
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
static uint16_t txframe_ip; /* iptx bytes in the frame in flight */
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

/* First byte beyond the previous frame's records. Everything at and past this
 * offset is already LINK_CH_PAD — net_init establishes it, every build_frame
 * restores it — so padding only has to cover the span the last frame's
 * records dirtied. The old full-width pad loop was ~35 cycles a byte through
 * `sta long`, ~4,200 cycles for the ~120 pad bytes of a receive-mostly frame,
 * i.e. nearly all of build_frame's measured cost, inside NMI, every poll. */
static uint16_t txframe_fill;
static uint8_t peer_syn_seen;
static uint8_t peer_syn_seq;
static uint8_t syn_out;
static uint8_t rx_resync;
static uint8_t txframe[LINK_FRAME];
static uint8_t rxframe[LINK_FRAME];
static uint8_t tx_seq;      /* sequence of the frame currently in flight */
static uint8_t rx_next;     /* the sequence we are waiting to receive */
static uint8_t awaiting_ack;
/* How many control bytes the frame in flight carried. Recomputing this at ack
 * time from the queue's current occupancy would be wrong: the foreground can
 * enqueue more while the frame is in flight, and the queue would then be
 * advanced past bytes that were never sent. */
static uint16_t txframe_bytes;

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

static void build_frame(void);

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

static void session_init(void) {
  int j;
  txq_head = 0;
  txq_tail = 0;
  iprx_head = 0;
  iprx_tail = 0;
  iptx_head = 0;
  iptx_tail = 0;
  txframe_ip = 0;
  ip_mode = 0;
  iprx_resync = 0;
  /* Establish the pad invariant build_frame relies on: everything at and
   * beyond txframe_fill is LINK_CH_PAD. Statics are zeroed, and zero is a
   * valid channel number, not PAD. */
  for (j = 0; j < LINK_FRAME; j++) txframe[j] = LINK_CH_PAD;
  txframe_fill = LINK_HDR;
  cmsg_got = 0;
  cmsg_state = 0;
  tx_seq = 0;
  rx_next = 0;
  awaiting_ack = 0;
  peer_syn_seen = 0;
  peer_syn_seq = 0;
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
  txframe_bytes = 0;
  net_stat_rx_frames = 0;
  net_stat_hello = 0;
  net_text[0] = 0;
  net_gw_version = 0;
  net_stat_bad_sum = 0;
  net_stat_resent = 0;
  net_stat_refused = 0;
  /* rxframe only: txframe was JUST filled with LINK_CH_PAD to establish the
   * pad invariant, and zeroing it here silently defeated that — unused frame
   * bytes went out as 0x00, which both parsers happen to read as empty
   * control records rather than stopping at PAD. Benign by luck, wrong by
   * the invariant this file documents. */
  for (j = 0; j < LINK_FRAME; j++) rxframe[j] = 0;
  net_hot_until = snos_frames;
  build_frame();
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
  txq_put(C2S_IPFWD);
  txq_put(3);
  txq_put((uint8_t)ch);
  txq_put((uint8_t)(port & 0xFF));
  txq_put((uint8_t)(port >> 8));
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
      txq_put(C2S_IPUNFWD);
      txq_put(3);
      txq_put((uint8_t)ch);
      txq_put((uint8_t)(unfwd_port[ch] & 0xFF));
      txq_put((uint8_t)(unfwd_port[ch] >> 8));
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
    txq_put(C2S_HELLO);
    txq_put(0);
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
      txq_put(C2S_IPUNFWD);
      txq_put(3);
      txq_put((uint8_t)ch);
      txq_put((uint8_t)(ch_lport[ch] & 0xFF));
      txq_put((uint8_t)(ch_lport[ch] >> 8));
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
     * which is the forward-compatibility rule both sides have always had. */
    if (ch == LINK_CH_IP) need_ip = (uint16_t)(need_ip + len);
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

static void frame_deliver(const uint8_t *f) {
  int o = LINK_HDR;
  while (o + 2 <= LINK_FRAME) {
    uint8_t ch = f[o];
    uint8_t len = f[o + 1];
    if (ch == LINK_CH_PAD) break;
    if (o + 2 + len > LINK_FRAME) break;
    if (ch == LINK_CH_CTRL) ctrl_bytes(&f[o + 2], len);
    else if (ch == LINK_CH_IP)
      iprx_put(&f[o + 2], len); /* frame_fits reserved the space */
    else if (ch == LINK_CH_KBD) {
      uint8_t i;
      for (i = 0; i < len; i++) kbd_push(f[o + 2 + i]);
    }
    /* Anything else is a record type this kernel no longer speaks: skipped. */
    o += 2 + len;
  }
}

/* Build the next outgoing frame from whatever is queued. */
static void build_frame(void) {
  int o = LINK_HDR;
  int n;

  txframe[LINK_SEQ] = tx_seq;
  txframe[LINK_ACK] = (uint8_t)(rx_next - 1);
  txframe[LINK_FLAGS] = syn_out ? LINK_F_SYN : 0;

  /* Control first, then IP, greedily until the frame is full. Control goes
   * first on purpose: a forward-withdrawal queued behind a datagram backlog
   * would leave the daemon's socket alive after the listener died. */
  n = (int)((txq_head - txq_tail) & (TXQ_SIZE - 1));
  if (n > 0) {
    int room = LINK_FRAME - o - 2;
    uint16_t first;
    if (n > room) n = room;
    if (n > 255) n = 255;
    txframe[o++] = LINK_CH_CTRL;
    txframe[o++] = (uint8_t)n;
    /* Block moves, split at the ring wrap — this runs inside NMI, and the
     * per-byte loop it replaces rebuilt a ring address every iteration at
     * the ~230-cycles/byte cost this file has already measured twice. */
    first = (uint16_t)(TXQ_SIZE - txq_tail);
    if (first > (uint16_t)n) first = (uint16_t)n;
    ring_move(&txframe[o], &txq[txq_tail], first);
    if (first < (uint16_t)n)
      ring_move(&txframe[o + first], &txq[0], (uint16_t)((uint16_t)n - first));
    o += n;
    txframe[LINK_FLAGS] |= LINK_F_DATA;
  }
  txframe_bytes = (uint16_t)(n > 0 ? n : 0);

  /* Then IP — which is everything except control now. The channel txrings
   * never reach this function: uIP consumes them in the foreground and what
   * leaves the wire is the resulting datagrams. */
  {
    int have = (int)((iptx_head - iptx_tail) & (IP_TXRING - 1));
    int room = LINK_FRAME - o - 2;
    uint16_t first;
    txframe_ip = 0;
    if (ip_mode && have > 0 && room > 0) {
      if (have > room) have = room;
      if (have > 255) have = 255;
      txframe[o++] = LINK_CH_IP;
      txframe[o++] = (uint8_t)have;
      /* Same block-move discipline as the control copy above; since the
       * cutover EVERY payload byte on the wire passes through here, so
       * this is the frame builder's whole variable cost. */
      first = (uint16_t)(IP_TXRING - iptx_tail);
      if (first > (uint16_t)have) first = (uint16_t)have;
      ring_move(&txframe[o], &iptx[iptx_tail], first);
      if (first < (uint16_t)have)
        ring_move(&txframe[o + first], &iptx[0],
                  (uint16_t)((uint16_t)have - first));
      o += have;
      txframe_ip = (uint16_t)have;
      txframe[LINK_FLAGS] |= LINK_F_DATA;
    }
  }

  /* Re-pad only what the previous frame's records covered (see txframe_fill);
   * the checksum still sums the whole frame, so a hole in the invariant would
   * fail loudly on the far side rather than corrupt silently. */
  {
    uint16_t end = txframe_fill;
    uint16_t rec_end = (uint16_t)o;
    while (o < (int)end) txframe[o++] = LINK_CH_PAD;
    txframe_fill = rec_end;
  }
  txframe[LINK_SUM] = 0;
  txframe[LINK_SUM] = net_checksum(txframe, LINK_FRAME);
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
  if (!net_ready || !net_up()) return;

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
      }

      /* An ack for the frame in flight releases it. Both sources the frame
       * drew from advance by what THAT frame carried, recorded when it was
       * built — recomputing from current occupancy would be wrong, because
       * the foreground can enqueue more while the frame is in flight. */
      if (awaiting_ack && rxframe[LINK_ACK] == tx_seq) {
        txq_tail = (uint16_t)((txq_tail + txframe_bytes) & (TXQ_SIZE - 1));
        txframe_bytes = 0;
        iptx_tail = (uint16_t)((iptx_tail + txframe_ip) & (IP_TXRING - 1));
        txframe_ip = 0;
        awaiting_ack = 0;
        tx_seq++;
        syn_out = 0; /* it acked us, so it has our sequence base */
        /* The peer just acknowledged us: it is alive and mid-conversation,
         * so keep the foreground polling — the next frame we build (more of
         * this transfer, usually) should not wait for an NMI. */
        net_hot_until = (uint16_t)(snos_frames + NET_HOT_FRAMES);
      }
      /* The peer restarted. A repeated SYN at the same base is a retransmit;
       * a changed base is a fresh restart even when no non-SYN frame arrived
       * between the two (for example, a Pico mode transition). */
      if ((rxframe[LINK_FLAGS] & LINK_F_SYN) != 0) {
        if (!peer_syn_seen || rxframe[LINK_SEQ] != peer_syn_seq) {
          peer_syn_seen = 1;
          peer_syn_seq = rxframe[LINK_SEQ];
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
          iptx_tail = iptx_head;
          txframe_ip = 0;
          iprx_mark = iprx_head;
          iprx_resync = 1;
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

  if (!awaiting_ack) {
    build_frame();
    if ((txframe[LINK_FLAGS] & LINK_F_DATA) != 0) {
      awaiting_ack = 1;
      /* We just put fresh data on the wire; poll for the ack rather than
       * waiting a frame for it. Deliberately NOT stamped on the resend path
       * below: against a dead peer the resends would otherwise keep the
       * window open forever, and the foreground would spin the bridge (and
       * grow its files) at full rate until someone restarted the gateway.
       * This way a dead peer stops stamping, the window closes in
       * NET_HOT_FRAMES, and retransmission falls back to the NMI's 60 Hz —
       * exactly the rate it has always run at. */
      net_hot_until = (uint16_t)(snos_frames + NET_HOT_FRAMES);
    }
  } else {
    /* Still unacknowledged: resend the identical frame, but refresh the ack
     * field so a retransmission also carries our latest receive state. */
    net_stat_resent++;
    txframe[LINK_ACK] = (uint8_t)(rx_next - 1);
    txframe[LINK_SUM] = 0;
    txframe[LINK_SUM] = net_checksum(txframe, LINK_FRAME);
  }

  /* Last, and only now: the frame above carries this poll's ack. */
  link_send(txframe);
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
