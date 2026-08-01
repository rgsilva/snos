/* IPv4 on a Super Nintendo (M11 step 1: ICMP by hand, the stack follows).
 *
 * This file owns everything between net.c's LINK_CH_IP byte stream and the
 * protocols: datagram framing, header validation, and — for now — an ICMP
 * echo responder written out longhand. Step 2 replaces the longhand with uIP;
 * the framing, the checksum discipline and the service loop stay.
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
 * why a nonsense length here drops the whole buffered stream rather than
 * trying to be clever: one desync event costs at most the ring's contents,
 * and the daemon's packets after it parse cleanly again.
 *
 * ----------------------------------------------------------- concurrency
 *
 * ip_service runs in the FOREGROUND, from kbd_idle — never in NMI. With M10
 * both processes yield through kbd_idle, so two processes can race into it;
 * a test-and-set under NMI-off makes the second one simply skip a turn. The
 * NMI's only involvement is moving link bytes, on the other side of net.c's
 * single-writer rings.
 */
#include "ip.h"
#include "net.h"
#include "snos.h"

/* kernel/netasm.s: ones-complement sum, not complemented, n EVEN. */
extern uint16_t ip_sum(const uint8_t *p, uint16_t n);

volatile uint16_t ip_ping_replies;
volatile uint16_t ip_ping_rtt;
volatile uint16_t ip_ping_seq;
volatile uint16_t ip_stat_rx;
volatile uint16_t ip_stat_tx;
volatile uint16_t ip_stat_drop;
volatile uint16_t ip_stat_desync;

/* Ping bookkeeping. The id is fixed and ours; the daemon's NAT echoes it
 * back, so a reply is matched by id alone and the seq/stamp ride in the
 * payload where the far end must echo them byte-for-byte (RFC 792). */
#define PING_ID_HI 0x53u /* 'S' */
#define PING_ID_LO 0x4Eu /* 'N' */
static uint16_t ping_next_seq;

static uint8_t pkt[IP_MTU];   /* one datagram, in or out */
static uint16_t asm_len;      /* bytes of the length prefix seen (0..2) */
static uint16_t asm_need;     /* datagram length once the prefix is whole */
static uint16_t asm_got;      /* payload bytes collected so far */

static uint8_t ip_busy;       /* two processes may race into ip_service */

/* HELLO pacing. The gateway only states its version when asked, nothing on
 * this link is unsolicited, and the bridge can claim the mailbox minutes
 * after boot — so ask, at boot and then patiently, until an answer names a
 * version. One 2-byte record every ~2 s is the cost of autoconfiguration. */
static uint16_t hello_last;

#define R8(a) (*(volatile uint8_t *)(a))

void ip_init(void) {
  asm_len = 0;
  asm_need = 0;
  asm_got = 0;
  ip_busy = 0;
  ping_next_seq = 0;
  hello_last = 0;
}

int ip_up(void) { return net_up() && net_ipmode(); }

/* Fold-and-finish helpers over the asm sum. csum_add is RFC 1071 addition;
 * csum_odd is the trailing byte ip_sum's even-count contract excludes. */
static uint16_t csum_add(uint16_t a, uint16_t b) {
  uint32_t t = (uint32_t)a + (uint32_t)b;
  t = (t & 0xFFFFu) + (t >> 16);
  t = (t & 0xFFFFu) + (t >> 16);
  return (uint16_t)t;
}

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

/* Queue one datagram toward the daemon: prefix, then packet. All or nothing —
 * net_ip_write refuses partial writes by contract, so a full ring costs the
 * datagram (IP is allowed to drop) and never poisons the stream. */
static void ip_output(const uint8_t *p, uint16_t n) {
  uint8_t pre[2];
  if ((int)n + 2 > net_ip_wspace()) {
    ip_stat_drop++;
    return;
  }
  pre[0] = (uint8_t)n;
  pre[1] = (uint8_t)(n >> 8);
  net_ip_write(pre, 2);
  net_ip_write(p, (int)n);
  ip_stat_tx++;
}

int ip_ping_send(const uint8_t dst[4]) {
  uint8_t q[36]; /* 20 IP + 8 ICMP + 8 payload */
  uint16_t s;
  if (!ip_up()) return -1;
  q[0] = 0x45; /* IPv4, 20-byte header */
  q[1] = 0;
  put16(&q[2], 36);
  put16(&q[4], ping_next_seq); /* id: reuse the seq; uniqueness is enough */
  put16(&q[6], 0x4000);        /* DF, no fragments — we never fragment */
  q[8] = 64;                   /* TTL */
  q[9] = 1;                    /* ICMP */
  q[10] = 0;
  q[11] = 0;
  q[12] = IP_SELF0; q[13] = IP_SELF1; q[14] = IP_SELF2; q[15] = IP_SELF3;
  q[16] = dst[0]; q[17] = dst[1]; q[18] = dst[2]; q[19] = dst[3];
  s = (uint16_t)~csum_range(q, 20);
  put16(&q[10], s);
  q[20] = 8; /* echo request */
  q[21] = 0;
  q[22] = 0;
  q[23] = 0;
  q[24] = PING_ID_HI;
  q[25] = PING_ID_LO;
  put16(&q[26], ping_next_seq);
  /* Payload: the frame stamp the reply must echo, plus a signature that
   * makes a mangled echo visible as a wrong RTT rather than a crash. */
  put16(&q[28], snos_frames);
  q[30] = 'S'; q[31] = 'N'; q[32] = 'O'; q[33] = 'S';
  q[34] = 0;
  q[35] = 0;
  s = (uint16_t)~csum_range(&q[20], 16);
  put16(&q[22], s);
  ping_next_seq++;
  ip_output(q, 36);
  return 0;
}

