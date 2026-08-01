/* uIP, configured for a Super Nintendo (M11).
 *
 * uipopt.h includes this file and nothing else configures the stack; every
 * number below is a SNOS decision, so they are all explained here rather
 * than left to uIP's defaults.
 *
 * Why uIP at all: it was written in 2001 for 8- and 16-bit microcontrollers
 * with a few hundred bytes of RAM, which describes this machine's budget
 * better than anything written since. Its central simplification — ONE packet
 * buffer, ONE outstanding segment per connection, the application called back
 * to re-supply data on retransmission instead of the stack buffering it — is
 * exactly the trade a 3.58 MHz CPU with 8 KB of usable bank-$00 wants.
 * (Author's own summary: "the sliding window is the thing you give up.")
 */
#ifndef UIP_CONF_H
#define UIP_CONF_H

#include <stdint.h>

typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
/* uIP's own accumulator type for statistics counters. */
typedef unsigned short uip_stats_t;

/* The 65816 is little-endian; the SNES has no byte-swap instruction worth
 * using here, so HTONS() compiles to the obvious shift-and-or. Getting this
 * wrong does not fail loudly — it produces packets whose every field is
 * byte-swapped, which the far end drops as garbage. */
#define UIP_CONF_BYTE_ORDER UIP_LITTLE_ENDIAN

/* NO LINK-LAYER HEADER. The link to the daemon is point-to-point and carries
 * IP datagrams directly (kernel/ip.c frames them with a length prefix), so
 * there is no ethernet header, no ARP, and nothing to resolve. That is also
 * why uip_arp.c is not part of the build. */
#define UIP_CONF_LLH_LEN 0

/* One datagram. 576 is the IPv4 "must accept" minimum (RFC 791) and leaves
 * TCP an MSS of 536 — the classic floor, which is what the far side of the
 * NAT advertises too. Bigger buys little: the link underneath moves 128-byte
 * frames, so a large datagram is many frames whatever this says. */
#define UIP_CONF_BUFFER_SIZE 576

/* Eight, to match NET_CHANNELS: every SNOS channel can be a live connection
 * and none of them can find the stack full when the channel layer says there
 * is room. ~30 bytes each in far WRAM, where there is ~29 KB spare. */
#define UIP_CONF_MAX_CONNECTIONS 8
#define UIP_CONF_MAX_LISTENPORTS 4

/* UDP, for DNS. The console resolves names ITSELF — real queries in real
 * packets — which is the difference between having a network stack and having
 * a gateway that has one. Two sockets: one query in flight and one spare. */
#define UIP_CONF_UDP 1
#define UIP_CONF_UDP_CONNS 2
#define UIP_CONF_UDP_CHECKSUMS 1
#define UIP_CONF_BROADCAST 0

/* Counters are cheap and this is a stack nobody here has debugged before:
 * `net status` printing "17 dropped, 3 bad checksums" is the difference
 * between diagnosing a link and guessing at one. */
#define UIP_CONF_STATISTICS 1
#define UIP_CONF_LOGGING 0

/* Application hooks. The names are what uip.c calls; the bodies are in
 * kernel/ip.c. */
void snos_tcp_appcall(void);
void snos_udp_appcall(void);
#define UIP_APPCALL snos_tcp_appcall
#define UIP_UDP_APPCALL snos_udp_appcall

/* Per-connection application state, which uIP stores inside uip_conn for us.
 * Keeping the channel number here is what lets the appcall — which is handed
 * a connection, not a channel — find the rings to move bytes through. */
typedef struct snos_tcp_state {
  uint8_t ch;      /* SNOS channel, or 0xFF while unbound */
  uint8_t sent;    /* bytes handed to uip_send and not yet acknowledged */
  uint8_t closing; /* the app asked to close; send FIN once the queue drains */
} uip_tcp_appstate_t;

typedef struct snos_udp_state {
  uint8_t kind;    /* 1 = the resolver's socket */
} uip_udp_appstate_t;

#endif
