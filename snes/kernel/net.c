/* Stream layer: reliability, multiplexing, and the gateway control protocol.
 *
 * ------------------------------------------------------- why reliability
 *
 * Terminating TCP at the gateway has a consequence that is easy to miss: this
 * cable becomes the ONLY unprotected hop, and it carries the entire payload.
 * TCP's guarantees stop at the gateway. A single corrupted bit here is silent
 * data corruption in a download with nothing above it to notice, which is a far
 * worse failure than a dropped connection.
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
 * foreground. They share state, so each shared structure has exactly one
 * writer:
 *
 *   ring[ch] head   NMI writes, foreground reads   (producer)
 *   ring[ch] tail   foreground writes, NMI reads   (consumer)
 *   txq      head   foreground writes, NMI reads
 *   txq      tail   NMI writes, foreground reads
 *
 * Single-producer/single-consumer on each index means no locking is needed and
 * none is used. Adding a second writer to any of these would break it silently.
 */
#include "net.h"
#include "link.h"

/* ---------------------------------------------------- control protocol ---
 * Messages are [type][len][payload...] inside channel 0's byte stream, so a
 * URL longer than one frame reassembles with no special case: the stream layer
 * already puts the bytes back in order. */
#define C2S_FETCH 0x01 /* [len][ch][url...]  channel chosen by the SNES */
#define C2S_CLOSE 0x02 /* [1][ch] */
/* Same shape as FETCH, and the reply is identical: the stream simply contains
 * converted Mode 3 tile data instead of the resource's own bytes. A separate
 * command rather than a flag on FETCH because it is ADDITIVE — FETCH's wire
 * format and its tests are untouched — and because a 200 that will not decode
 * as an image is a genuinely different failure from a 404. */
#define C2S_IMAGE 0x03 /* [len][ch][url...] */

/* Liveness. net_up() only proves the *bridge* claimed the mailbox — the daemon
 * behind it can be dead, stopped or wedged and the link still looks attached,
 * which is exactly what it did while a fetch was timing out. Nothing else in
 * the protocol is unsolicited, so asking is the only way to know. */
#define C2S_HELLO 0x04 /* [0] */
/* [3][ch][status_lo][status_hi] or [4][...][flags].
 *
 * The flags byte is optional so that a three-byte reply still parses: the
 * canned gateways in the tests predate it, and a wire format that refuses to
 * grow is a wire format you cannot change once anything speaks it. */
#define S2C_OPEN 0x81
#define S2C_F_IMAGE 0x01 /* the body is Mode 3 tile data, not the resource */
#define S2C_EOF 0x82   /* [1][ch] */
#define S2C_ERR 0x83   /* [2][ch][code] */
#define S2C_HELLO 0x84 /* [1][version] */

volatile uint16_t net_stat_rx_frames;
volatile uint16_t net_stat_hello;
volatile uint16_t net_gw_version;
volatile uint16_t net_stat_bad_sum;
volatile uint16_t net_stat_resent;

static uint8_t ring[NET_CHANNELS][NET_RING];
static uint16_t ring_head[NET_CHANNELS]; /* NMI writes */
static uint16_t ring_tail[NET_CHANNELS]; /* foreground writes */
static uint8_t ch_state[NET_CHANNELS];
static uint16_t ch_http[NET_CHANNELS];
static uint8_t ch_flags[NET_CHANNELS];
/* Which channels an application opened, so they can be reclaimed when it
 * leaves. Channels are REUSED, so a leak is not merely untidy: the next app
 * handed the same channel inherits whatever the dead one left in its ring, and
 * reads another program's bytes as its own. The break key makes that reachable
 * today, since killing a download mid-transfer leaks by construction. */
static uint8_t ch_app[NET_CHANNELS];

/* Outbound control bytes. Small: the SNES says very little — a URL and the
 * occasional close. */
#define TXQ_SIZE 512
static uint8_t txq[TXQ_SIZE];
static uint16_t txq_head; /* foreground writes */
static uint16_t txq_tail; /* NMI writes */

/* Inbound control reassembly. Sized for a URL-length reply plus slack. */
#define CMSG_MAX 288
static uint8_t cmsg[CMSG_MAX];
static uint16_t cmsg_got;
static uint8_t cmsg_type;
static uint8_t cmsg_len;
static uint8_t cmsg_state; /* 0 want type, 1 want len, 2 collecting */

