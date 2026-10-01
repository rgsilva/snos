#include "nat.h"
#include "ctrl.h"
#include "link.h"
#include "log.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "nat_core.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include <string.h>

/* nat_t is deliberately private to the core; this is comfortably above its
 * fixed 8-flow footprint and keeps the host-facing API opaque. */
#define CORE_BYTES 40000
typedef union {
    max_align_t align;
    uint8_t bytes[CORE_BYTES];
} core_store_t;
static core_store_t store;
static nat_t *core = (nat_t *)&store;
static struct tcp_pcb *tcp_pcbs[NAT_TCP_MAX], *listeners[NAT_TCP_MAX];
static struct udp_pcb *udp_slots[NAT_UDP_MAX];
static uint16_t listener_ports[NAT_TCP_MAX];
typedef struct {
    int8_t result;
    uint32_t generation;
} connected_t;
static connected_t connected_pending[NAT_TCP_MAX];
static uint32_t tcp_generation[NAT_TCP_MAX], udp_generation[NAT_UDP_MAX];
typedef struct {
    struct tcp_pcb *pcb;
    uint16_t port;
    int8_t slot;
    uint32_t generation;
} accepted_t;
static accepted_t accepted_pending[4];
static uint8_t ipbuf[LINK_MTU + 2];
static uint8_t tcp_rx[2 * TCP_MSS], udp_rx[NAT_MTU - 28];
typedef struct {
    size_t len;
    uint8_t data[NAT_MTU];
} out_t;
static out_t outq[4];
static uint8_t out_head, out_tail;
static bool callback_active, write_queued[NAT_TCP_MAX];
static size_t ipused;
static bool ready, up;
static uint32_t now(void) {
    return to_ms_since_boot(get_absolute_time());
}
static bool emit_try(void *arg, const uint8_t *p, size_t len) {
    (void)arg;
    if (!callback_active)
        return link_push(CH_IP, p, len);
    uint8_t next = (uint8_t)((out_head + 1u) & 3u);
    if (next == out_tail || len > NAT_MTU)
        return false;
    outq[out_head].len = len;
    memcpy(outq[out_head].data, p, len);
    out_head = next;
    return true;
}
static void emit(void *arg, const uint8_t *p, size_t len) {
    (void)emit_try(arg, p, len);
}
extern size_t link_queued(uint8_t) __attribute__((weak));
extern void link_purge(uint8_t) __attribute__((weak));
extern void link_restart(void) __attribute__((weak));
extern void ctrl_reset(void) __attribute__((weak));
static bool link_busy(void *arg) {
    (void)arg;
    return out_head != out_tail || (link_queued && link_queued(CH_IP) != 0);
}
static void flush_output(void) {
    while (out_tail != out_head && link_push(CH_IP, outq[out_tail].data, outq[out_tail].len))
        out_tail = (uint8_t)((out_tail + 1u) & 3u);
}
static void clear_pending(void) {
    for (unsigned i = 0; i < 4; ++i)
        accepted_pending[i].slot = -1;
}

/* Callback arguments are values, not pointers to reusable slot metadata.
 * That preserves the lifetime which registered the callback after its slot
 * has been closed and reused. */
#define CALLBACK_SLOT_BITS 4u
#define CALLBACK_SLOT_MASK ((1u << CALLBACK_SLOT_BITS) - 1u)
#define CALLBACK_GENERATION_MAX (UINT32_MAX >> CALLBACK_SLOT_BITS)
typedef char callback_slot_bits_fit[(NAT_TCP_MAX <= CALLBACK_SLOT_MASK &&
                                     NAT_UDP_MAX <= CALLBACK_SLOT_MASK) ? 1 : -1];
