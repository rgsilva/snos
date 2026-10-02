#include "nat_core.h"
#include <string.h>

#define SNES 0x0a00020fu
#define GW 0x0a000202u
#define DNS 0x0a000203u
#define ICMP 1
#define TCP 6
#define UDP 17
#define FIN 1
#define SYN 2
#define RST 4
#define PSH 8
#define ACK 16
#define MSS 1460
#define WINDOW 2048
#define RTO 2500u /* slirp.py RTO */
#define DATA_RETRIES 120u /* slirp.py DATA_RETRIES */
#define MAX_RETRIES 8u /* slirp.py MAX_RETRIES */
#define TCP_IDLE 300000u /* slirp.py TCP_IDLE */
#define UDP_IDLE 60000u /* slirp.py UDP_IDLE */
#define ICMP_IDLE 15000u /* slirp.py ICMP_IDLE */
#define TIME_WAIT 4000u /* slirp.py TIME_WAIT */
#define FIN_GRACE 750u /* slirp.py FIN_GRACE */
#define INBOUND_CAP 4u /* slirp.py INBOUND_CAP */
#define TCP_RX (2 * MSS)
#define ECHO_REPLY 0
#define ECHO 8
#define SPEED_PORT 7778u
#define SPEED_LINE 24u
#define SPEED_TIMEOUT 15000u
#define SPEED_TCP_MAX 2u
typedef enum { FREE, CONNECTING, SYN_RCVD, SYN_SENT, EST, FIN_SENT, DONE } state_t;
typedef enum { SPEED_REQUEST, SPEED_GO, SPEED_DOWNLOAD, SPEED_UPLOAD, SPEED_REPORT } speed_t;
typedef struct {
    state_t state;
    uint16_t sport, dport;
    uint32_t dip, iss, snd_nxt, snd_una, rcv_nxt;
    uint16_t peer_wnd, unacked, out_len, in_len;
    uint8_t retries;
    bool peer_fin, sock_eof, fin_sent, inbound, close_write, unsent, local, speed_upload;
    bool retry_busy_early;
    bool speed_deadline_set, local_deadline_set;
    uint32_t touched, sent_at, eof_at, last_console, closed_at, local_deadline;
    uint32_t speed_deadline, speed_end, speed_count;
    uint16_t speed_frames;
    uint8_t speed_state, speed_line_len;
    uint8_t out[TCP_RX], in[MSS];
    uint8_t speed_line[SPEED_LINE];
} tcp_t;
typedef struct {
    bool used;
    uint16_t sport, dport;
    uint32_t reply_from, touched;
} udp_t;
/* An echo leaves with an identifier of the NAT's choosing, as on a Linux ping
 * socket.  Its low bits are the slot and the rest a generation bumped on every
 * reuse, so a reply that outlives its flow cannot match the slot's next flow;
 * wire therefore persists while the slot is free, and across reset. */
typedef struct {
    bool used;
    uint16_t ident, wire;
    uint32_t touched;
} icmp_t;
typedef char icmp_slots_divide_ids[(NAT_ICMP_MAX & (NAT_ICMP_MAX - 1)) == 0 ? 1 : -1];
typedef struct {
    bool used;
    uint16_t port;
    uint8_t owners;
} fwd_t;
/* These callbacks describe the adapter rather than a flow.  Reset keeps the
 * whole group intact, so a new hook cannot be accidentally left behind. */
typedef struct {
    bool (*emit_try)(void *, const uint8_t *, size_t);
    bool (*link_busy)(void *);
    void (*udp_close)(void *, unsigned);
    bool (*tcp_queued)(void *, unsigned);
    bool (*icmp_send)(void *, uint32_t, const uint8_t *, size_t);
} adapter_t;
struct nat {
    nat_ops_t op;
    adapter_t adapter;
    tcp_t tcp[NAT_TCP_MAX];
    udp_t udp[NAT_UDP_MAX];
    icmp_t icmp[NAT_ICMP_MAX];
    fwd_t fwd[NAT_TCP_MAX];
    uint32_t ident, iss, local_address, resolver[6];
    uint16_t next_eport;
    uint8_t resolver_count, resolver_next, tcp_next;
    nat_stats_t stat;
};
/* The core is entered under the lwIP lock. emit copies before returning, so these
   fixed buffers cannot be re-entered and keep packet-sized frames off the stack. */