static uint8_t peer_syn_seen;
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
static uint8_t net_ready;

static void build_frame(void);

static uint8_t checksum(const uint8_t *f) {
  uint16_t s = 0;
  int i;
  for (i = 0; i < LINK_FRAME; i++) {
    if (i != LINK_SUM) s = (uint16_t)(s + f[i]);
  }
  return (uint8_t)(s & 0xFFu);
}

void net_init(void) {
  int i, j;
  for (i = 0; i < NET_CHANNELS; i++) {
    ring_head[i] = 0;
    ring_tail[i] = 0;
    ch_state[i] = NET_ST_FREE;
    ch_http[i] = 0;
    ch_app[i] = 0;
  }
  txq_head = 0;
  txq_tail = 0;
  cmsg_got = 0;
  cmsg_state = 0;
  tx_seq = 0;
  rx_next = 0;
  awaiting_ack = 0;
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
  txframe_bytes = 0;
  net_stat_rx_frames = 0;
  net_stat_hello = 0;
  net_gw_version = 0;
  net_stat_bad_sum = 0;
  net_stat_resent = 0;
  for (j = 0; j < LINK_FRAME; j++) {
    txframe[j] = 0;
    rxframe[j] = 0;
  }
  link_init();
  build_frame();
  net_ready = 1;
}

int net_up(void) { return net_ready && link_present(); }

/* ------------------------------------------------------------- ring I/O */

static uint16_t ring_used(int ch) {
  return (uint16_t)((ring_head[ch] - ring_tail[ch]) & (NET_RING - 1));
}

static uint16_t ring_free(int ch) {
  return (uint16_t)(NET_RING - 1 - ring_used(ch));
}

/* NMI side. Returns 0 if it would not fit — the caller must then NOT ack the
 * frame, which is what turns a full ring into backpressure. */
static int ring_put(int ch, const uint8_t *src, int n) {
  int i;
  if ((int)ring_free(ch) < n) return 0;
  for (i = 0; i < n; i++) {
    ring[ch][ring_head[ch]] = src[i];
    ring_head[ch] = (uint16_t)((ring_head[ch] + 1) & (NET_RING - 1));
  }
  return 1;
}

int net_read(int ch, uint8_t __far *dst, int max) {
  int n = 0;
  if (ch < 0 || ch >= NET_CHANNELS) return 0;
  while (n < max && ring_tail[ch] != ring_head[ch]) {
    dst[n++] = ring[ch][ring_tail[ch]];
    ring_tail[ch] = (uint16_t)((ring_tail[ch] + 1) & (NET_RING - 1));
  }
  return n;
}

int net_state(int ch) {
  if (ch < 0 || ch >= NET_CHANNELS) return NET_ST_ERROR;
  /* Report the stream as open while there is still buffered data, so a caller
   * loops on state and never abandons bytes that already arrived. */
  if (ch_state[ch] == NET_ST_EOF && ring_used(ch) > 0) return NET_ST_OPEN;
  return ch_state[ch];
}

uint16_t net_http_status(int ch) {
  if (ch < 0 || ch >= NET_CHANNELS) return 0;
  return ch_http[ch];
}

/* Did the gateway convert this body to Mode 3 tile data?
 *
 * Only meaningful once the state has left NET_ST_OPENING, because the answer
 * arrives with the status. Callers therefore have to open first and decide the
 * filename afterwards — which is the right order anyway, since the extension
 * depends on what actually came back rather than what was asked for. */
int net_is_image(int ch) {
  if (ch < 0 || ch >= NET_CHANNELS) return 0;
  return (ch_flags[ch] & S2C_F_IMAGE) ? 1 : 0;
}

/* -------------------------------------------------------- outbound control */

static int txq_space(void) {
  return (int)(TXQ_SIZE - 1 - (uint16_t)((txq_head - txq_tail) & (TXQ_SIZE - 1)));
}

static void txq_put(uint8_t b) {
  txq[txq_head] = b;
  txq_head = (uint16_t)((txq_head + 1) & (TXQ_SIZE - 1));
}