/* One valid-looking IPv4 packet addressed to whoever it is addressed to. */
static void ip_input(uint8_t *p, uint16_t n) {
  uint16_t ihl, total;
  if (n < 20) { ip_stat_drop++; return; }
  if ((p[0] >> 4) != 4) { ip_stat_drop++; return; }
  ihl = (uint16_t)((p[0] & 0x0Fu) << 2);
  total = get16(&p[2]);
  if (ihl < 20 || total < ihl || total > n) { ip_stat_drop++; return; }
  if (csum_range(p, ihl) != 0xFFFFu) { ip_stat_drop++; return; }
  if (p[16] != IP_SELF0 || p[17] != IP_SELF1 || p[18] != IP_SELF2 ||
      p[19] != IP_SELF3) {
    ip_stat_drop++; /* not ours; point-to-point has no forwarding */
    return;
  }
  ip_stat_rx++;

  if (p[9] == 1) { /* ICMP */
    uint8_t *ic = p + ihl;
    uint16_t icn = (uint16_t)(total - ihl);
    if (icn < 8) { ip_stat_drop++; return; }
    if (csum_range(ic, icn) != 0xFFFFu) { ip_stat_drop++; return; }

    if (ic[0] == 8) { /* echo request: answer it ourselves, longhand */
      int i;
      /* Swap src and dst — checksum-neutral for the IP header, so only the
       * TTL refresh below forces a recompute. */
      for (i = 0; i < 4; i++) {
        uint8_t t = p[12 + i];
        p[12 + i] = p[16 + i];
        p[16 + i] = t;
      }
      p[8] = 64; /* our reply, our TTL */
      p[10] = 0;
      p[11] = 0;
      put16(&p[10], (uint16_t)~csum_range(p, ihl));
      ic[0] = 0; /* echo reply */
      ic[2] = 0;
      ic[3] = 0;
      put16(&ic[2], (uint16_t)~csum_range(ic, icn));
      ip_output(p, total);
      return;
    }
    if (ic[0] == 0 && ic[4] == PING_ID_HI && ic[5] == PING_ID_LO) {
      /* An answer to ip_ping_send: RTT from the echoed stamp. */
      if (icn >= 10) {
        ip_ping_seq = get16(&ic[6]);
        ip_ping_rtt = (uint16_t)(snos_frames - get16(&ic[8]));
        ip_ping_replies++;
      }
      return;
    }
    ip_stat_drop++; /* other ICMP: nothing to do with it yet */
    return;
  }

  /* TCP and UDP arrive with the stack (steps 2-5). Until then they are
   * counted, not silently lost, so a too-early test says so. */
  ip_stat_drop++;
}

void ip_service(void) {
  int n;
  uint8_t take;

  /* Two processes may yield through kbd_idle concurrently; the state below
   * (reassembly, pkt[]) is single-threaded. Test-and-set under NMI-off —
   * NMI-off only because a context switch between the test and the set is
   * exactly the race being closed. */
  R8(0x4200) = 0x01;
  take = ip_busy;
  ip_busy = 1;
  R8(0x4200) = 0x81;
  if (take) return;

  if (!net_up()) {
    ip_busy = 0;
    return;
  }

  /* Ask the gateway what it is, until it answers. net_gw_version is zeroed
   * at boot and again on a peer restart, so this also re-arms IP mode after
   * the daemon bounces — the new process states its own capabilities. */
  if (net_gw_version == 0 &&
      (uint16_t)(snos_frames - hello_last) > 120u) {
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
      ip_input(pkt, asm_need);
      asm_len = 0;
    }
  }

  ip_busy = 0;
  return;

desync:
  /* Either the peer restarted mid-datagram (net_ip_read said so) or a length
   * prefix made no sense. Same cure both times: drop everything buffered and
   * start clean at the next prefix the daemon writes. Counted, because a
   * desync that happens more than once per gateway restart is a bug. */
  ip_stat_desync++;
  asm_len = 0;
  while (net_ip_read(pkt, IP_MTU) > 0) {
  }
  ip_busy = 0;
}