static uint8_t ip_scratch[NAT_MTU], tcp_scratch[MSS + 24], udp_scratch[NAT_MTU - 20];
static uint16_t rd16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)rd16(p) << 16) | rd16(p + 2);
}
static void wr16(uint8_t *p, uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
}
static void wr32(uint8_t *p, uint32_t n) {
    wr16(p, n >> 16);
    wr16(p + 2, n);
}
static uint16_t sum(const uint8_t *p, size_t n, uint32_t initial) {
    uint32_t s = initial;
    while (n > 1) {
        s += rd16(p);
        p += 2;
        n -= 2;
    }
    if (n)
        s += (uint16_t)*p << 8;
    while (s >> 16)
        s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)~s;
}
static uint16_t pseudo(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *p, size_t n) {
    return sum(p, n, (src >> 16) + (src & 0xffffu) + (dst >> 16) + (dst & 0xffffu) + proto + n);
}
static bool expired(uint32_t now, uint32_t then, uint32_t span) {
    return (uint32_t)(now - then) > span;
}
static bool reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}
static bool busy(nat_t *n) {
    return n->adapter.link_busy && n->adapter.link_busy(n->op.arg);
}
static void count(nat_t *n) {
    n->stat.tcp = n->stat.udp = n->stat.forwards = n->stat.speed = n->stat.icmp = 0;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->tcp[i].state != FREE) {
            ++n->stat.tcp;
            if (n->tcp[i].local)
                ++n->stat.speed;
        }
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        if (n->udp[i].used)
            ++n->stat.udp;
    for (unsigned i = 0; i < NAT_ICMP_MAX; ++i)
        if (n->icmp[i].used)
            ++n->stat.icmp;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->fwd[i].used)
            ++n->stat.forwards;
}
static bool emit(nat_t *n, uint32_t src, uint8_t proto, const uint8_t *body, size_t len) {
    uint8_t *p = ip_scratch;
    if (len + 20 > NAT_MTU) {
        ++n->stat.drop;
        return false;
    }
    memset(p, 0, 20);
    p[0] = 0x45;
    wr16(p + 2, 20 + len);
    wr16(p + 4, ++n->ident);
    wr16(p + 6, 0x4000);
    p[8] = 64;
    p[9] = proto;
    wr32(p + 12, src);
    wr32(p + 16, SNES);
    wr16(p + 10, sum(p, 20, 0));
    memcpy(p + 20, body, len);
    if (n->adapter.emit_try ? n->adapter.emit_try(n->op.arg, p, len + 20)
                    : (n->op.emit(n->op.arg, p, len + 20), true)) {
        ++n->stat.out;
        return true;
    }
    ++n->stat.drop;
    return false;
}
static void tcp_served(nat_t *n, unsigned i) {
    n->tcp_next = (uint8_t)((i + 1) % NAT_TCP_MAX);
}
/* Preserve the old one-shot deferral when an already-due retry first finds a
 * busy link.  Once a flow has seen the link busy during its RTO, however, do
 * not move that RTO again: repeated busy/clear polling must not starve it. */
static void defer_retry(tcp_t *t, uint32_t now) {
    if (!expired(now, t->sent_at, RTO))
        t->retry_busy_early = true;
    else if (!t->retry_busy_early)
        t->sent_at = now;
}
static bool tcp_out(nat_t *n, tcp_t *t, uint8_t flags, const uint8_t *data, size_t len,
                    uint32_t seq) {
    uint8_t *b = tcp_scratch;
    size_t h = (flags & SYN) ? 24 : 20;
    memset(b, 0, h);
    wr16(b, t->dport);
    wr16(b + 2, t->sport);
    wr32(b + 4, seq);
    wr32(b + 8, t->rcv_nxt);
    b[12] = (uint8_t)(h / 4 << 4);
    b[13] = flags;
    wr16(b + 14, WINDOW);
    if (flags & SYN) {
        b[20] = 2;
        b[21] = 4;
        wr16(b + 22, MSS);
    }
    if (len)
        memcpy(b + h, data, len);
    wr16(b + 16, pseudo(t->dip, SNES, TCP, b, h + len));
    return emit(n, t->dip, TCP, b, h + len);
}
static void rst(nat_t *n, uint32_t dip, uint16_t sport, uint16_t dport, uint32_t next,
                uint32_t ack, bool had_ack) {
    uint8_t b[20] = {0};
    wr16(b, dport);
    wr16(b + 2, sport);
    wr32(b + 4, had_ack ? ack : 0);
    wr32(b + 8, next);
    b[12] = 0x50;
    b[13] = RST | ACK;
    wr16(b + 16, pseudo(dip, SNES, TCP, b, sizeof(b)));
    (void)emit(n, dip, TCP, b, sizeof(b));
}
static int find_tcp(nat_t *n, uint16_t sport, uint32_t dip, uint16_t dport) {
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) {
        tcp_t *t = &n->tcp[i];
        if (t->state != FREE && t->sport == sport && t->dip == dip && t->dport == dport)
            return (int)i;
    }
    return -1;
}
static int free_tcp(nat_t *n) {
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->tcp[i].state == FREE)
            return (int)i;
    return -1;
}
static unsigned local_tcp_count(const nat_t *n) {
    unsigned count = 0;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->tcp[i].state != FREE && n->tcp[i].local)
            ++count;
    return count;
}
static void close_tcp(nat_t *n, unsigned i) {
    if (n->tcp[i].state != FREE && !n->tcp[i].local)
        n->op.tcp_abort(n->op.arg, i);
    memset(&n->tcp[i], 0, sizeof(n->tcp[i]));
}
static void tcp_reset(nat_t *n, unsigned i) {
    tcp_t *t = &n->tcp[i];
    rst(n, t->dip, t->sport, t->dport, t->rcv_nxt, t->snd_nxt, true);
    close_tcp(n, i);
}
/* A queued lwIP write is accepted on the next poll.  Leaving the console
 * segment pending until then means a reset cannot leave its ACK on the link. */