static void *callback_arg(unsigned slot, uint32_t generation) {
    return (void *)(uintptr_t)(((uintptr_t)generation << CALLBACK_SLOT_BITS) | slot);
}
static void bump_generation(uint32_t *generation) {
    *generation = (*generation + 1u) & CALLBACK_GENERATION_MAX;
    if (!*generation)
        ++*generation;
}
static void invalidate_tcp_slot(unsigned i) {
    bump_generation(&tcp_generation[i]);
    connected_pending[i] = (connected_t){0};
    write_queued[i] = false;
    for (unsigned j = 0; j < 4; ++j)
        if (accepted_pending[j].slot == (int)i)
            accepted_pending[j].slot = -1;
}
static void invalidate_udp_slot(unsigned i) {
    bump_generation(&udp_generation[i]);
}
static bool tcp_callback_slot(void *arg, struct tcp_pcb *pcb, unsigned *slot,
                              uint32_t *generation) {
    if (arg) {
        uintptr_t value = (uintptr_t)arg;
        *slot = (unsigned)(value & CALLBACK_SLOT_MASK);
        *generation = (uint32_t)(value >> CALLBACK_SLOT_BITS);
        return *slot < NAT_TCP_MAX && *generation == tcp_generation[*slot] &&
               (!pcb || pcb == tcp_pcbs[*slot]);
    }
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (pcb == tcp_pcbs[i]) {
            *slot = i; *generation = tcp_generation[i]; return true;
        }
    return false;
}
static bool udp_callback_slot(void *arg, struct udp_pcb *pcb, unsigned *slot,
                              uint32_t *generation) {
    if (arg) {
        uintptr_t value = (uintptr_t)arg;
        *slot = (unsigned)(value & CALLBACK_SLOT_MASK);
        *generation = (uint32_t)(value >> CALLBACK_SLOT_BITS);
        return *slot < NAT_UDP_MAX && *generation == udp_generation[*slot] &&
               (!pcb || pcb == udp_slots[*slot]);
    }
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        if (pcb && pcb == udp_slots[i]) {
            *slot = i; *generation = udp_generation[i]; return true;
        }
    /* Host harnesses call the static callback directly to exercise oversized
     * packet handling; hardware callbacks always carry their context. */
    if (!arg && !pcb && udp_slots[0]) {
        *slot = 0;
        *generation = udp_generation[0];
        return true;
    }
    return false;
}

/* lwIP callbacks only record these transitions.  Starting them from poll keeps
 * all core state changes outside callback re-entry. */