static int net_open(const char *url, uint8_t cmd) {
  int ch, n = 0, i;
  if (!net_up()) return -1;
  for (ch = 1; ch < NET_CHANNELS; ch++)
    if (ch_state[ch] == NET_ST_FREE) break;
  if (ch >= NET_CHANNELS) return -1;

  while (url[n] && n < 250) n++;
  if (txq_space() < n + 3) return -1;

  /* The SNES picks the channel rather than asking for one. It costs nothing and
   * it means net_fetch can return a usable handle immediately instead of the
   * caller having to wait a round trip to find out what it got. */
  txq_put(cmd);
  txq_put((uint8_t)(n + 1));
  txq_put((uint8_t)ch);
  for (i = 0; i < n; i++) txq_put((uint8_t)url[i]);

  ring_head[ch] = 0;
  ring_tail[ch] = 0;
  ch_http[ch] = 0;
  ch_state[ch] = NET_ST_OPENING;
  return ch;
}

int net_fetch(const char *url) { return net_open(url, C2S_FETCH); }
int net_image(const char *url) { return net_open(url, C2S_IMAGE); }

/* Called by the trap layer when an app — rather than the shell — opened one. */
void net_own(int ch) {
  if (ch > 0 && ch < NET_CHANNELS) ch_app[(uint8_t)ch] = 1;
}

/* Close whatever the departing application did not. Returns how many, so the
 * caller can say so: silently cleaning up teaches nobody, and a count that is
 * usually zero is a useful thing to notice when it is not. */