/* The local peer uses the NAT's console-facing TCP flow, not an lwIP socket. */
static bool speed_line(tcp_t *t, const uint8_t *data, size_t len, uint32_t now) {
    while (len && (t->speed_state == SPEED_REQUEST || t->speed_state == SPEED_GO)) {
        if (t->speed_line_len == SPEED_LINE)
            return false;
        t->speed_line[t->speed_line_len++] = *data++;
        --len;
        if (t->speed_line[t->speed_line_len - 1] != '\n') {
            if (t->speed_line_len == SPEED_LINE)
                return false;
            continue;
        }
        if (t->speed_state == SPEED_REQUEST) {
            uint8_t kind = t->speed_line[0];
            uint32_t frames = 0;
            if (t->speed_line_len < 5 || (kind != 'D' && kind != 'U') ||
                t->speed_line[1] != ' ')
                return false;
            for (unsigned i = 2; i + 1 < t->speed_line_len; ++i) {
                uint8_t c = t->speed_line[i];
                if (c < '0' || c > '9')
                    return false;
                frames = frames * 10u + c - '0';
                if (frames > 7200u)
                    return false;
            }
            if (frames < 60u)
                return false;
            t->speed_frames = (uint16_t)frames;
            t->speed_upload = kind == 'U';
            t->speed_state = SPEED_GO;
            t->speed_deadline = now + SPEED_TIMEOUT;
            t->speed_deadline_set = true;
            t->out[0] = 'O';
            t->out[1] = 'K';
            t->out[2] = '\n';
            t->out_len = 3;
        } else if (t->speed_state == SPEED_GO) {
            if (t->speed_line_len != 3 || memcmp(t->speed_line, "GO\n", 3))
                return false;
            t->speed_state = t->speed_upload ? SPEED_UPLOAD : SPEED_DOWNLOAD;
            t->speed_end = now + (uint32_t)t->speed_frames * 1000u / 60u;
            if (!t->speed_upload) {
                t->speed_deadline = t->speed_end + SPEED_TIMEOUT;
                t->speed_deadline_set = true;
            }
        }
        t->speed_line_len = 0;
    }
    if (t->speed_state == SPEED_DOWNLOAD && len)
        return false;
    if (t->speed_state == SPEED_UPLOAD && !reached(now, t->speed_end))
        t->speed_count += (uint32_t)len;
    return true;
}
static void speed_report(tcp_t *t) {
    uint8_t digits[10];
    unsigned n = 0;
    uint32_t value = t->speed_count;
    do {
        digits[n++] = (uint8_t)('0' + value % 10u);
        value /= 10u;
    } while (value);
    for (unsigned i = 0; i < n; ++i)
        t->out[i] = digits[n - 1 - i];
    t->out[n] = '\n';
    t->out_len = (uint16_t)(n + 1);
}
static bool speed_received(tcp_t *t, const uint8_t *data, size_t len, uint32_t now) {
    if (t->speed_state == SPEED_REQUEST || t->speed_state == SPEED_GO)
        return speed_line(t, data, len, now);
    if (t->speed_state == SPEED_DOWNLOAD)
        return false;
    if (t->speed_state == SPEED_UPLOAD && !reached(now, t->speed_end))
        t->speed_count += (uint32_t)len;
    return true;
}
/* Return true when a download's hard deadline must discard in-flight data. */
static bool speed_poll(tcp_t *t, uint32_t now) {
    if ((t->speed_state == SPEED_REQUEST || t->speed_state == SPEED_GO) &&
        t->speed_deadline_set && reached(now, t->speed_deadline)) {
        if (t->out_len || t->unacked)
            return true;
        if (!t->sock_eof) {
            t->sock_eof = true;
            t->eof_at = now;
            t->local_deadline = now + 2 * SPEED_TIMEOUT;
            t->local_deadline_set = true;
        }
    }
    if (t->speed_state == SPEED_DOWNLOAD && t->speed_deadline_set &&
        reached(now, t->speed_deadline)) {
        if (t->unacked)
            return true;
        t->out_len = 0;
        if (!t->sock_eof) {
            t->sock_eof = true;
            t->eof_at = now;
            t->local_deadline = now + 2 * SPEED_TIMEOUT;
            t->local_deadline_set = true;
        }
    }
    if (t->speed_state == SPEED_UPLOAD && reached(now, t->speed_end) &&
        !t->out_len && !t->unacked) {
        speed_report(t);
        t->speed_state = SPEED_REPORT;
        t->speed_deadline = now + SPEED_TIMEOUT;
        t->speed_deadline_set = true;
    }
    if (t->speed_state == SPEED_DOWNLOAD && !t->peer_fin && !t->out_len &&
        !t->unacked && !t->sock_eof) {
        memset(t->out, 'x', MSS);
        t->out_len = MSS;
    }
    if (t->speed_state == SPEED_REPORT && t->speed_deadline_set &&
        reached(now, t->speed_deadline)) {
        if (t->out_len || t->unacked)
            return true;
        if (!t->sock_eof) {
            t->sock_eof = true;
            t->eof_at = now;
            t->local_deadline = now + 2 * SPEED_TIMEOUT;
            t->local_deadline_set = true;
        }
    }
    return false;
}
static bool try_write(nat_t *n, unsigned i, uint32_t now) {
    tcp_t *t = &n->tcp[i];
    if (!t->in_len)
        return true;
    if (t->local) {
        if (!speed_received(t, t->in, t->in_len, now)) {
            tcp_reset(n, i);
            return false;
        }
    } else if (!n->op.tcp_write(n->op.arg, i, t->in, t->in_len))
        return true;
    t->rcv_nxt += t->in_len;
    t->in_len = 0;
    (void)tcp_out(n, t, ACK, NULL, 0, t->snd_nxt);
    if (t->close_write && !t->local) {
        n->op.tcp_shutdown(n->op.arg, i);
        t->close_write = false;
    }
    return true;
}
/* TCP is intentionally stop-and-wait at the adapter boundary.  In particular,
 * the peer window is recorded on every segment and gates new output in poll. */
