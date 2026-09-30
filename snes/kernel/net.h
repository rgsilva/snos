/* Channels over the link — backed, since the M11 cutover, by the console's
 * own TCP/IP and nothing else.
 *
 * The SNES-side abstraction is a STREAM, always, and that is the design's
 * load-bearing simplification: a channel is a pair of rings and a state, and
 * applications read, write and watch the state without knowing what fills
 * the rings. Before the cutover that was a gateway holding sockets; now it
 * is uIP on this side of the wire, and no application changed — which was
 * the whole point of keeping this header stable.
 *
 * What the daemon still is: a slirp box (user-mode NAT) and a link peer. It
 * routes the console's IPv4, holds real sockets for inbound port-forwards,
 * and answers HELLO. It no longer understands any protocol the console
 * speaks over TCP — there is nothing left on the wire to understand.
 *
 * TLS is explicitly not a goal, period-correctly (PLAN.md M11): anything
 * that needs it can be fronted by an ordinary HTTP service (M12).
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
 * Listening is still one connection per channel at a time, by design: a
 * second SYN on a bound port is refused while a connection is being served
 * (kernel/ip.c, listen_ch_for), because interleaving two clients into one
 * ring pair would corrupt both. A server loops back to LISTEN on hangup, so
 * the refusal window is only as long as a request takes to serve. */
#define NET_CHANNELS 8

/* 2 KB per channel out of HiRAM's 52 KB.
 *
 * Deliberately small — when a ring fills, TCP's window closes and the sender
 * stops, which is backpressure with no flow-control code on either side. But
 * it has a hard floor: ip.c only reopens the window when a FULL segment fits
 * (rxfree >= UIP_TCP_MSS, and it must — uIP acks before the appcall, so a
 * segment that does not fit is LOST, not retried). The ring therefore has to
 * be comfortably larger than the MSS, and the MSS went 536 -> 1460 with the
 * MTU (uip-conf.h). At 1024 the window could never open at all: every TCP
 * connection wedged on arrival, found the hard way when the MTU was raised. */
#define NET_RING 2048

/* The NET_ST_* values live in snos_abi.h, because an application receives them
 * from SYS_NET_STATE and cannot interpret the result without them. Defining
 * them twice invites the two copies to drift, and the drift would show up as a
 * state machine that disagrees with itself across the syscall boundary. */
#include "snos_abi.h"

void net_init(void);
int net_reset(void); /* shell only; returns the number of failed channels */
void net_service(void); /* one link poll; called from the NMI handler */
/* Extra polls from the foreground, over and above the NMI's one per frame.
 * net_service_fg() masks NMI for the span, so to every shared index a
 * foreground poll is indistinguishable from an NMI poll; net_fg_wanted()
 * says whether one would do any good — the link is mid-transfer or has
 * something queued — so an idle session never spins the bridge. */
int net_fg_wanted(void);
void net_service_fg(void);
int net_up(void);       /* is a gateway attached at all */

int net_read(int ch, uint8_t __far *dst, int max); /* bytes moved, 0 = none yet */
int net_state(int ch);
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
/* Retry any port-forward withdrawal that did not fit in the control queue
 * when its channel closed. Foreground only; ip_service calls it each pass. */
void net_unfwd_retry(void);

/* ---- channels backed by the STACK rather than by the gateway -------------
 *
 * A channel is a pair of rings and a state; who fills them is a detail an
 * application never sees. Before M11 the gateway held the socket and the NMI
 * moved the bytes; a stack-backed channel has uIP on this side of the wire
 * and the FOREGROUND (ip_service) moving them instead. SYS_NET_CONNECT and
 * everything above it are unchanged, which is the point — `irc` runs over
 * the console's own TCP without a line of its own changing.
 *
 * BOTH indices of each ring are written by the foreground, which is strictly
 * safer than the NMI/foreground split the stream protocol needed; since the
 * cutover the NMI never touches a channel ring at all. */
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
/* Two spans, one indivisible record (prefix + datagram): a peer restart in
 * the NMI cannot land between them and leave an unprefixed packet on the
 * stream. Prefer this over two net_ip_write calls for anything framed. */
int net_ip_write2(const uint8_t *a, int na, const uint8_t *b, int nb);
int net_ip_wspace(void);

#endif