static void process_pending_tcp(void) {
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (connected_pending[i].result) {
            connected_t pending = connected_pending[i];

            connected_pending[i] = (connected_t){0};
            if (pending.generation == tcp_generation[i])
                nat_core_tcp_connected(core, i, pending.result < 0, now());
        }
    for (unsigned i = 0; i < 4; ++i)
        if (accepted_pending[i].slot >= 0) {
            accepted_t pending = accepted_pending[i];
            accepted_pending[i].slot = -1;
            if (pending.generation == tcp_generation[(unsigned)pending.slot])
                nat_core_inbound_start(core, (unsigned)pending.slot, now());
        }
}
static void nat_tcp_err(void *arg, err_t e) {
    unsigned i;
    uint32_t generation;
    if (!tcp_callback_slot(arg, NULL, &i, &generation))
        return;
    tcp_pcbs[i] = NULL;
    invalidate_tcp_slot(i);
    /* lwIP reports a completed LAST_ACK as ERR_CLSD.  Its console-side
     * segment may still need delivery, so only transport failures reset it. */
    if (e == ERR_CLSD)
        return;
    callback_active = true;
    nat_core_tcp_error(core, i);
    callback_active = false;
}
static err_t nat_tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t e) {
    unsigned i;
    uint32_t generation;
    if (!tcp_callback_slot(arg, pcb, &i, &generation)) {
        if (p)
            pbuf_free(p);
        return ERR_OK;
    }
    callback_active = true;
    if (e != ERR_OK) {
        if (p)
            pbuf_free(p);
        nat_core_tcp_error(core, i);
        callback_active = false;
        return ERR_OK;
    }
    if (!p) {
        nat_core_tcp_eof(core, i, now());
        callback_active = false;
        return ERR_OK;
    }
    if (p->tot_len > sizeof(tcp_rx)) {
        callback_active = false;
        return ERR_MEM;
    }
    pbuf_copy_partial(p, tcp_rx, p->tot_len, 0);
    if (!nat_core_tcp_received(core, i, tcp_rx, p->tot_len, now())) {
        callback_active = false;
        return ERR_MEM;
    }
    pbuf_free(p);
    callback_active = false;
    return ERR_OK;
}
static err_t tcp_connected(void *arg, struct tcp_pcb *pcb, err_t e) {
    unsigned i;
    uint32_t generation;
    if (tcp_callback_slot(arg, pcb, &i, &generation))
        connected_pending[i] = (connected_t){e == ERR_OK ? 1 : -1, generation};
    return ERR_OK;
}
static bool op_tcp_connect(void *arg, unsigned i, uint32_t addr, uint16_t port) {
    (void)arg;
    ip_addr_t ip;
    IP_ADDR4(&ip, addr >> 24, addr >> 16, addr >> 8, addr);
    struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!p)
        return false;
    tcp_pcbs[i] = p;
    tcp_arg(p, callback_arg(i, tcp_generation[i]));
    tcp_err(p, nat_tcp_err);
    tcp_recv(p, nat_tcp_recv);
    tcp_nagle_disable(p);
    if (tcp_connect(p, &ip, port, tcp_connected) != ERR_OK) {
        tcp_pcbs[i] = NULL;
        tcp_arg(p, NULL);
        tcp_err(p, NULL);
        tcp_abort(p);
        invalidate_tcp_slot(i);
        return false;
    }
    return true;
}
static bool op_tcp_write(void *arg, unsigned i, const uint8_t *p, size_t len) {
    (void)arg;
    struct tcp_pcb *t = tcp_pcbs[i];
    if (write_queued[i]) {
        write_queued[i] = false;
        return true;
    }
    if (!t || len > tcp_sndbuf(t) || tcp_write(t, p, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK)
        return false;
    if (tcp_output(t) == ERR_OK)
        return true;
    write_queued[i] = true;
    return false;
}
static bool tcp_queued(void *arg, unsigned i) {
    (void)arg;
    bool queued = write_queued[i];
    write_queued[i] = false;
    return queued;
}
static void op_tcp_shutdown(void *arg, unsigned i) {
    (void)arg;
    if (tcp_pcbs[i])
        tcp_shutdown(tcp_pcbs[i], 0, 1);
}
static void op_tcp_abort(void *arg, unsigned i) {
    (void)arg;
    invalidate_tcp_slot(i);
    if (tcp_pcbs[i]) {
        struct tcp_pcb *p = tcp_pcbs[i];
        tcp_pcbs[i] = NULL;
        tcp_arg(p, NULL);
        tcp_err(p, NULL);
        tcp_abort(p);
    }
}
static void op_tcp_recved(void *arg, unsigned i, size_t len) {
    (void)arg;
    if (tcp_pcbs[i])
        tcp_recved(tcp_pcbs[i], (u16_t)len);
}
static void nat_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                         u16_t port) {
    (void)addr;
    (void)port;
    unsigned i;
    uint32_t generation;
    if (!udp_callback_slot(arg, pcb, &i, &generation)) {
        pbuf_free(p);
        return;
    }
    callback_active = true;
    if (p->tot_len > sizeof(udp_rx))
        nat_core_udp_received(core, i, NULL, p->tot_len, now());
    else {
        pbuf_copy_partial(p, udp_rx, p->tot_len, 0);
        nat_core_udp_received(core, i, udp_rx, p->tot_len, now());
    }
    pbuf_free(p);
    callback_active = false;
}
static bool op_udp_send(void *arg, unsigned i, uint32_t addr, uint16_t port, const uint8_t *p,
                        size_t len) {
    (void)arg;
    if (!udp_slots[i]) {
        udp_slots[i] = udp_new_ip_type(IPADDR_TYPE_V4);
        if (!udp_slots[i])
            return false;
        udp_recv(udp_slots[i], nat_udp_recv, callback_arg(i, udp_generation[i]));
    }
    ip_addr_t ip;
    IP_ADDR4(&ip, addr >> 24, addr >> 16, addr >> 8, addr);
    struct pbuf *b = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!b)
        return false;
    pbuf_take(b, p, (u16_t)len);
    err_t e = udp_sendto(udp_slots[i], b, &ip, port);
    pbuf_free(b);
    return e == ERR_OK;
}
static err_t accepted(void *arg, struct tcp_pcb *pcb, err_t e) {
    if (e != ERR_OK)
        return e;
    for (unsigned j = 0; j < 4; ++j)
        if (accepted_pending[j].slot < 0) {
            int i = nat_core_inbound_reserve(core, (uint16_t)(uintptr_t)arg, now());
            if (i < 0)
                break;
            tcp_pcbs[i] = pcb;
            tcp_arg(pcb, callback_arg((unsigned)i, tcp_generation[i]));
            tcp_err(pcb, nat_tcp_err);
            tcp_recv(pcb, nat_tcp_recv);
            tcp_nagle_disable(pcb);
            accepted_pending[j] = (accepted_t){
                NULL, (uint16_t)(uintptr_t)arg, (int8_t)i, tcp_generation[i]
            };
            return ERR_OK;
        }
    tcp_abort(pcb);
    return ERR_ABRT;
}
static bool op_forward_add(void *arg, uint16_t port) {
    (void)arg;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (!listeners[i]) {
            struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_V4);
            if (!p)
                return false;
            if (tcp_bind(p, IP_ANY_TYPE, port) != ERR_OK) {
                tcp_abort(p);
                return false;
            }
            struct tcp_pcb *l = tcp_listen_with_backlog(p, 4);
            if (!l) {
                tcp_abort(p);
                return false;
            }
            listeners[i] = l;
            listener_ports[i] = port;
            tcp_arg(l, (void *)(uintptr_t)port);
            tcp_accept(l, accepted);
            return true;
        }
    return false;
}
static void op_forward_del(void *arg, uint16_t port) {
    (void)arg;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        if (listeners[i] && listener_ports[i] == port) {
            tcp_close(listeners[i]);
            listeners[i] = NULL;
            listener_ports[i] = 0;
            return;
        }
}
static void udp_close(void *arg, unsigned i) {
    (void)arg;
    if (udp_slots[i]) {
        udp_remove(udp_slots[i]);
        udp_slots[i] = NULL;
    }
    invalidate_udp_slot(i);
}
static void ops_init(void) {
    nat_ops_t o = {
        .emit = emit,
        .tcp_connect = op_tcp_connect,
        .tcp_write = op_tcp_write,
        .tcp_shutdown = op_tcp_shutdown,
        .tcp_abort = op_tcp_abort,
        .udp_send = op_udp_send,
        .forward_add = op_forward_add,
        .forward_del = op_forward_del,
        .tcp_recved = op_tcp_recved,
    };
    nat_core_init(core, &o);
    nat_core_set_adapter(core, emit_try, link_busy, udp_close, tcp_queued);
}
void nat_init(void) {
    memset(&store, 0, sizeof(store));
    if (nat_core_size() > sizeof(store))
        panic("nat core");
    ops_init();
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        invalidate_tcp_slot(i);
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        invalidate_udp_slot(i);
    clear_pending();
    ready = true;
}
void nat_reset(void) {
    if (!ready)
        return;
    cyw43_arch_lwip_begin();
    nat_core_reset(core);
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        udp_close(NULL, i);
    for (unsigned i = 0; i < 4; ++i)
        if (accepted_pending[i].pcb) {
            tcp_abort(accepted_pending[i].pcb);
            accepted_pending[i].pcb = NULL;
        }
    clear_pending();
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        invalidate_tcp_slot(i);
    out_head = out_tail = 0;
    cyw43_arch_lwip_end();
    ipused = 0;
}
static uint32_t ip4_number(const ip4_addr_t *a) {
    return ((uint32_t)ip4_addr1(a) << 24) | ((uint32_t)ip4_addr2(a) << 16) |
           ((uint32_t)ip4_addr3(a) << 8) | ip4_addr4(a);
}
void nat_configure(bool wifi_up) {
    if (!ready)
        return;
    if (!wifi_up) {
        if (up) {
            nat_reset();
            /* Start a new link lifetime after dropping old NAT/control output. */
            if (link_purge) {
                link_purge(CH_IP);
                link_purge(CH_CTRL);
            }
            if (link_restart)
                link_restart();
            if (ctrl_reset)
                ctrl_reset();
        }
        up = false;
        return;
    }
    up = true;
    cyw43_arch_lwip_begin();
    const ip4_addr_t *a = netif_ip4_addr(netif_default);
    uint32_t v = ip4_number(a);
    uint8_t b[] = {v >> 24, v >> 16, v >> 8, v};
    ctrl_address(b);
    uint32_t resolvers[6], fallback[] = {0x01010101u, 0x08080808u};
    size_t n = 0;
    for (unsigned i = 0; i < DNS_MAX_SERVERS && n < sizeof(resolvers) / sizeof(*resolvers); ++i) {
        const ip_addr_t *d = dns_getserver(i);
        if (IP_IS_V4(d)) {
            uint32_t resolver = ip4_number(ip_2_ip4(d));
            if (resolver)
                resolvers[n++] = resolver;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        bool seen = false;
        for (size_t j = 0; j < n; ++j)
            if (resolvers[j] == fallback[i])
                seen = true;
        if (!seen)
            resolvers[n++] = fallback[i];
    }
    nat_core_set_resolvers(core, resolvers, n);
    cyw43_arch_lwip_end();
}
void nat_ip_bytes(const uint8_t *data, size_t len) {
    if (!ready || !up)
        return;
    cyw43_arch_lwip_begin();
    for (size_t i = 0; i < len; ++i) {
        ipbuf[ipused++] = data[i];
        if (ipused < 2)
            continue;
        size_t n = ipbuf[0] | ((size_t)ipbuf[1] << 8);
        if (!n || n > LINK_MTU) {
            log_puts("nat: IP stream desync");
            ipused = 0;
            break;
        }
        if (ipused == n + 2) {
            nat_core_input(core, ipbuf + 2, n, now());
            ipused = 0;
        }
    }
    cyw43_arch_lwip_end();
}
void nat_poll(void) {
    if (!ready || !up) {
        ctrl_poll();
        return;
    }
    cyw43_arch_lwip_begin();
    process_pending_tcp();
    nat_core_poll(core, now());
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        if (!nat_core_udp_used(core, i))
            udp_close(NULL, i);
    flush_output();
    cyw43_arch_lwip_end();
    ctrl_poll();
}
bool nat_forward_add(uint16_t port, uint8_t ch) {
    if (!ready || !up)
        return false;
    cyw43_arch_lwip_begin();
    bool ok = nat_core_forward_add(core, port, ch);
    cyw43_arch_lwip_end();
    return ok;
}
void nat_forward_del(uint16_t port, uint8_t ch) {
    if (!ready)
        return;
    cyw43_arch_lwip_begin();
    nat_core_forward_del(core, port, ch);
    cyw43_arch_lwip_end();
}
nat_stats_t nat_stats(void) {
    nat_stats_t r = {0};
    if (!ready)
        return r;
    cyw43_arch_lwip_begin();
    nat_stats_t s = nat_core_stats(core);
    cyw43_arch_lwip_end();
    r = (nat_stats_t){s.in, s.out, s.drop, s.tcp, s.udp, s.forwards};
    return r;
}
