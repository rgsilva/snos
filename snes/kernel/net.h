/* Streams over the gateway link.
 *
 * The SNES-side abstraction is a STREAM, always, and that is the design's load
 * bearing simplification. Commands differ only in how a stream is opened —
 * FETCH now, OPEN host:port later, a WebSocket after that — and none of those
 * change anything above this header. The gateway can grow new ways to open a
 * stream indefinitely while the kernel and every application stay put.
 *
 * The gateway terminates TLS and speaks HTTP. That is not a shortcut taken for
 * convenience: a TLS handshake needs elliptic-curve arithmetic, which on a
 * 3.58 MHz 65816 runs to minutes per connection, and gzip and chunked encoding
 * are similarly the wrong work to do here. The SNES never sees an IP packet,
 * never does crypto, and never parses a header block.
 */
#ifndef SNOS_NET_H
#define SNOS_NET_H

#include <stdint.h>

/* Eight. The number is set by what fits, and 1536 bytes per channel against
 * ~35 KB of free far WRAM means it was never memory that limited this — 4 was a
 * guess made when the question was "can it do two or three at once" and it
 * simply never got revisited. Eight covers a server listening, a shell session
 * and a download at the same time, with spare.
 *
 * It does NOT make a browser work. Listening is one connection at a time by
 * design — the gateway blocks in pump() for the life of each — so a browser's
 * six parallel connections still queue no matter how many channels exist. That
 * needs the listener handing each accept a different channel, which is a
 * protocol change and dubious value on a 615 B/s link. */
#define NET_CHANNELS 8

/* 1 KB per channel out of HiRAM's 52 KB.
 *
 * Deliberately small. When a ring fills, the receiver simply stops acking, so
 * the sender retransmits and stops making progress — which is backpressure, and
 * it propagates through the gateway into TCP's own window without a single line
 * of flow control on either side. Large buffers would only delay that, and
 * delay is exactly what you do not want between a slow consumer and a fast
 * server. At the current throughput 1 KB is over half a second of slack. */
#define NET_RING 1024

/* The NET_ST_* values live in snos_abi.h, because an application receives them
 * from SYS_NET_STATE and cannot interpret the result without them. Defining
 * them twice invites the two copies to drift, and the drift would show up as a
 * state machine that disagrees with itself across the syscall boundary. */
#include "snos_abi.h"

void net_init(void);
void net_service(void); /* one link poll; called from the NMI handler */
int net_up(void);       /* is a gateway attached at all */

int net_fetch(const char *url); /* -> channel, or -1 if none free */
int net_read(int ch, uint8_t __far *dst, int max); /* bytes moved, 0 = none yet */
int net_state(int ch);
uint16_t net_http_status(int ch);
/* 1 when the gateway converted the body to Mode 3 tile data. It decides from
 * the Content-Type, so the caller never has to declare in advance what it is
 * fetching — but it only knows once the status has arrived. */
int net_is_image(int ch);
void net_close(int ch);
void net_own(int ch, uint8_t tag);
int net_reap(uint8_t tag);

/* Counters, purely so tests can assert that the recovery paths actually ran
 * rather than that the transfer merely succeeded. A retransmit that never
 * happens and a retransmit that works look identical from the outside. */
extern volatile uint16_t net_stat_rx_frames;
extern volatile uint16_t net_stat_hello;

/* The gateway's own address, from its HELLO. The SNES has none, so this is what
 * anything it serves is reached at. */
#define NET_TEXT_MAX 48
extern char net_text[NET_TEXT_MAX + 1];
extern volatile uint16_t net_gw_version;
int net_hello(void);
int net_connect(const char *hostport);
int net_listen(uint16_t port);
int net_hangup(int ch);
int net_write(int ch, const uint8_t *src, int n);
int net_wspace(int ch);
extern volatile uint16_t net_stat_bad_sum;
extern volatile uint16_t net_stat_resent;
/* Data frames refused for want of receive-ring space, i.e. backpressure events.
 * Non-zero means an application is not draining fast enough and the peer is
 * retransmitting at it; see the comment on the definition in net.c. */
extern volatile uint16_t net_stat_refused;

/* ---- M11: the raw-IP datagram path (kernel/ip.c is the only consumer) ----
 *
 * net.c owns the rings and their single-writer discipline, exactly as it does
 * for the stream channels; ip.c owns everything above the byte stream. The
 * gateway claims IP support with its HELLO version (>= 2), and until it does
 * the kernel never puts a LINK_CH_IP record on the wire. */
int net_ipmode(void); /* gateway speaks IP (slirp): HELLO version >= 2 */

/* ---- channels backed by the STACK rather than by the gateway -------------
 *
 * A channel is a pair of rings and a state; who fills them is a detail an
 * application never sees. Before M11 the gateway held the socket and the NMI
 * moved the bytes; a stack-backed channel has uIP on this side of the wire
 * and the FOREGROUND (ip_service) moving them instead. SYS_NET_CONNECT and
 * everything above it are unchanged, which is the point — `irc` runs over
 * the console's own TCP without a line of its own changing.
 *
 * The ring contracts hold either way. For a stack channel BOTH indices of
 * each ring are written by the foreground, which is strictly safer than the
 * NMI/foreground split it replaces; the one thing that must not happen is
 * the NMI also packing such a channel's txring, and build_frame skips them
 * for exactly that reason. */
int net_stack_claim(void);   /* a free channel, marked stack-backed, or -1 */
int net_stack_is(int ch);
void net_stack_setstate(int ch, int st);
int net_stack_rxfree(int ch);
int net_stack_rxput(int ch, const uint8_t *p, int n); /* 1 ok, 0 = no room */
int net_stack_txused(int ch);
/* Copy up to `max` unsent bytes out WITHOUT consuming them: uIP needs the
 * same bytes again on a retransmission, and only an acknowledgement is
 * permission to forget them. */
int net_stack_txcopy(int ch, uint8_t *dst, int max);
void net_stack_txdrop(int ch, int n);
/* Drain received IP-stream bytes. Returns bytes moved, 0 for none — and -1
 * exactly ONCE after a peer restart: the length-prefixed datagram framing
 * broke mid-stream when the peer died, so the caller must reset its
 * reassembly state. The stale bytes are already discarded when -1 returns. */
int net_ip_read(uint8_t *dst, int max);
/* Queue bytes toward the gateway. All or nothing — half a datagram on the
 * wire poisons the stream for every datagram after it. */
int net_ip_write(const uint8_t *src, int n);
int net_ip_wspace(void);

#endif
