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

/* Four channels, because that is comfortably more than the two or three
 * simultaneous streams anything here will want, and the cost is per-channel
 * ring buffers. */
#define NET_CHANNELS 4

/* 1 KB per channel, 4 KB in total out of HiRAM's 52 KB.
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
/* Identical stream, except the gateway decodes, scales and quantises the
 * resource into Mode 3 tile data first. The console cannot decode a JPEG in any
 * reasonable time and does not have to. */
int net_image(const char *url);
int net_read(int ch, uint8_t __far *dst, int max); /* bytes moved, 0 = none yet */
int net_state(int ch);
uint16_t net_http_status(int ch);
/* 1 when the gateway converted the body to Mode 3 tile data. It decides from
 * the Content-Type, so the caller never has to declare in advance what it is
 * fetching — but it only knows once the status has arrived. */
int net_is_image(int ch);
void net_close(int ch);
void net_own(int ch);
int net_reap(void);

/* Counters, purely so tests can assert that the recovery paths actually ran
 * rather than that the transfer merely succeeded. A retransmit that never
 * happens and a retransmit that works look identical from the outside. */
extern volatile uint16_t net_stat_rx_frames;
extern volatile uint16_t net_stat_hello;
extern volatile uint16_t net_gw_version;
int net_hello(void);
extern volatile uint16_t net_stat_bad_sum;
extern volatile uint16_t net_stat_resent;

#endif