static void in_tcp(nat_t *n, uint32_t dip, const uint8_t *b, size_t len, uint32_t now) {
    if (len < 20) {
        ++n->stat.drop;
        return;
    }
    uint16_t sport = rd16(b), dport = rd16(b + 2), wnd = rd16(b + 14);
    uint32_t seq = rd32(b + 4), ack = rd32(b + 8);
    size_t off = (b[12] >> 4) * 4;
    uint8_t flags = b[13];
    if (off < 20 || off > len || pseudo(SNES, dip, TCP, b, len)) {
        ++n->stat.drop;
        return;
    }
    const uint8_t *data = b + off;
    size_t dn = len - off;
    int slot = find_tcp(n, sport, dip, dport);
    if (slot >= 0 && (flags & SYN) && !(flags & ACK) &&
        (n->tcp[slot].state == EST || n->tcp[slot].state == FIN_SENT ||
         n->tcp[slot].state == DONE)) {
        close_tcp(n, slot);
        slot = -1;
    }
    if (slot < 0) {
        if (flags & SYN) {
            bool local_speed = dip == n->local_address && dport == SPEED_PORT;
            if (local_speed && local_tcp_count(n) >= SPEED_TCP_MAX) {
                ++n->stat.drop;
                return;
            }
            slot = free_tcp(n);
            if (slot < 0) {
                ++n->stat.drop;
                return;
            }
            tcp_t *t = &n->tcp[slot];
            memset(t, 0, sizeof(*t));
            t->state = CONNECTING;
            t->sport = sport;
            t->dport = dport;
            t->dip = dip;
            t->iss = (n->iss += 0x1000);
            t->snd_nxt = t->snd_una = t->iss;
            t->rcv_nxt = seq + 1;
            t->touched = now;
            if (local_speed) {
                t->local = true;
                t->speed_state = SPEED_REQUEST;
                t->state = SYN_RCVD;
                t->local_deadline = now + 2 * SPEED_TIMEOUT;
                t->local_deadline_set = true;
                t->unsent = !tcp_out(n, t, SYN | ACK, NULL, 0, t->iss);
                t->snd_nxt = t->iss + 1;
                t->sent_at = now;
            } else if (!n->op.tcp_connect(n->op.arg, slot, dip, dport))
                tcp_reset(n, slot);
        } else if (!(flags & RST))
            rst(n, dip, sport, dport, seq + (uint32_t)dn + ((flags & FIN) ? 1 : 0), ack,
                flags & ACK);
        return;
    }
    tcp_t *t = &n->tcp[slot];
    t->touched = now;
    t->peer_wnd = wnd;
    if (flags & RST) {
        close_tcp(n, slot);
        return;
    }
    if (t->state == CONNECTING)
        return;
    if (t->state == SYN_RCVD) {
        if (!(flags & ACK) || ack != t->iss + 1)
            return;
        t->state = EST;
        t->snd_una = ack;
        if (t->local) {
            t->speed_deadline = now + SPEED_TIMEOUT;
            t->speed_deadline_set = true;
            t->local_deadline_set = false;
        }
    }
    if (t->state == SYN_SENT) {
        if ((flags & (SYN | ACK)) == (SYN | ACK) && ack == t->iss + 1) {
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->state = EST;
            (void)tcp_out(n, t, ACK, NULL, 0, t->snd_nxt);
        }
        return;
    }
    if (flags & ACK) {
        uint32_t a = ack - t->snd_una;
        if (a && a <= t->unacked + (uint32_t)(t->fin_sent ? 1 : 0)) {
            uint16_t d = a < t->unacked ? (uint16_t)a : t->unacked;
            if (d) {
                memmove(t->out, t->out + d, t->out_len - d);
                t->out_len -= d;
                t->unacked -= d;
                n->op.tcp_recved(n->op.arg, (unsigned)slot, d);
            }
            t->snd_una = ack;
            t->retries = 0;
            if (t->fin_sent && a > d) {
                t->state = DONE;
                t->closed_at = now;
            }
        }
    }
    if (dn) {
        if (t->in_len) {
        } else if (seq == t->rcv_nxt) {
            if (dn > MSS) {
                ++n->stat.drop;
                return;
            }
            memcpy(t->in, data, dn);
            t->in_len = dn;
            t->last_console = now;
            if (!try_write(n, (unsigned)slot, now))
                return;
        } else
            (void)tcp_out(n, t, ACK, NULL, 0, t->snd_nxt);
    }
    if (flags & FIN) {
        uint32_t fs = seq + (uint32_t)dn;
        if (!t->peer_fin && fs == t->rcv_nxt) {
            t->rcv_nxt++;
            t->peer_fin = true;
            (void)tcp_out(n, t, ACK, NULL, 0, t->snd_nxt);
            if (!t->in_len) {
                if (!t->local)
                    n->op.tcp_shutdown(n->op.arg, (unsigned)slot);
            } else if (!t->local)
                t->close_write = true;
            if (t->local) {
                if (t->speed_state == SPEED_DOWNLOAD)
                    t->out_len = t->unacked;
                t->sock_eof = true;
                t->eof_at = now;
                t->local_deadline = now + 2 * SPEED_TIMEOUT;
                t->local_deadline_set = true;
            }
        } else if (t->peer_fin && fs + 1 == t->rcv_nxt)
            (void)tcp_out(n, t, ACK, NULL, 0, t->snd_nxt);
    }
}
/* UDP slots preserve the original destination for reply source addresses;
 * DNS may be sent elsewhere, but replies still come from DNS to the console. */
