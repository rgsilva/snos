/* The console's own IP layer (M11).
 *
 * Below this header: net.c moves opaque LINK_CH_IP bytes over the reliable
 * link, exactly as it moves stream bytes. Above it: real IPv4 packets that
 * the SNES composes and parses itself. The gateway daemon stops being a proxy
 * that understands protocols and becomes a slirp box — user-mode NAT, the
 * QEMU arrangement — that terminates our TCP and opens real sockets on our
 * behalf. Only the far side of the NAT is synthetic; every header on this
 * side is ours.
 *
 * ------------------------------------------------------------- addressing
 *
 * Slirp networks are configured by convention, not DHCP — QEMU guests have
 * been 10.0.2.15 since forever, and there is nothing to discover on a
 * two-node point-to-point subnet. These constants ARE the configuration; the
 * daemon hard-codes the same three. On real hardware the far end changes
 * (Pico W becomes the slirp box), the addresses do not — until a wired
 * tier-3 adapter gives the console a real LAN address, which is the day DHCP
 * becomes worth an on-console implementation (PLAN.md M11).
 */
#ifndef SNOS_IP_H
#define SNOS_IP_H

#include <stdint.h>

#define IP_SELF0 10
#define IP_SELF1 0
#define IP_SELF2 2
#define IP_SELF3 15
#define IP_GW3 2   /* 10.0.2.2 — the daemon itself (NAT + default route) */
#define IP_DNS3 3  /* 10.0.2.3 — the daemon's resolver front */

/* One datagram, one buffer. 576 is the classic "must accept" IP datagram
 * (RFC 791) and what uIP calls a sensible UIP_BUFSIZE; it fits three deep in
 * net.c's receive ring and leaves TCP an MSS of 536. The link's 128-byte
 * frames are irrelevant here — datagrams ride a length-prefixed byte stream,
 * so the MTU never has to know the frame size. */
#define IP_MTU 576

void ip_init(void);
/* The foreground pump: datagram reassembly, the stack, timers, and the
 * auto-HELLO that discovers whether the gateway speaks IP at all. Called
 * from kbd_idle — every blocking loop in the kernel funnels through there —
 * and safe against concurrent entry from two processes (it guards itself). */
void ip_service(void);
int ip_up(void); /* link up AND the gateway claimed IP support */

/* ---- DNS: the console resolves names ITSELF ------------------------------
 *
 * Real UDP queries to 10.0.2.3, parsed on the 65816. The daemon is only the
 * recursive resolver's address — it forwards the query to whatever this
 * machine resolves with and hands the answer back untouched. That is the
 * difference between having a stack and having a gateway that has one: the
 * old (deleted) `net resolve` asked the gateway to resolve a name and print
 * it, which measured the gateway.
 *
 * ONE query at a time, deliberately. A resolver cache and parallel queries
 * are what you build when name lookup is on a critical path; here it happens
 * once per connection, on a machine whose link moves 600 bytes a second.
 *
 * The slot is single but the ANSWER is tagged. ip_resolve hands back a
 * nonzero tag naming this lookup, and every later call takes it: a caller
 * whose lookup has been superseded reads FAIL rather than somebody else's
 * address. That closes the recorded one-frame race (an answer arriving and a
 * second lookup starting before the asker read it) without the owned-until-
 * released scheme that was tried and reverted — a stale tag can never lock
 * the resolver, because the slot itself was never the thing being owned. */
#define IP_RES_IDLE 0
#define IP_RES_BUSY 1
#define IP_RES_OK 2
#define IP_RES_FAIL 3
int ip_resolve(const char *name); /* start a lookup: tag > 0, -1 busy/not up */
int ip_resolve_state(uint8_t tag); /* FAIL for a superseded tag */
int ip_resolve_slot(void); /* the slot's raw state — `net status` only */
void ip_resolve_addr(uint8_t tag, uint8_t out[4]); /* only while OK and current */
/* Hand the resolver back. A stale tag is a no-op, so a caller that gave up
 * long ago cannot kill the lookup somebody else is waiting on. */
void ip_resolve_release(uint8_t tag);

/* ---- TCP, behind the existing ABI ----------------------------------------
 *
 * net_connect() calls ip_connect() when the gateway is a slirp box, and the
 * channel it returns behaves exactly as a gateway-backed one does: OPENING
 * until the handshake completes, then OPEN, then EOF or ERROR. Applications
 * are not aware any of this happened.
 *
 * `hostport` is "host:port", the same string SYS_NET_CONNECT already takes;
 * a name is resolved by the console (asynchronously — the channel simply
 * stays OPENING while that happens). */
int ip_connect(const char *hostport); /* -> channel, or -1 */
void ip_close(int ch);                /* abort/close whatever holds it */
int ip_listen(int ch, uint16_t port); /* bind a port to a channel */
/* End the current connection, keep the port. Refuses (-1) while a segment is
 * unacknowledged, for the same reason the stream path does: a close that
 * overtakes the response truncates what the client receives. */
int ip_hangup(int ch);
void ip_abandon(int ch); /* dead-connection hangup: abort, keep the port */

/* ---- ping, for the shell -------------------------------------------------
 * The console composes a real ICMP echo request and parses the real reply;
 * the RTT rides in the echoed payload as a frame-counter stamp. (The long-
 * deleted `net ping` measured the GATEWAY's connectivity over control
 * records — this one exercises the console's own packets, which is the
 * entire point of M11.) */
int ip_ping_send(const uint8_t dst[4]); /* 0 ok, -1 not up / no space */
extern volatile uint16_t ip_ping_replies; /* total echo replies to our id */
extern volatile uint16_t ip_ping_rtt;     /* frames, from the payload stamp */
extern volatile uint16_t ip_ping_seq;     /* seq of the latest reply */

/* Counters for `net status` — measured, not inferred. */
extern volatile uint16_t ip_stat_rx;      /* datagrams delivered to us */
extern volatile uint16_t ip_stat_tx;      /* datagrams sent */
extern volatile uint16_t ip_stat_drop;    /* not-ours / bad header / no room */
extern volatile uint16_t ip_stat_desync;  /* framing lost; stream resynced */

/* uIP's own counters, copied out so shell.c does not have to include the
 * stack's headers. They answer a different question from the four above: those
 * count what the LINK did, these count what the STACK made of it — "17
 * datagrams arrived and the stack dropped every one for a bad checksum" is a
 * sentence only these can say. Also the honest proof that uIP is the thing
 * answering, which a reply on the wire alone does not establish. */
/* Only the counters uIP actually maintains. Two of its fields are traps and
 * are deliberately absent: `tcp.sent` sits on the shared ip_send tail, so it
 * counts UDP output as TCP (three DNS queries read as "tcp 0/3", which is a
 * lie told confidently), and `udp.recv`/`udp.sent` are never incremented at
 * all. ip.sent is the honest count of everything the stack emitted. */
typedef struct {
  uint16_t ip_recv, ip_sent, ip_drop, ip_chkerr;
  uint16_t icmp_recv, icmp_sent, icmp_typeerr;
  uint16_t tcp_recv, tcp_drop, tcp_chkerr, tcp_rexmit, tcp_rst, tcp_syndrop;
  uint16_t udp_drop, udp_chkerr;
} ip_uipstat_t;
void ip_uipstat(ip_uipstat_t *out);

#endif
