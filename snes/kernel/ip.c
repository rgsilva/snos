/* IPv4 on a Super Nintendo (M11): the stack, and what feeds it.
 *
 * This file owns everything between net.c's LINK_CH_IP byte stream and uIP:
 * datagram framing, the service loop, the timers, and the two application
 * callbacks through which uIP and SNOS's channels exchange bytes.
 *
 * ------------------------------------------------------------ the framing
 *
 * The link channel is a reliable, ordered BYTE stream (net.c's seq/ack layer
 * guarantees that), so datagrams need only a length prefix: two bytes,
 * little-endian, then exactly that many bytes of IPv4 packet. Little-endian
 * because this prefix is link-local bookkeeping between us and the daemon —
 * it is not part of any packet, so network byte order has no claim on it.
 *
 * A broken prefix is unrecoverable by inspection (nothing marks a datagram
 * boundary), which is why net_ip_read reports a peer restart explicitly and
 * why a nonsense length drops the whole buffered stream rather than trying to
 * be clever: one desync costs at most the ring's contents, and the daemon's
 * packets after it parse cleanly again.
 *
 * Reassembly uses its own buffer rather than uip_buf, and the copy at the end
 * is one MVN. uIP's single buffer is also its OUTPUT buffer, so a periodic
 * timer firing while a datagram was half-assembled would overwrite the half
 * — and gating the timers on "no datagram in flight" makes the stack's clock
 * depend on the peer's, which is how a stalled sender turns into a stalled
 * retransmission timer.
 *
 * ----------------------------------------------------------- concurrency
 *
 * ip_service runs in the FOREGROUND, from kbd_idle — never in NMI. With M10
 * both processes yield through kbd_idle, so two processes can race into it;
 * a test-and-set under NMI-off makes the second one skip a turn. The NMI's
 * only involvement is moving link bytes, on the other side of net.c's
 * single-writer rings.
 *
 * ------------------------------------------------------------------ ICMP
 *
 * uIP answers echo requests itself (address swap, type 0, checksum fixup), so
 * there is no responder here. It has no way to ORIGINATE one, though, so
 * `ping` composes its request longhand below and intercepts the replies
 * before uip_input sees them — uIP drops an echo reply as an unsupported
 * type, which would make the console's own ping mysteriously never answer.
 */
#include "ip.h"
#include <string.h>
#include "net.h"
#include "snos.h"
#include "link.h" /* link_mb_move: one MVN within bank $7E */
#include "proc.h"
#include "uip.h"

/* uIP's own send buffer, inside uip_buf. It is a global with external
 * linkage that uip.h happens not to declare, and uip_send() is written to
 * support exactly this use — `if (data != uip_sappdata) memcpy(...)` — so
 * assembling a segment directly here skips a 536-byte copy that would cost
 * ~50,000 cycles at -O0 on every single segment. */
extern void *uip_sappdata;

volatile uint16_t ip_ping_replies;
volatile uint16_t ip_ping_rtt;
volatile uint16_t ip_ping_seq;
volatile uint16_t ip_stat_rx;
volatile uint16_t ip_stat_tx;
volatile uint16_t ip_stat_drop;
volatile uint16_t ip_stat_desync;

/* Ping bookkeeping. The id is fixed and ours; the daemon's NAT restores it on
 * the way back (Linux rewrites the field, macOS does not), so a reply is
 * matched by id alone and the seq and timestamp ride in the payload, which
 * RFC 792 requires the far end to echo byte-for-byte. */
#define PING_ID_HI 0x53u /* 'S' */
#define PING_ID_LO 0x4Eu /* 'N' */
static uint16_t ping_next_seq;

static uint8_t pkt[IP_MTU]; /* datagram reassembly; see the header comment */
static uint16_t asm_len;    /* bytes of the length prefix seen (0..2) */
static uint16_t asm_need;   /* datagram length once the prefix is whole */
static uint16_t asm_got;    /* payload bytes collected so far */

static uint8_t ip_busy;     /* two processes may race into ip_service */
static uint8_t ip_started;  /* uip_init has run */

/* HELLO pacing. The gateway only states its version when asked, nothing on
 * this link is unsolicited, and the bridge can claim the mailbox minutes
 * after boot — so ask, at boot and then patiently, until an answer names a
 * version. One 2-byte record every ~2 s is the cost of autoconfiguration. */
static uint16_t hello_last;

/* uIP's periodic timer. Every connection gets a tick every half second: that
 * is uIP's retransmission clock, its connection timeout, and the only thing
 * that ever calls the application with uip_poll(). 30 frames is that half
 * second on NTSC, and the counter is compared with the wrapping-safe idiom
 * used everywhere else in this kernel. */
#define IP_TICK_FRAMES 30u
static uint16_t tick_last;

#define R8(a) (*(volatile uint8_t *)(a))

/* ------------------------------------------------------------- datagram IO */

/* Queue one datagram toward the daemon: prefix, then packet. All or nothing —
 * net_ip_write refuses partial writes by contract, so a full ring costs the
 * datagram (IP is allowed to drop; TCP will resend) and never poisons the
 * stream with half a length-prefixed record. */
static void ip_output(const uint8_t *p, uint16_t n) {
  uint8_t pre[2];
  /* net_ip_wspace does not consider ip_mode, but net_ip_write refuses on it
   * PER CALL — so a mode that flips between the two writes below would put a
   * bare packet on the stream with no length prefix, and the daemon would
   * read its version byte as a length. Check once, here. */
  if (!net_ipmode() || n == 0 || (int)n + 2 > net_ip_wspace()) {
    ip_stat_drop++;
    return;
  }
  pre[0] = (uint8_t)n;
  pre[1] = (uint8_t)(n >> 8);
  /* ONE record, one publishing store: two writes could be torn apart by the
   * NMI's peer-restart discard, putting a bare packet on the stream whose
   * first two bytes the daemon would read as a length. */
  net_ip_write2(pre, 2, p, (int)n);
  ip_stat_tx++;
}