static void in_udp(nat_t *n, uint32_t dip, const uint8_t *b, size_t len, uint32_t now) {
    if (len < 8 || rd16(b + 4) < 8 || rd16(b + 4) > len ||
        (rd16(b + 6) && pseudo(SNES, dip, UDP, b, rd16(b + 4)))) {
        ++n->stat.drop;
        return;
    }
    uint16_t sp = rd16(b), dp = rd16(b + 2), ln = rd16(b + 4);
    int s = -1;
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        if (n->udp[i].used && n->udp[i].sport == sp && n->udp[i].reply_from == dip &&
            n->udp[i].dport == dp)
            s = i;
    if (s < 0)
        for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
            if (!n->udp[i].used) {
                s = i;
                break;
            }
    if (s < 0) {
        ++n->stat.drop;
        return;
    }
    udp_t *u = &n->udp[s];
    if (!u->used)
        *u = (udp_t){.used = true, .sport = sp, .dport = dp, .reply_from = dip};
    u->touched = now;
    uint32_t target = dip;
    if (dip == DNS) {
        if (!n->resolver_count) {
            n->resolver[0] = 0x01010101u;
            n->resolver[1] = 0x08080808u;
            n->resolver_count = 2;
        }
        target = n->resolver[n->resolver_next++ % n->resolver_count];
    }
    if (!n->op.udp_send(n->op.arg, (unsigned)s, target, dp, b + 8, ln - 8))
        ++n->stat.drop;
}
/* Flows are keyed on the console's identifier alone, as slirp.py keys its
 * sockets: one flow carries that identifier's pings to every destination. */
static int icmp_flow(nat_t *n, uint16_t ident, uint32_t now) {
    int s = -1;
    for (unsigned i = 0; s < 0 && i < NAT_ICMP_MAX; ++i)
        if (n->icmp[i].used && n->icmp[i].ident == ident)
            s = (int)i;
    for (unsigned i = 0; s < 0 && i < NAT_ICMP_MAX; ++i)
        if (!n->icmp[i].used) {
            s = (int)i;
            n->icmp[i].used = true;
            n->icmp[i].ident = ident;
            n->icmp[i].wire = (uint16_t)(n->icmp[i].wire + NAT_ICMP_MAX);
        }
    if (s >= 0)
        n->icmp[s].touched = now;
    return s;
}
/* Echo requests to the gateway, the DNS front and the Pico's own address are
 * answered here.  lwIP has no loopback: forwarded, the last would go out on
 * WiFi addressed to the Pico, and nothing there answers it. */
