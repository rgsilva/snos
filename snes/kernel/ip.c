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
#include "net.h"
#include "snos.h"
#include "link.h" /* link_mb_move: one MVN within bank $7E */
#include "uip.h"

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
  if (n == 0 || (int)n + 2 > net_ip_wspace()) {
    ip_stat_drop++;
    return;
  }
  pre[0] = (uint8_t)n;
  pre[1] = (uint8_t)(n >> 8);
  net_ip_write(pre, 2);
  net_ip_write(p, (int)n);
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
void snos_tcp_appcall(void) {
  /* Step 4 fills this in. Until a connection can be created there is nothing
   * to be called about, and an empty body is what keeps the stack linked and
   * exercised — ICMP and the timers run through the same uip_process. */
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
static struct uip_udp_conn *res_conn;

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
  uip_ipaddr_t a;
  int i;
  if (!ip_up()) return -1;
  if (res_state == IP_RES_BUSY) return -1;
  for (i = 0; i < RES_NAME_MAX && name[i]; i++) res_name[i] = name[i];
  if (i == 0 || (i == RES_NAME_MAX && name[i])) return -1;
  res_name[i] = 0;
  res_id++;
  if (res_build(res_name) < 0) return -1;
  if (res_conn == 0) {
    uip_ipaddr(a, IP_SELF0, IP_SELF1, IP_SELF2, IP_DNS3);
    res_conn = uip_udp_new(&a, HTONS(53));
    if (res_conn == 0) return -1;
    res_conn->appstate.kind = 1;
  }
  res_state = IP_RES_BUSY;
  res_tries = 4; /* ~2 s of patience, at one try per periodic tick */
  /* Send the first query NOW rather than waiting for the next 0.5 s tick.
   * uip_udp_periodic_conn runs the appcall with uip_poll() set, which is the
   * only event a UDP connection ever gets. */
  uip_udp_periodic_conn(res_conn);
  ip_send_uip();
  return 0;
}

int ip_resolve_state(void) { return res_state; }

void ip_resolve_addr(uint8_t out[4]) {
  int i;
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
    ip_started = 1;
    uip_init();
    uip_ipaddr(a, IP_SELF0, IP_SELF1, IP_SELF2, IP_SELF3);
    uip_sethostaddr(a);
    uip_ipaddr(a, IP_SELF0, IP_SELF1, IP_SELF2, IP_GW3);
    uip_setdraddr(a);
    uip_ipaddr(a, 255, 255, 255, 0);
    uip_setnetmask(a);
  }
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

  if ((uint16_t)(snos_frames - tick_last) >= IP_TICK_FRAMES) {
    tick_last = snos_frames;
    ip_timers();
  }

  ip_busy = 0;
  return;

desync:
  /* Either the peer restarted mid-datagram (net_ip_read says so explicitly)
   * or a length prefix made no sense. Same cure both times: drop everything
   * buffered and start clean at the next prefix the daemon writes. Counted,
   * because a desync that happens more than once per gateway restart is a
   * bug rather than a fact of life. */
  ip_stat_desync++;
  asm_len = 0;
  while (net_ip_read(pkt, IP_MTU) > 0) {
  }
  ip_busy = 0;
}