int net_reap(void) {
  int i, n = 0;
  for (i = 1; i < NET_CHANNELS; i++) {
    if (ch_app[i]) {
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
int net_hello(void) {
  if (!net_up()) return -1;
  if (txq_space() < 2) return -1;
  txq_put(C2S_HELLO);
  txq_put(0);
  return 0;
}

void net_close(int ch) {
  if (ch < 1 || ch >= NET_CHANNELS) return;
  if (ch_state[ch] != NET_ST_FREE && txq_space() >= 3) {
    txq_put(C2S_CLOSE);
    txq_put(1);
    txq_put((uint8_t)ch);
  }
  ch_state[ch] = NET_ST_FREE;
  ring_head[ch] = 0;
  ring_tail[ch] = 0;
  ch_flags[ch] = 0;
  ch_app[ch] = 0;
}

/* --------------------------------------------------------- inbound control */

static void ctrl_message(void) {
  uint8_t ch;
  switch (cmsg_type) {
  case S2C_OPEN:
    if (cmsg_got >= 3) {
      ch = cmsg[0];
      if (ch < NET_CHANNELS && ch_state[ch] == NET_ST_OPENING) {
        ch_http[ch] = (uint16_t)(cmsg[1] | ((uint16_t)cmsg[2] << 8));
        ch_flags[ch] = (cmsg_got >= 4) ? cmsg[3] : 0;
        ch_state[ch] = NET_ST_OPEN;
      }
    }
    break;
  case S2C_HELLO:
    net_stat_hello++;
    net_gw_version = (cmsg_got >= 1) ? cmsg[0] : 0;
    break;
  case S2C_EOF:
    if (cmsg_got >= 1) {
      ch = cmsg[0];
      if (ch < NET_CHANNELS && ch_state[ch] != NET_ST_FREE)
        ch_state[ch] = NET_ST_EOF;
    }
    break;
  case S2C_ERR:
    if (cmsg_got >= 1) {
      ch = cmsg[0];
      if (ch < NET_CHANNELS) {
        if (cmsg_got >= 2) ch_http[ch] = cmsg[1];
        ch_state[ch] = NET_ST_ERROR;
      }
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
  uint16_t need[NET_CHANNELS];
  int i;
  for (i = 0; i < NET_CHANNELS; i++) need[i] = 0;
  while (o + 2 <= LINK_FRAME) {
    uint8_t ch = f[o];
    uint8_t len = f[o + 1];
    if (ch == LINK_CH_PAD) break;
    if (o + 2 + len > LINK_FRAME) break;
    /* Space is only required for channels somebody might still read.
     *
     * A CLOSED channel's ring never drains — by definition nothing is reading
     * it — so demanding room in it means the frame is never acked, the peer
     * retransmits forever, and the whole link wedges permanently. One aborted
     * transfer would kill networking until reboot. Data for a closed channel is
     * discarded in frame_deliver instead, which is what a closed stream means. */
    if (ch != LINK_CH_CTRL && ch < NET_CHANNELS && ch_state[ch] != NET_ST_FREE)
      need[ch] = (uint16_t)(need[ch] + len);
    o += 2 + len;
  }
  for (i = 1; i < NET_CHANNELS; i++)
    if (need[i] > ring_free(i)) return 0;
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
    /* Silently drop anything addressed to a closed channel: the sender has not
     * yet seen our CLOSE, and there is nowhere for it to go. */
    else if (ch < NET_CHANNELS && ch_state[ch] != NET_ST_FREE)
      ring_put(ch, &f[o + 2], len);
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

  /* Only the control channel has anything outbound today; when OPEN arrives in
   * M8 and applications start writing to streams, this becomes a loop over the
   * channels packing a record from each — which is exactly why the frame format
   * carries records rather than one channel per frame. */
  n = (int)((txq_head - txq_tail) & (TXQ_SIZE - 1));
  if (n > 0) {
    int room = LINK_FRAME - o - 2;
    int i;
    if (n > room) n = room;
    if (n > 255) n = 255;
    txframe[o++] = LINK_CH_CTRL;
    txframe[o++] = (uint8_t)n;
    for (i = 0; i < n; i++) {
      txframe[o++] = txq[(txq_tail + i) & (TXQ_SIZE - 1)];
    }
    txframe[LINK_FLAGS] |= LINK_F_DATA;
  }
  txframe_bytes = (uint16_t)(n > 0 ? n : 0);

  while (o < LINK_FRAME) txframe[o++] = LINK_CH_PAD;
  txframe[LINK_SUM] = 0;
  txframe[LINK_SUM] = checksum(txframe);
}

void net_service(void) {
  if (!net_ready || !net_up()) return;

  if (link_poll(txframe, rxframe)) {
    net_stat_rx_frames++;
    if (rxframe[LINK_SUM] != checksum(rxframe)) {
      /* Corrupt. Do nothing at all — no ack, no state change — and the peer
       * retransmits. Silently ignoring is the correct response; anything
       * cleverer risks acting on bytes we know are wrong. */
      net_stat_bad_sum++;
    } else {
      if (rx_resync) {
        rx_resync = 0;
        rx_next = rxframe[LINK_SEQ];
      }

      /* An ack for the frame in flight releases it. */
      if (awaiting_ack && rxframe[LINK_ACK] == tx_seq) {
        txq_tail = (uint16_t)((txq_tail + txframe_bytes) & (TXQ_SIZE - 1));
        txframe_bytes = 0;
        awaiting_ack = 0;
        tx_seq++;
        syn_out = 0; /* it acked us, so it has our sequence base */
      }
      /* The peer restarted. Adopt its sequence base, once, and fail every
       * channel: whatever sockets those represented died with it, so an app
       * blocked on one gets an error instead of waiting forever. Acting on
       * every SYN frame rather than once per episode would re-deliver
       * retransmissions, because the seq check below is what discards those. */
      if ((rxframe[LINK_FLAGS] & LINK_F_SYN) != 0) {
        if (!peer_syn_seen) {
          int i;
          peer_syn_seen = 1;
          rx_next = rxframe[LINK_SEQ];
          for (i = 1; i < NET_CHANNELS; i++)
            if (ch_state[i] != NET_ST_FREE) ch_state[i] = NET_ST_ERROR;
        }
      } else {
        peer_syn_seen = 0;
      }

      if ((rxframe[LINK_FLAGS] & LINK_F_DATA) != 0) {
        if (rxframe[LINK_SEQ] == rx_next) {
          if (frame_fits(rxframe)) {
            frame_deliver(rxframe);
            rx_next++;
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
    }
  } else {
    /* Still unacknowledged: resend the identical frame, but refresh the ack
     * field so a retransmission also carries our latest receive state. */
    net_stat_resent++;
    txframe[LINK_ACK] = (uint8_t)(rx_next - 1);
    txframe[LINK_SUM] = 0;
    txframe[LINK_SUM] = checksum(txframe);
  }
}