static void in_icmp(nat_t *n, uint32_t dip, const uint8_t *b, size_t len, uint32_t now) {
    if (len < 8 || len + 20 > NAT_MTU || sum(b, len, 0) || b[0] != ECHO) {
        ++n->stat.drop;
        return;
    }
    uint8_t *r = udp_scratch;
    memcpy(r, b, len);
    if (dip == GW || dip == DNS || (n->local_address && dip == n->local_address)) {
        r[0] = ECHO_REPLY;
        wr16(r + 2, 0);
        wr16(r + 2, sum(r, len, 0));
        (void)emit(n, dip, ICMP, r, len);
        return;
    }
    int s = n->adapter.icmp_send ? icmp_flow(n, rd16(b + 4), now) : -1;
    if (s < 0) {
        ++n->stat.drop;
        return;
    }
    wr16(r + 4, n->icmp[s].wire);
    wr16(r + 2, 0);
    wr16(r + 2, sum(r, len, 0));
    if (!n->adapter.icmp_send(n->op.arg, dip, r, len))
        ++n->stat.drop;
}
size_t nat_core_size(void) {
    return sizeof(nat_t);
}
void nat_core_init(nat_t *n, const nat_ops_t *op) {
    memset(n, 0, sizeof(*n));
    n->op = *op;
    n->iss = 0x2000;
    n->next_eport = 40000;
    for (unsigned i = 0; i < NAT_ICMP_MAX; ++i)
        n->icmp[i].wire = (uint16_t)i;
}
void nat_core_set_resolver(nat_t *n, uint32_t address) {
    nat_core_set_resolvers(n, &address, 1);
}
void nat_core_set_resolvers(nat_t *n, const uint32_t *a, size_t count) {
    uint32_t resolvers[6];
    size_t valid = 0;
    if (count > 6)
        count = 6;
    for (size_t i = 0; i < count; ++i)
        if (a[i])
            resolvers[valid++] = a[i];
    if (n->resolver_count == valid && !memcmp(n->resolver, resolvers, valid * sizeof(*a)))
        return;
    memcpy(n->resolver, resolvers, valid * sizeof(*a));
    n->resolver_count = (uint8_t)valid;
    n->resolver_next = 0;
}
void nat_core_set_local_address(nat_t *n, uint32_t address) {
    n->local_address = address;
}
void nat_core_set_adapter(nat_t *n, bool (*emit_try)(void *, const uint8_t *, size_t),
                          bool (*link_busy)(void *), void (*udp_close)(void *, unsigned),
                          bool (*tcp_queued)(void *, unsigned)) {
    n->adapter = (adapter_t){emit_try, link_busy, udp_close, tcp_queued, n->adapter.icmp_send};
}
void nat_core_set_icmp(nat_t *n, bool (*send)(void *, uint32_t, const uint8_t *, size_t)) {
    n->adapter.icmp_send = send;
}
void nat_core_reset(nat_t *n) {
    nat_ops_t op = n->op;
    adapter_t adapter = n->adapter;
    uint16_t wire[NAT_ICMP_MAX];
    for (unsigned i = 0; i < NAT_ICMP_MAX; ++i)
        wire[i] = n->icmp[i].wire;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->tcp[i].state != FREE)
            op.tcp_abort(op.arg, i);
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->fwd[i].used)
            op.forward_del(op.arg, n->fwd[i].port);
    memset(n, 0, sizeof(*n));
    n->op = op;
    n->adapter = adapter;
    n->iss = 0x2000;
    n->next_eport = 40000;
    for (unsigned i = 0; i < NAT_ICMP_MAX; ++i)
        n->icmp[i].wire = wire[i];
}
void nat_core_drop(nat_t *n) {
    ++n->stat.drop;
}
void nat_core_input(nat_t *n, const uint8_t *p, size_t len, uint32_t now) {
    if (len < 20 || (p[0] >> 4) != 4) {
        ++n->stat.drop;
        return;
    }
    size_t h = (p[0] & 15) * 4, total = rd16(p + 2);
    if (h < 20 || total < h || total > len || sum(p, h, 0) || rd32(p + 12) != SNES) {
        ++n->stat.drop;
        return;
    }
    ++n->stat.in;
    switch (p[9]) {
    case ICMP:
        in_icmp(n, rd32(p + 16), p + h, total - h, now);
        break;
    case UDP:
        in_udp(n, rd32(p + 16), p + h, total - h, now);
        break;
    case TCP:
        in_tcp(n, rd32(p + 16), p + h, total - h, now);
        break;
    default:
        ++n->stat.drop;
    }
}
void nat_core_tcp_connected(nat_t *n, unsigned i, int error, uint32_t now) {
    if (i >= NAT_TCP_MAX || n->tcp[i].state != CONNECTING)
        return;
    if (error) {
        tcp_reset(n, i);
        return;
    }
    tcp_t *t = &n->tcp[i];
    t->state = SYN_RCVD;
    t->unsent = !tcp_out(n, t, SYN | ACK, NULL, 0, t->iss);
    t->snd_nxt = t->iss + 1;
    t->sent_at = now;
}
bool nat_core_tcp_received(nat_t *n, unsigned i, const uint8_t *d, size_t len, uint32_t now) {
    if (i >= NAT_TCP_MAX || (n->tcp[i].state != EST && n->tcp[i].state != SYN_RCVD) ||
        len > TCP_RX || n->tcp[i].out_len)
        return false;
    tcp_t *t = &n->tcp[i];
    memcpy(t->out, d, len);
    t->out_len = (uint16_t)len;
    t->touched = now;
    return true;
}
void nat_core_tcp_eof(nat_t *n, unsigned i, uint32_t now) {
    if (i < NAT_TCP_MAX && n->tcp[i].state != FREE) {
        n->tcp[i].sock_eof = true;
        n->tcp[i].eof_at = now;
    }
}
void nat_core_tcp_error(nat_t *n, unsigned i) {
    if (i < NAT_TCP_MAX && n->tcp[i].state != FREE)
        tcp_reset(n, i);
}
void nat_core_udp_received(nat_t *n, unsigned i, const uint8_t *d, size_t len, uint32_t now) {
    if (i >= NAT_UDP_MAX || !n->udp[i].used)
        return;
    if (len > NAT_MTU - 28) {
        ++n->stat.drop;
        return;
    }
    udp_t *u = &n->udp[i];
    uint8_t *b = udp_scratch;
    wr16(b, u->dport);
    wr16(b + 2, u->sport);
    wr16(b + 4, 8 + len);
    wr16(b + 6, 0);
    memcpy(b + 8, d, len);
    uint16_t c = pseudo(u->reply_from, SNES, UDP, b, 8 + len);
    wr16(b + 6, c ? c : 0xffff);
    u->touched = now;
    (void)emit(n, u->reply_from, UDP, b, 8 + len);
}
/* Only echo replies arrive here, still carrying the NAT's identifier.  One
 * that matches no live flow is late, not lost, so it is ignored uncounted. */