/* uIP left a packet in uip_buf: put it on the wire. */
static void ip_send_uip(void) {
  if (uip_len > 0) {
    ip_output(uip_buf, uip_len);
    uip_len = 0;
  }
}

int ip_up(void) { return net_up() && net_ipmode(); }

/* ------------------------------------------------------------------- ping */

static uint16_t csum_add(uint16_t a, uint16_t b) {
  uint32_t t = (uint32_t)a + (uint32_t)b;
  t = (t & 0xFFFFu) + (t >> 16);
  t = (t & 0xFFFFu) + (t >> 16);
  return (uint16_t)t;
}

/* kernel/netasm.s: ones-complement sum, uncomplemented, n EVEN. */
extern uint16_t ip_sum(const uint8_t *p, uint16_t n);

static uint16_t csum_range(const uint8_t *p, uint16_t n) {
  uint16_t s = ip_sum(p, (uint16_t)(n & ~1u));
  if (n & 1u) s = csum_add(s, (uint16_t)((uint16_t)p[n - 1] << 8));
  return s;
}

static void put16(uint8_t *p, uint16_t v) { /* network byte order */
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* uIP's checksums (UIP_ARCH_CHKSUM in uip-conf.h), over ip_sum.
 *
 * uip.c's own are C at -O0: ~494 master cycles a byte, 838k to take in one
 * 1500-byte datagram -- three frames of CPU on a console whose NMI already
 * spends most of each frame on the link (docs/link-throughput.md). ip_sum
 * is ~45 a byte. The arithmetic is uip.c's exactly: a ones-complement sum
 * chained from the pseudo-header, 0 reported as 0xffff, and neither version
 * ever produces 0 from data that is not all zero, so the two agree on every
 * packet, not just on valid ones. */
u16_t uip_chksum(u16_t *data, u16_t len) {
  uint16_t s = csum_range((const uint8_t *)data, len);
  return HTONS(s);
}

u16_t uip_ipchksum(void) {
  uint16_t s = csum_range(&uip_buf[UIP_LLH_LEN], UIP_IPH_LEN);
  return s == 0 ? 0xffffu : HTONS(s);
}

static u16_t upper_chksum(uint8_t proto) {
  const uint8_t *ip = &uip_buf[UIP_LLH_LEN];
  uint16_t len = (uint16_t)(get16(&ip[2]) - UIP_IPH_LEN);
  uint16_t s = csum_add((uint16_t)(len + proto), csum_range(&ip[12], 8));
  s = csum_add(s, csum_range(&ip[UIP_IPH_LEN], len));
  return s == 0 ? 0xffffu : HTONS(s);
}

u16_t uip_tcpchksum(void) { return upper_chksum(UIP_PROTO_TCP); }
u16_t uip_udpchksum(void) { return upper_chksum(UIP_PROTO_UDP); }

int ip_ping_send(const uint8_t dst[4]) {
  uint8_t q[36]; /* 20 IP + 8 ICMP + 8 payload */
  uint16_t s;
  if (!ip_up()) return -1;
  q[0] = 0x45; /* IPv4, 20-byte header */
  q[1] = 0;
  put16(&q[2], 36);
  put16(&q[4], ping_next_seq); /* datagram id: the seq is unique enough */
  put16(&q[6], 0x4000);        /* DF: this stack never fragments */
  q[8] = 64;                   /* TTL */
  q[9] = 1;                    /* ICMP */
  q[10] = 0;
  q[11] = 0;
  q[12] = IP_SELF0; q[13] = IP_SELF1; q[14] = IP_SELF2; q[15] = IP_SELF3;
  q[16] = dst[0]; q[17] = dst[1]; q[18] = dst[2]; q[19] = dst[3];
  put16(&q[10], (uint16_t)~csum_range(q, 20));
  q[20] = 8; /* echo request */
  q[21] = 0;
  q[22] = 0;
  q[23] = 0;
  q[24] = PING_ID_HI;
  q[25] = PING_ID_LO;
  put16(&q[26], ping_next_seq);
  /* Payload: the frame stamp the reply must echo, plus a signature, so a
   * mangled echo shows up as a wrong RTT rather than as nothing at all. */
  put16(&q[28], snos_frames);
  q[30] = 'S'; q[31] = 'N'; q[32] = 'O'; q[33] = 'S';
  q[34] = 0;
  q[35] = 0;
  put16(&q[22], (uint16_t)~csum_range(&q[20], 16));
  ping_next_seq++;
  ip_output(q, 36);
  return 0;
}

/* An echo REPLY to one of ours? uIP would drop it as an unsupported ICMP
 * type, so it is taken here first. Returns 1 if this datagram was consumed. */
static int ping_intercept(const uint8_t *p, uint16_t n) {
  uint16_t ihl, total;
  const uint8_t *ic;
  uint16_t icn;
  if (n < 20 || (p[0] >> 4) != 4 || p[9] != 1) return 0;
  ihl = (uint16_t)((p[0] & 0x0Fu) << 2);
  total = get16(&p[2]);
  if (ihl < 20 || total < ihl || total > n) return 0;
  ic = p + ihl;
  icn = (uint16_t)(total - ihl);
  if (icn < 10 || ic[0] != 0) return 0;
  if (ic[4] != PING_ID_HI || ic[5] != PING_ID_LO) return 0;
  if (csum_range(ic, icn) != 0xFFFFu) {
    ip_stat_drop++;
    return 1;
  }
  ip_ping_seq = get16(&ic[6]);
  ip_ping_rtt = (uint16_t)(snos_frames - get16(&ic[8]));
  ip_ping_replies++;
  return 1;
}

/* ------------------------------------------------------ uIP application ---
 *
 * Nothing above the kernel calls these: uIP calls them, once per event, with
 * uip_conn naming the connection. They are the whole bridge between the
 * stack's world (one buffer, one segment, re-supply on retransmission) and
 * SNOS's (a channel with two rings, drained by an application whenever it
 * gets round to it).
 */
/* Both are defined with the connection table further down, but the appcall
 * above needs them: it is the first thing that asks "which channel was
 * listening on this port, and is it free?". Without these C would infer
 * non-static declarations and the definitions would then conflict. */
static int listen_ch_for(uint16_t netport);
static struct uip_conn *conn_for(int ch);

/* Move as much of the channel's send queue into this segment as uIP will
 * take. The bytes go STRAIGHT into uIP's own send buffer: uip_send memcpys
 * unless the pointer it is given is already uip_sappdata, so writing there
 * turns a 536-byte copy at ~100 cycles a byte into one MVN at ~7.
 *
 * The queue is NOT consumed. uIP asks for the same bytes again on a
 * retransmission, and an acknowledgement is the only thing that makes them
 * safe to forget. */
static void tcp_fill(uint8_t ch, uip_tcp_appstate_t *s, int max) {
  int n;
  /* Clamp to THIS connection's MSS, not to the compile-time maximum.
   * uip_process re-clamps uip_slen to uip_conn->mss and records that as the
   * segment length — and mss is recomputed from the peer's advertised window
   * on every segment. Offering 400 bytes to a peer advertising 256 sends 256
   * but recorded sent=400, so the ACK advanced the ring by 400 and 144 bytes
   * were skipped and never transmitted. Silent stream corruption, not a
   * stall, which is the worst kind. */
  if (max > (int)uip_mss()) max = (int)uip_mss();
  if (max > UIP_TCP_MSS) max = UIP_TCP_MSS;
  if (max <= 0) return;
  n = net_stack_txcopy(ch, (uint8_t *)uip_sappdata, max);
  if (n > 0) {
    s->sent = (uint16_t)n;
    uip_send(uip_sappdata, n);
  }
}

void snos_tcp_appcall(void) {
  uip_tcp_appstate_t *s = &uip_conn->appstate;
  uint8_t ch;

  /* An INBOUND connection: uIP accepted it against a port ip_listen bound,
   * and the connection carries no channel yet. Outbound connections are
   * bound before their SYN leaves, so an unbound one here can only have come
   * from a listen — and every appstate is stamped 0xFF at init and again
   * when a connection ends, so a recycled slot cannot be mistaken for one. */
  if (uip_connected() && s->ch == 0xFFu) {
    int lch = listen_ch_for(uip_conn->lport);
    if (lch < 0) {
      uip_abort(); /* nobody is listening there any more */
      return;
    }
    s->ch = (uint8_t)lch;
    s->sent = 0;
    s->closing = 0;
  }
  ch = s->ch;

  if (ch == 0xFFu) {
    /* closing == 2 means a graceful close is already under way: the FIN has
     * been issued and the channel deliberately detached, so uIP is simply
     * finishing. Aborting here would turn that polite close into a RST — and
     * a RST lets the client discard response bytes it has not read yet. */
    if (s->closing == 2) return;
    /* A true orphan: the application closed its channel while the connection
     * was live, so there is nowhere to put anything. */
    uip_abort();
    return;
  }

  if (uip_connected()) {
    net_stack_setstate(ch, NET_ST_OPEN);
    s->sent = 0;
  }

  /* Death, in the three shapes uIP reports it. The channel keeps its state
   * for the application to read; only the binding is dropped, so a later
   * event on this connection cannot reach a channel that has since been
   * reused by somebody else.
   *
   * UNLESS a hangup is under way (closing != 0): net_hangup has already
   * reset the rings and put the channel back in LISTEN, so writing EOF or
   * ERROR onto it would wedge the listener — and rxput below would hand
   * the DEAD conversation's tail bytes to the NEXT client that binds the
   * channel. Mid-close, only the binding is dropped; the channel is no
   * longer this connection's to describe. */
  if (uip_aborted() || uip_timedout()) {
    if (s->closing == 0) net_stack_setstate(ch, NET_ST_ERROR);
    s->ch = 0xFFu;
    return;
  }
  if (uip_closed()) {
    if (s->closing == 0) net_stack_setstate(ch, NET_ST_EOF);
    s->ch = 0xFFu;
    return;
  }

  /* Inbound first, and before anything writes to uip_sappdata: on a data
   * segment the received bytes and the send buffer are the same memory. */
  if (uip_newdata() && uip_datalen() > 0 && s->closing == 0) {
    if (!net_stack_rxput(ch, (const uint8_t *)uip_appdata, (int)uip_datalen())) {
      /* Should not happen — the window below is closed a segment in advance
       * precisely so it cannot. If it ever does, these bytes are LOST: uIP
       * advanced rcv_nxt before calling us, so the ack already claims them
       * and the peer will never resend. Stopping now at least prevents the
       * next one. */
      uip_stop();
    }
  }

  if (uip_acked()) {
    net_stack_txdrop(ch, (int)s->sent);
    s->sent = 0;
  }

  /* A retransmission must carry EXACTLY the bytes the lost segment did. They
   * are still at the tail of the ring precisely because nothing acknowledged
   * them. */
  if (uip_rexmit()) {
    if (s->sent > 0) {
      int n = net_stack_txcopy(ch, (uint8_t *)uip_sappdata, (int)s->sent);
      if (n > 0) uip_send(uip_sappdata, n);
    }
    return;
  }

  if (uip_connected() || uip_newdata() || uip_acked() || uip_poll()) {
    if (s->sent == 0) {
      int have = net_stack_txused(ch);
      if (have > 0) {
        tcp_fill(ch, s, have);
      } else if (s->closing == 1) {
        /* Everything queued has been acknowledged, so the FIN cannot
         * overtake data the far end still needs. The channel is released
         * here rather than when uIP finally reports the close, because it
         * has already gone back to LISTEN for the next connection — letting
         * a later event write EOF onto it would break the server loop. */
        uip_close();
        s->closing = 2;
        s->ch = 0xFFu;
      }
    }
  }

  /* THE WINDOW MUST CLOSE ONE SEGMENT EARLY.
   *
   * uIP advances rcv_nxt and acks BEFORE calling the application, so by the
   * time a ring is discovered full the data is already acknowledged and the
   * peer will never send it again — dropping it makes a hole in the stream
   * rather than backpressure. Advertising zero while there is still a whole
   * segment of room means the peer stops before sending anything we cannot
   * hold, which is the real thing. */
  if (uip_stopped(uip_conn)) {
    if (net_stack_rxfree(ch) >= UIP_TCP_MSS) uip_restart();
  } else if (net_stack_rxfree(ch) < UIP_TCP_MSS) {
    uip_stop();
  }
}

/* ------------------------------------------------------------------- DNS */

#define RES_NAME_MAX 64
static char res_name[RES_NAME_MAX + 1];
static uint8_t res_query[RES_NAME_MAX + 24];
static uint16_t res_qlen;
static uint16_t res_id;
static uint8_t res_state;
static uint8_t res_addr[4];
static uint8_t res_tries;
/* Which lookup the slot currently holds. Handed to the caller by ip_resolve
 * and demanded back by every query: a caller with yesterday's tag sees FAIL,
 * never the answer to somebody else's question. Never 0, so a zeroed caller
 * variable cannot accidentally match. */
static uint8_t res_tag;
static struct uip_udp_conn *res_conn;
/* Set by ip_resolve, consumed by ip_service: the query is BUILT by the
 * caller and SENT by the service pass, because nothing outside ip_service
 * may run uIP. One frame of latency instead of the half second a periodic
 * tick would cost. */
static uint8_t res_kick;
/* When the current lookup started. uIP has no UDP timer of its own and the
 * retry count only advances on polls, so a lookup whose replies simply stop
 * needs a wall-clock bound as well or it pins the single resolver. */
static uint16_t res_started;

/* Encode "www.example.com" as 3www7example3com0, then QTYPE=A, QCLASS=IN.
 * Returns 0 on success. */
static int res_build(const char *name) {
  uint16_t o = 12, lab, i = 0;
  res_query[0] = (uint8_t)(res_id >> 8);
  res_query[1] = (uint8_t)res_id;
  res_query[2] = 0x01; /* recursion desired — the daemon's resolver does the
                        * walking; a 65816 chasing referrals is not a fight
                        * worth having */
  res_query[3] = 0x00;
  res_query[4] = 0; res_query[5] = 1;  /* one question */
  res_query[6] = 0; res_query[7] = 0;
  res_query[8] = 0; res_query[9] = 0;
  res_query[10] = 0; res_query[11] = 0;
  for (;;) {
    lab = o++;
    res_query[lab] = 0;
    while (name[i] && name[i] != '.') {
      if (res_query[lab] >= 63 || o >= sizeof res_query - 6) return -1;
      res_query[o++] = (uint8_t)name[i++];
      res_query[lab]++;
    }
    if (res_query[lab] == 0) return -1; /* empty label: ".." or a trailing dot */
    if (name[i] == 0) break;
    i++; /* the dot */
  }
  res_query[o++] = 0;      /* root label */
  res_query[o++] = 0; res_query[o++] = 1; /* QTYPE A */
  res_query[o++] = 0; res_query[o++] = 1; /* QCLASS IN */
  res_qlen = o;
  return 0;
}

/* Walk a DNS name at `o`, returning the offset just past it. Compression
 * pointers end a name, so this never follows one — the only names we skip
 * over are in records we are not reading. */
static uint16_t res_skip_name(const uint8_t *p, uint16_t o, uint16_t n) {
  while (o < n) {
    uint8_t len = p[o];
    if (len == 0) return (uint16_t)(o + 1);
    if ((len & 0xC0u) == 0xC0u) return (uint16_t)(o + 2);
    o = (uint16_t)(o + 1u + len);
  }
  return n;
}

static void res_parse(const uint8_t *p, uint16_t n) {
  uint16_t qd, an, o, i;
  if (n < 12) return;
  if (((uint16_t)p[0] << 8 | p[1]) != res_id) return; /* not our question */
  if ((p[3] & 0x0Fu) != 0) {                          /* RCODE: NXDOMAIN etc */
    res_state = IP_RES_FAIL;
    return;
  }
  qd = (uint16_t)((uint16_t)p[4] << 8 | p[5]);
  an = (uint16_t)((uint16_t)p[6] << 8 | p[7]);
  o = 12;
  for (i = 0; i < qd; i++) {
    o = res_skip_name(p, o, n);
    o = (uint16_t)(o + 4); /* QTYPE + QCLASS */
  }
  for (i = 0; i < an && o + 10 <= n; i++) {
    uint16_t type, rdlen;
    o = res_skip_name(p, o, n);
    if (o + 10 > n) break;
    type = (uint16_t)((uint16_t)p[o] << 8 | p[o + 1]);
    rdlen = (uint16_t)((uint16_t)p[o + 8] << 8 | p[o + 9]);
    o = (uint16_t)(o + 10);
    if (o + rdlen > n) break;
    /* The first A record wins, and everything else is stepped over — which
     * is how a CNAME chain resolves without any chain-following code: the
     * server that followed it already put the A record in the same answer. */
    if (type == 1 && rdlen == 4) {
      res_addr[0] = p[o];
      res_addr[1] = p[o + 1];
      res_addr[2] = p[o + 2];
      res_addr[3] = p[o + 3];
      res_state = IP_RES_OK;
      return;
    }
    o = (uint16_t)(o + rdlen);
  }
  res_state = IP_RES_FAIL; /* answered, but with no address in it */
}

void snos_udp_appcall(void) {
  if (uip_udp_conn->appstate.kind != 1) return;
  if (uip_newdata()) {
    res_parse((const uint8_t *)uip_appdata, uip_datalen());
    return;
  }
  /* Poll: (re)send the question. uIP has no UDP retransmission of its own —
   * the datagram is sent when the application says so and forgotten — so the
   * retry count lives here, and this is also the ONLY path by which the
   * first query goes out. */
  if (uip_poll() && res_state == IP_RES_BUSY) {
    if (res_tries == 0) {
      res_state = IP_RES_FAIL;
      return;
    }
    res_tries--;
    uip_send(res_query, (int)res_qlen);
  }
}

int ip_resolve(const char *name) {
  int i;
  if (!ip_up()) return -1;
  /* Only BUSY refuses. A finished answer nobody has read yet may be
   * displaced — the displaced asker's tag goes stale and it reads FAIL,
   * which is the honest outcome of losing the slot, where the old scheme
   * silently handed it the new lookup's address. Holding the slot until an
   * explicit release was tried and reverted: one forgotten release locked
   * the resolver for the life of the boot. */
  if (res_state == IP_RES_BUSY) return -1;
  for (i = 0; i < RES_NAME_MAX && name[i]; i++) res_name[i] = name[i];
  if (i == 0 || (i == RES_NAME_MAX && name[i])) return -1;
  res_name[i] = 0;
  res_id++;
  if (res_build(res_name) < 0) return -1;
  res_tag = (uint8_t)(res_tag + 1);
  if (res_tag == 0) res_tag = 1;
  res_state = IP_RES_BUSY;
  res_tries = 4; /* ~2 s of patience, at one try per periodic tick */
  res_started = snos_frames;
  res_kick = 1;  /* ip_service sends it on its next pass, about a frame away */
  return (int)res_tag;
}

/* The asker is done with the answer — or has given up waiting. Only the
 * lookup's own tag releases it; anyone else's slot is left alone. */
void ip_resolve_release(uint8_t tag) {
  /* BUSY counts too. A caller that gave up waiting (host_addr's deadline)
   * still owns the lookup, and leaving it BUSY made the resolver refuse
   * every request afterwards for the life of the boot — one slow answer and
   * the machine could never resolve another name. */
  if (tag == res_tag) res_state = IP_RES_IDLE;
}

/* Create the resolver's socket and push the query out. ip_service only. */
static void res_service(void) {
  uip_ipaddr_t a;
  if (!res_kick) return;
  res_kick = 0;
  if (res_conn == 0) {
    uip_ipaddr(a, IP_SELF0, IP_SELF1, IP_SELF2, IP_DNS3);
    res_conn = uip_udp_new(&a, HTONS(53));
    if (res_conn == 0) {
      res_state = IP_RES_FAIL;
      return;
    }
    res_conn->appstate.kind = 1;
  }
  /* uip_udp_periodic_conn runs the appcall with uip_poll() set, which is the
   * only event a UDP connection ever gets. */
  uip_udp_periodic_conn(res_conn);
  ip_send_uip();
}

int ip_resolve_state(uint8_t tag) {
  if (tag != res_tag) return IP_RES_FAIL; /* superseded: the slot moved on */
  return res_state;
}

int ip_resolve_slot(void) { return res_state; }

void ip_resolve_addr(uint8_t tag, uint8_t out[4]) {
  int i;
  if (tag != res_tag) return;
  for (i = 0; i < 4; i++) out[i] = res_addr[i];
}

void ip_uipstat(ip_uipstat_t *o) {
#if UIP_STATISTICS
  o->ip_recv = uip_stat.ip.recv;
  o->ip_sent = uip_stat.ip.sent;
  o->ip_drop = uip_stat.ip.drop;
  o->ip_chkerr = uip_stat.ip.chkerr;
  o->icmp_recv = uip_stat.icmp.recv;
  o->icmp_sent = uip_stat.icmp.sent;
  o->icmp_typeerr = uip_stat.icmp.typeerr;
  o->tcp_recv = uip_stat.tcp.recv;
  o->tcp_drop = uip_stat.tcp.drop;
  o->tcp_chkerr = uip_stat.tcp.chkerr;
  o->tcp_rexmit = uip_stat.tcp.rexmit;
  o->tcp_rst = uip_stat.tcp.rst;
  o->tcp_syndrop = uip_stat.tcp.syndrop;
  o->udp_drop = uip_stat.udp.drop;
  o->udp_chkerr = uip_stat.udp.chkerr;
#else
  uint16_t *p = (uint16_t *)o;
  int i;
  for (i = 0; i < (int)(sizeof *o / 2); i++) p[i] = 0;
#endif
}

/* ------------------------------------------------------- connect / close */

/* A connection that cannot be made until a name is resolved. One slot per
 * channel rather than one queue: the resolver serialises the lookups anyway,
 * and a per-channel slot means a slow lookup for one application cannot lose
 * another application's request. */
#define PEND_HOST_MAX 48
static struct {
  char host[PEND_HOST_MAX + 1];
  uint8_t addr[4];
  uint16_t port;
  uint8_t busy;      /* a connect is waiting on this channel */
  uint8_t resolving; /* ...and its name is with the resolver right now */
  uint8_t have_addr; /* the address is known; no lookup needed */
  uint8_t tag;       /* which lookup is ours, while resolving */
} pend[NET_CHANNELS];

/* Ports this console is listening on, per channel. uIP's own listen list is
 * a set of PORTS with no room for a channel number, so an inbound connection
 * arrives knowing only which port it landed on — this table is what turns
 * that back into the channel an application is waiting on. */
static uint16_t lport[NET_CHANNELS];

/* Which channel is listening on this port, and is it free to take a
 * connection? uIP will accept a second SYN on a bound port perfectly happily,
 * and binding it to the same channel would interleave two clients' bytes in
 * both rings. The gateway-backed path was safe only because the daemon
 * serialised accepts; nothing serialises them here, so the second is
 * refused. */
static int listen_ch_for(uint16_t netport) {
  int i;
  for (i = 1; i < NET_CHANNELS; i++) {
    if (lport[i] == 0 || HTONS(lport[i]) != netport) continue;
    /* Busy serving somebody: keep looking — since mc, one port may be
     * bound by SEVERAL channels precisely so a second connection lands on
     * the next free one (the ping/login overlap). Returning -1 at the
     * first busy match made the second listener unreachable. */
    if (conn_for(i) != 0) continue;
    if (net_state(i) != NET_ST_LISTEN) continue; /* mid-teardown: not ready */
    return i;
  }
  return -1;
}

static struct uip_conn *conn_for(int ch) {
  int i;
  for (i = 0; i < UIP_CONNS; i++)
    if (uip_conns[i].tcpstateflags != UIP_CLOSED &&
        uip_conns[i].appstate.ch == (uint8_t)ch)
      return &uip_conns[i];
  return 0;
}

static int start_connect(int ch, const uint8_t a[4], uint16_t port) {
  uip_ipaddr_t ip;
  struct uip_conn *c;
  uip_ipaddr(ip, a[0], a[1], a[2], a[3]);
  c = uip_connect(&ip, HTONS(port));
  if (c == 0) return -1;
  c->appstate.ch = (uint8_t)ch;
  c->appstate.sent = 0;
  c->appstate.closing = 0;
  /* Send the SYN NOW. uip_connect only creates the connection: the segment
   * itself goes out of the SYN_SENT arm of the periodic timer, so without
   * this every connection on this machine would begin with up to half a
   * second of nothing — and uIP would count its own first SYN as a
   * retransmission, which is how this was noticed. */
  uip_periodic_conn(c);
  ip_send_uip();
  return 0;
}

/* Split "host:port". Returns the port, or 0 when there is none — every
 * caller of this ABI has always been required to give one. */
static uint16_t split_hostport(const char *s, char *host, int cap) {
  int i = 0;
  uint16_t port = 0;
  int any = 0;
  while (s[i] && s[i] != ':' && i < cap - 1) {
    host[i] = s[i];
    i++;
  }
  host[i] = 0;
  if (s[i] != ':') return 0;
  i++;
  while (s[i] >= '0' && s[i] <= '9') {
    port = (uint16_t)(port * 10u + (uint16_t)(s[i] - '0'));
    i++;
    any = 1;
  }
  return any ? port : 0;
}

static int parse_dotted(const char *s, uint8_t out[4]) {
  int i, k = 0;
  for (i = 0; i < 4; i++) {
    uint16_t v = 0;
    int any = 0;
    while (s[k] >= '0' && s[k] <= '9') {
      v = (uint16_t)(v * 10u + (uint16_t)(s[k] - '0'));
      if (v > 255) return -1;
      k++;
      any = 1;
    }
    if (!any) return -1;
    out[i] = (uint8_t)v;
    if (i < 3) {
      if (s[k] != '.') return -1;
      k++;
    }
  }
  return s[k] == 0 ? 0 : -1;
}

int ip_connect(const char *hostport) {
  char host[PEND_HOST_MAX + 1];
  uint8_t a[4];
  uint16_t port;
  int ch, i;
  if (!ip_up()) return -1;
  port = split_hostport(hostport, host, (int)sizeof host);
  if (port == 0) return -1;
  ch = net_stack_claim();
  if (ch < 0) return -1;

  /* Recorded, not performed — see the comment at the top of ip_service for
   * why nothing outside that function may call into uIP. The channel stays
   * OPENING until it happens, which is a state applications already wait in
   * whether the name needed resolving or not. */
  pend[ch].port = port;
  pend[ch].resolving = 0;
  if (parse_dotted(host, a) == 0) {
    pend[ch].addr[0] = a[0];
    pend[ch].addr[1] = a[1];
    pend[ch].addr[2] = a[2];
    pend[ch].addr[3] = a[3];
    pend[ch].have_addr = 1;
    pend[ch].host[0] = 0;
  } else {
    for (i = 0; i < PEND_HOST_MAX && host[i]; i++) pend[ch].host[i] = host[i];
    pend[ch].host[i] = 0;
    pend[ch].have_addr = 0;
  }
  pend[ch].busy = 1;
  return ch;
}

void ip_close(int ch) {
  struct uip_conn *c;
  if (ch <= 0 || ch >= NET_CHANNELS) return;
  c = conn_for(ch);
  pend[ch].busy = 0;
  if (lport[ch] != 0) {
    uip_unlisten(HTONS(lport[ch]));
    lport[ch] = 0;
  }
  if (c == 0) return;
  /* Unbind, and leave the teardown to ip_service. Two reasons, both
   * load-bearing: uip_abort() is a FLAG uip_process consumes when an appcall
   * returns, so calling it from here would emit nothing at all; and this runs
   * from a syscall, which must never touch uIP (see ip_service). The poll
   * loop there picks up orphaned connections and aborts them from inside
   * uip_process, where it becomes a RST on the wire. Unbinding first is also
   * what stops that appcall from writing into rings net_close is clearing. */
  c->appstate.ch = 0xFFu;
  c->appstate.closing = 0; /* a stale "graceful close" would suppress the abort */
}

/* Advance whatever is waiting on a name. Called once per service pass. */
static void pend_service(void) {
  int i;
  /* The resolver is one slot, so only LOOKUPS serialise. Scanning used to
   * `return` the moment one was in flight, which also stranded every
   * higher-numbered channel whose address needed no lookup at all — a
   * dotted-quad connect waited out somebody else's six-second DNS timeout
   * for no reason. */
  int resolver_taken = 0;
  for (i = 1; i < NET_CHANNELS; i++) {
    if (!pend[i].busy) continue;
    if (pend[i].have_addr) {
      pend[i].busy = 0;
      pend[i].have_addr = 0;
      if (start_connect(i, pend[i].addr, pend[i].port) < 0)
        net_stack_setstate(i, NET_ST_ERROR);
      continue;
    }
    if (pend[i].resolving) {
      int st = ip_resolve_state(pend[i].tag);
      if (st == IP_RES_BUSY) {       /* still working; nobody else may start */
        resolver_taken = 1;
        continue;
      }
      pend[i].resolving = 0;
      pend[i].busy = 0;
      if (st == IP_RES_OK) {
        uint8_t a[4];
        ip_resolve_addr(pend[i].tag, a);
        if (start_connect(i, a, pend[i].port) < 0)
          net_stack_setstate(i, NET_ST_ERROR);
      } else {
        net_stack_setstate(i, NET_ST_ERROR);
      }
      ip_resolve_release(pend[i].tag); /* our answer, consumed */
      continue;
    }
    if (resolver_taken) continue;  /* our turn comes on a later pass */
    {
      int tag = ip_resolve(pend[i].host);
      if (tag > 0) {
        pend[i].tag = (uint8_t)tag;
        pend[i].resolving = 1;
        resolver_taken = 1; /* one lookup at a time */
        continue;
      }
      /* Refused with the resolver idle: the NAME is unusable (malformed, or
       * too long), and retrying next pass would wedge this slot — and, via
       * the return below, every later channel's pending connect — forever. */
      if (res_state != IP_RES_BUSY) {
        pend[i].busy = 0;
        net_stack_setstate(i, NET_ST_ERROR);
        continue;
      }
      /* Busy with somebody else's name: try again next pass. */
      resolver_taken = 1;
    }
  }
}

int ip_listen(int ch, uint16_t port) {
  if (ch <= 0 || ch >= NET_CHANNELS || port == 0) return -1;
  lport[ch] = port;
  uip_listen(HTONS(port));
  return 0;
}

/* A dead connection's teardown: unbind it so the service loop aborts it
 * (RST), but KEEP the port — this is net_hangup's path for a channel in
 * ERROR, where a graceful close has nothing left to be graceful about. */
void ip_abandon(int ch) {
  struct uip_conn *c = conn_for(ch);
  if (c == 0) return;
  c->appstate.ch = 0xFFu;
  c->appstate.closing = 0;
}

int ip_hangup(int ch) {
  struct uip_conn *c = conn_for(ch);
  if (c == 0) return 0; /* already gone: the caller may return to LISTEN */
  if (c->appstate.sent != 0) return -1; /* a segment is still unacknowledged */
  /* Graceful, and it has to go through an appcall to be graceful at all:
   * uip_close() is a flag uip_process reads after the callback returns, so
   * setting it here would emit nothing and the connection would eventually
   * be aborted instead — a RST, which lets the client discard the response
   * bytes it has received but not yet read. That truncation is the exact
   * thing this function exists to prevent. `closing` is the request; the
   * appcall issues the close once the send queue has drained. */
  c->appstate.closing = 1;
  return 0;
}

/* ----------------------------------------------------------- the service */

static void ip_deliver(uint16_t n) {
  ip_stat_rx++;
  if (ping_intercept(pkt, n)) return;
  /* One MVN into uIP's buffer: 576 bytes of C byte-loop at -O0 would be
   * ~57,000 cycles, a whole frame, per datagram. */
  link_mb_move(uip_buf, pkt, n);
  uip_len = n;
  uip_input();
  ip_send_uip();
}

static void ip_timers(void) {
  int i;
  for (i = 0; i < UIP_CONNS; i++) {
    uip_periodic(i);
    ip_send_uip();
  }
#if UIP_UDP
  for (i = 0; i < UIP_UDP_CONNS; i++) {
    uip_udp_periodic(i);
    ip_send_uip();
  }
#endif
}

void ip_init(void) {
  asm_len = 0;
  asm_need = 0;
  asm_got = 0;
  ip_busy = 0;
  ping_next_seq = 0;
  hello_last = 0;
  tick_last = 0;
  if (!ip_started) {
    uip_ipaddr_t a;
    int i;
    ip_started = 1;
    uip_init();
    /* 0xFF means "no channel". Set here because uIP does not clear appstate
     * for a connection it accepts, and the inbound path above reads it to
     * tell a fresh connection from a bound one. */
    for (i = 0; i < UIP_CONNS; i++) uip_conns[i].appstate.ch = 0xFFu;
    uip_ipaddr(a, IP_SELF0, IP_SELF1, IP_SELF2, IP_SELF3);
    uip_sethostaddr(a);
    uip_ipaddr(a, IP_SELF0, IP_SELF1, IP_SELF2, IP_GW3);
    uip_setdraddr(a);
    uip_ipaddr(a, 255, 255, 255, 0);
    uip_setnetmask(a);
  }
}

/* Called with the scheduler pinned. Refuse a suspended service pass: clearing
 * its tables would leave it resuming into somebody else's stack state. */
int ip_reset(void) {
  int i;
  if (ip_busy) return -1;
  ip_started = 0;
  ip_init();
  uip_len = 0;
  memset(&uip_stat, 0, sizeof uip_stat);
  for (i = 0; i < NET_CHANNELS; i++) {
    lport[i] = 0;
    pend[i].busy = 0;
    pend[i].resolving = 0;
  }
  res_state = IP_RES_IDLE;
  res_conn = 0;
  res_kick = 0;
  res_qlen = 0;
  res_tries = 0;
  res_name[0] = 0;
  for (i = 0; i < 4; i++) res_addr[i] = 0;
  /* Do not rewind tags or DNS ids: old callers and late answers must not
   * match the first lookup in the new session. */
  res_tag = (uint8_t)(res_tag + 1);
  if (res_tag == 0) res_tag = 1;
  hello_last = (uint16_t)(snos_frames - 121u);
  tick_last = snos_frames;
  ip_ping_replies = ip_ping_rtt = ip_ping_seq = 0;
  ip_stat_rx = ip_stat_tx = ip_stat_drop = ip_stat_desync = 0;
  return 0;
}

void ip_service(void) {
  int n;
  uint8_t take;

  /* Two processes may yield through kbd_idle concurrently; the state below
   * (reassembly, pkt[], and every byte of uIP) is single-threaded. Test-and-
   * set under NMI-off — NMI-off only because a context switch between the
   * test and the set is exactly the race being closed. */
  R8(0x4200) = 0x01;
  take = ip_busy;
  ip_busy = 1;
  R8(0x4200) = 0x81;
  if (take) return;
  /* ONLY THIS FUNCTION MAY TOUCH uIP.
   *
   * ip_busy stops a second ip_service, but that is not enough on its own: the
   * shell reaches here from kernel code rather than mid-syscall, so the
   * scheduler can preempt it anywhere in this function, and the application
   * that gets scheduled can then call SYS_NET_CONNECT. If those entry points
   * ran uIP they would repoint uip_conn under a half-finished uip_process —
   * aborting a healthy connection, or rebuilding uip_buf mid-segment.
   *
   * So they do not run uIP at all. ip_connect records the request in pend[],
   * ip_close unbinds the channel, ip_hangup sets `closing`, and the loops at
   * the bottom of this function perform the actual uip_process calls. Every
   * one of those is a single-byte or single-word store, which no context
   * switch can tear.
   *
   * proc_lock() is NOT the answer here and was tried: it clears PROC_WANT,
   * which cancels the switch kbd_idle requests immediately before calling
   * this — so the two processes stopped alternating and the machine wedged
   * the moment anything ran in the background. */

  if (!net_up()) {
    ip_busy = 0;
    return;
  }

  /* Ask the gateway what it is, until it answers. net_gw_version is zeroed at
   * boot and again on a peer restart, so this also re-arms IP mode after the
   * daemon bounces — the new process states its own capabilities. */
  if (net_gw_version == 0 && (uint16_t)(snos_frames - hello_last) > 120u) {
    hello_last = snos_frames;
    net_hello();
  }

  for (;;) {
    if (asm_len < 2) {
      uint8_t b;
      n = net_ip_read(&b, 1);
      if (n == -1) goto desync;
      if (n == 0) break;
      if (asm_len == 0) {
        asm_need = b;
        asm_len = 1;
      } else {
        asm_need |= (uint16_t)((uint16_t)b << 8);
        asm_len = 2;
        asm_got = 0;
        if (asm_need == 0 || asm_need > IP_MTU) goto desync;
      }
      continue;
    }
    n = net_ip_read(&pkt[asm_got], (int)(asm_need - asm_got));
    if (n == -1) goto desync;
    if (n == 0) break;
    asm_got = (uint16_t)(asm_got + (uint16_t)n);
    if (asm_got == asm_need) {
      asm_len = 0;
      ip_deliver(asm_need);
    }
  }

  /* Six seconds is well past four tries half a second apart; past it the
   * answer is not coming and the resolver must be usable again. */
  if (res_state == IP_RES_BUSY &&
      (uint16_t)(snos_frames - res_started) > 360u)
    res_state = IP_RES_FAIL;

  res_service();
  pend_service();
  net_unfwd_retry();

  if ((uint16_t)(snos_frames - tick_last) >= IP_TICK_FRAMES) {
    tick_last = snos_frames;
    ip_timers();
  }

  /* Poll every live connection every pass, not only on the half-second
   * timer. uip_poll is the ONLY event that lets a connection send data the
   * application queued while nothing was arriving — waiting for the periodic
   * would put half a second between `sys_net_write` and the segment leaving,
   * on every request an application makes. */
  {
    int i;
    for (i = 0; i < UIP_CONNS; i++) {
      if (uip_conns[i].tcpstateflags == UIP_CLOSED) continue;
      if (uip_conns[i].appstate.ch == 0xFFu) {
        /* Orphaned by ip_close, or already closing gracefully. The first
         * needs one poll so the appcall can abort it; the second needs to be
         * left alone to finish. */
        if (uip_conns[i].appstate.closing == 2) continue;
        uip_poll_conn(&uip_conns[i]);
        ip_send_uip();
        continue;
      }
      /* A connection whose window we closed (the appcall's "close one
       * segment early") is polled as soon as its ring has room again: the
       * appcall's uip_restart then sends the window update. Waiting for the
       * periodic tick instead held every download segment for up to half a
       * second -- with a 1460-byte window, a segment and then nothing, so
       * downloads ran at one segment per tick (docs/link-throughput.md). */
      if (net_stack_txused(uip_conns[i].appstate.ch) == 0 &&
          uip_conns[i].appstate.closing != 1 &&
          !(uip_stopped(&uip_conns[i]) &&
            net_stack_rxfree(uip_conns[i].appstate.ch) >= UIP_TCP_MSS))
        continue; /* nothing to say; do not spend a uip_process on it */
      uip_poll_conn(&uip_conns[i]);
      ip_send_uip();
    }
  }

  ip_busy = 0;
  return;

desync:
  /* Either the peer restarted mid-datagram (net_ip_read says so explicitly)
   * or a length prefix made no sense. Same cure both times: drop everything
   * buffered and start clean at the next prefix the daemon writes.
   *
   * Counted only when something was actually LOST. A gateway announcing its
   * restart with no datagram in flight is the ordinary start of a session —
   * the first real run reported "desync 1" before a single packet had been
   * exchanged, which makes the counter useless for the thing it is for. */
  if (asm_len != 0) ip_stat_desync++;
  asm_len = 0;
  /* Drain the WHOLE ring, not one bufferful: the ring holds 2 KB and a single
   * read takes at most 576, so the old loop-with-a-break left up to 1471
   * poisoned bytes to be parsed as a length prefix on the very next pass. */
  while (net_ip_read(pkt, IP_MTU) > 0) {
  }
  ip_busy = 0;
}