void nat_core_icmp_received(nat_t *n, uint32_t from, const uint8_t *d, size_t len,
                            uint32_t now) {
    if (!d || len < 8 || len + 20 > NAT_MTU || d[0] != ECHO_REPLY) {
        ++n->stat.drop;
        return;
    }
    uint16_t wire = rd16(d + 4);
    icmp_t *f = &n->icmp[wire % NAT_ICMP_MAX];
    if (!f->used || f->wire != wire)
        return;
    if (sum(d, len, 0)) {
        ++n->stat.drop;
        return;
    }
    uint8_t *r = udp_scratch;
    memcpy(r, d, len);
    wr16(r + 4, f->ident);
    wr16(r + 2, 0);
    wr16(r + 2, sum(r, len, 0));
    f->touched = now;
    (void)emit(n, from, ICMP, r, len);
}
/* Reserve before handing a new accepted PCB to the poll path. DONE slots do
 * not count against slirp.py's per-port accept cap during their TIME_WAIT. */
int nat_core_inbound_reserve(nat_t *n, uint16_t port, uint32_t now) {
    unsigned live = 0;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->tcp[i].state != FREE && n->tcp[i].inbound && n->tcp[i].sport == port &&
            n->tcp[i].state != DONE)
            ++live;
    if (live >= INBOUND_CAP)
        return -1;
    int i = free_tcp(n);
    if (i < 0)
        return -1;
    tcp_t *t = &n->tcp[i];
    memset(t, 0, sizeof(*t));
    t->state = SYN_SENT;
    t->inbound = true;
    t->sport = port;
    t->dport = n->next_eport++;
    if (n->next_eport >= 60000)
        n->next_eport = 40000;
    t->dip = GW;
    t->iss = (n->iss += 0x1000);
    t->snd_nxt = t->snd_una = t->iss;
    t->sent_at = t->touched = now;
    return i;
}
void nat_core_inbound_start(nat_t *n, unsigned i, uint32_t now) {
    if (i >= NAT_TCP_MAX || n->tcp[i].state != SYN_SENT)
        return;
    tcp_t *t = &n->tcp[i];
    t->unsent = !tcp_out(n, t, SYN, NULL, 0, t->iss);
    t->snd_nxt++;
    t->sent_at = now;
}
int nat_core_inbound_accept(nat_t *n, uint16_t port, uint32_t now) {
    int i = nat_core_inbound_reserve(n, port, now);
    if (i >= 0)
        nat_core_inbound_start(n, (unsigned)i, now);
    return i;
}
bool nat_core_forward_add(nat_t *n, uint16_t port, uint8_t ch) {
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->fwd[i].used && n->fwd[i].port == port) {
            n->fwd[i].owners |= (uint8_t)(1u << (ch & 7));
            return true;
        }
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (!n->fwd[i].used) {
            if (!n->op.forward_add(n->op.arg, port))
                return false;
            n->fwd[i] = (fwd_t){true, port, (uint8_t)(1u << (ch & 7))};
            count(n);
            return true;
        }
    return false;
}
void nat_core_forward_del(nat_t *n, uint16_t port, uint8_t ch) {
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (n->fwd[i].used && n->fwd[i].port == port) {
            n->fwd[i].owners &= (uint8_t) ~(1u << (ch & 7));
            if (!n->fwd[i].owners) {
                n->op.forward_del(n->op.arg, port);
                n->fwd[i].used = false;
                count(n);
            }
            return;
        }
}
/* A busy link defers handshake retries without moving their deadline.  This
 * lets the next clear output opportunity serve an already-due flow. */
static void poll_handshake(nat_t *n, unsigned i, uint32_t now) {
    tcp_t *t = &n->tcp[i];
    if (busy(n)) {
        if (!t->unsent)
            defer_retry(t, now);
        return;
    }
    if (!t->unsent && !expired(now, t->sent_at, RTO))
        return;
    if (!t->unsent && t->retries >= MAX_RETRIES) {
        tcp_reset(n, i);
        return;
    }
    if (tcp_out(n, t, t->state == SYN_RCVD ? (SYN | ACK) : SYN, NULL, 0, t->iss)) {
        if (!t->unsent)
            ++t->retries;
        t->unsent = false;
        t->sent_at = now;
        t->retry_busy_early = false;
        tcp_served(n, i);
    }
}

/* Keep this else-if order: slirp.py sends new data before retrying it, FINs
 * only after output drains, and applies FIN_GRACE only to inbound flows. */
static void poll_tcp_data(nat_t *n, unsigned i, uint32_t now) {
    tcp_t *t = &n->tcp[i];
    if (!try_write(n, i, now))
        return;
    if (t->local && speed_poll(t, now)) {
        tcp_reset(n, i);
        return;
    }
    if (t->out_len && t->unacked == 0) {
        size_t send_len = t->out_len > MSS ? MSS : t->out_len;
        if (t->local && t->peer_wnd < send_len)
            send_len = t->peer_wnd;
        if (send_len && t->peer_wnd >= send_len && !busy(n) &&
            tcp_out(n, t, PSH | ACK, t->out, send_len, t->snd_una)) {
            t->unacked = (uint16_t)send_len;
            t->snd_nxt = t->snd_una + (uint32_t)send_len;
            t->sent_at = now;
            t->retries = 0;
            t->retry_busy_early = false;
            if (t->local && t->speed_state == SPEED_REPORT && send_len == t->out_len) {
                t->speed_deadline = now + SPEED_TIMEOUT;
                t->speed_deadline_set = true;
            }
            tcp_served(n, i);
        }
    } else if (t->unacked) {
        if (busy(n)) {
            defer_retry(t, now);
        } else if (expired(now, t->sent_at, RTO)) {
            if (t->retries >= DATA_RETRIES) {
                tcp_reset(n, i);
                return;
            }
            if (tcp_out(n, t, PSH | ACK, t->out, t->unacked, t->snd_una)) {
                ++t->retries;
                t->sent_at = now;
                t->retry_busy_early = false;
                tcp_served(n, i);
            }
        }
    } else if (t->sock_eof && !t->fin_sent && !t->out_len &&
               (!t->inbound || expired(now, t->eof_at > t->last_console ?
                                         t->eof_at : t->last_console, FIN_GRACE)) &&
               tcp_out(n, t, FIN | ACK, NULL, 0, t->snd_nxt)) {
        t->fin_sent = true;
        t->snd_nxt++;
        t->sent_at = now;
        t->retries = 0;
        t->retry_busy_early = false;
        tcp_served(n, i);
    } else if (t->fin_sent) {
        if (busy(n)) {
            defer_retry(t, now);
        } else if (expired(now, t->sent_at, RTO)) {
            if (t->retries >= MAX_RETRIES) {
                close_tcp(n, i);
                return;
            }
            if (tcp_out(n, t, FIN | ACK, NULL, 0, t->snd_nxt - 1)) {
                ++t->retries;
                t->sent_at = now;
                t->retry_busy_early = false;
                tcp_served(n, i);
            }
        }
    }
}

/* TIME_WAIT reserves a completed 4-tuple; check it before the normal idle
 * timer so that its dedicated slirp.py lifetime is never shortened. */
static void poll_tcp_slot(nat_t *n, unsigned i, uint32_t now) {
    tcp_t *t = &n->tcp[i];
    if (t->state == FREE)
        return;
    if (t->local && t->local_deadline_set && reached(now, t->local_deadline)) {
        tcp_reset(n, i);
        return;
    }
    if (t->state == DONE) {
        if (expired(now, t->closed_at, TIME_WAIT))
            close_tcp(n, i);
        return;
    }
    if (expired(now, t->touched, TCP_IDLE)) {
        close_tcp(n, i);
        return;
    }
    if (t->state == SYN_RCVD || t->state == SYN_SENT) {
        poll_handshake(n, i, now);
        return;
    }
    poll_tcp_data(n, i, now);
}

static void poll_udp(nat_t *n, uint32_t now) {
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        if (n->udp[i].used && expired(now, n->udp[i].touched, UDP_IDLE)) {
            if (n->adapter.udp_close)
                n->adapter.udp_close(n->op.arg, i);
            n->udp[i].used = false;
        }
}

static void poll_icmp(nat_t *n, uint32_t now) {
    for (unsigned i = 0; i < NAT_ICMP_MAX; ++i)
        if (n->icmp[i].used && expired(now, n->icmp[i].touched, ICMP_IDLE))
            n->icmp[i].used = false;
}

void nat_core_poll(nat_t *n, uint32_t now) {
    unsigned start = n->tcp_next;
    for (unsigned offset = 0; offset < NAT_TCP_MAX; ++offset) {
        unsigned i = (start + offset) % NAT_TCP_MAX;
        poll_tcp_slot(n, i, now);
    }
    poll_udp(n, now);
    poll_icmp(n, now);
    count(n);
}
nat_stats_t nat_core_stats(const nat_t *n) {
    return n->stat;
}
bool nat_core_udp_used(const nat_t *n, unsigned i) {
    return i < NAT_UDP_MAX && n->udp[i].used;
}
