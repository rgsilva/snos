/* Include the real adapter to drive its private raw callbacks; no implementation edits. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/inet_chksum.h"
#include "../nat.c"

static uint32_t clock_ms = 10;
static unsigned locks, outputs;
static bool output_fail, in_callback, callback_emit;
static uint8_t packet[NAT_MTU], hello_address[4];
static size_t packet_len;
static struct netif test_netif;
uint32_t get_absolute_time(void) { return clock_ms; }
u32_t sys_now(void) { return clock_ms; }
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t value) { (void)value; }
void cyw43_arch_lwip_begin(void) { ++locks; }
void cyw43_arch_lwip_end(void) { assert(locks); --locks; }
int log_event(const char *s, ...) { (void)s; return 0; }
void ctrl_poll(void) {}
void ctrl_address(const uint8_t *ip) { memcpy(hello_address, ip, 4); }
bool link_push(uint8_t ch, const uint8_t *p, size_t len) {
    assert(ch == CH_IP && len <= sizeof(packet));
    callback_emit |= in_callback;
    ++outputs; packet_len = len; memcpy(packet, p, len); return true;
}
static err_t net_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ip) {
    (void)netif; (void)p; (void)ip; return output_fail ? ERR_MEM : ERR_OK;
}
static err_t net_init(struct netif *netif) {
    netif->output = net_output; netif->mtu = 1500; netif->name[0] = 't'; netif->name[1] = 's';
    return ERR_OK;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t checksum(const uint8_t *p, size_t len, uint32_t s) {
    while (len > 1) { s += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) s += *p << 8;
    while (s >> 16) s = (s & 65535) + (s >> 16);
    return ~s;
}
static void console_tcp_to(uint32_t destination, uint16_t port, unsigned flags,
                           uint32_t ack) {
    uint8_t p[40] = {0}; p[0] = 0x45; put16(p + 2, sizeof(p)); p[8] = 64; p[9] = 6;
    put32(p + 12, 0x0a00020f); put32(p + 16, destination);
    put16(p + 10, checksum(p, 20, 0));
    put16(p + 20, 5000); put16(p + 22, port); put32(p + 24, flags == 2 ? 100 : 101);
    put32(p + 28, ack); p[32] = 0x50; p[33] = flags; put16(p + 34, 1460);
    put16(p + 36, checksum(p + 20, 20, 0x0a00 + 0x020f + (destination >> 16) +
                            (destination & 0xffffu) + 6 + 20));
    cyw43_arch_lwip_begin(); nat_core_input(core, p, sizeof(p), clock_ms); cyw43_arch_lwip_end();
}
static void console_tcp(unsigned flags, uint32_t ack) {
    console_tcp_to(0xc0000201, 80, flags, ack);
}
static struct tcp_pcb *established(void) {
    console_tcp(2, 0); assert(tcp_pcbs[0]);
    struct tcp_pcb *pcb = tcp_pcbs[0]; pcb->state = ESTABLISHED;
    pcb->snd_wnd = 65535; pcb->cwnd = 65535;
    tcp_connected(NULL, pcb, ERR_OK); nat_poll();
    assert(packet[33] == 0x12);
    console_tcp(16, get32(packet + 24) + 1);
    return pcb;
}
static void t_bug_receive_payload(void) {
    struct tcp_pcb *pcb = established();
    const char data[] = "real bytes from the internet";
    struct pbuf *p = pbuf_alloc(PBUF_RAW, sizeof(data), PBUF_RAM); assert(p);
    assert(pbuf_take(p, data, sizeof(data)) == ERR_OK);
    assert(nat_tcp_recv(NULL, pcb, p, ERR_OK) == ERR_OK);
    nat_poll();
    assert(packet_len == 40 + sizeof(data));
    assert(!memcmp(packet + 40, data, sizeof(data)));
}
static void t_bug_receive_coalesced_pbuf(void) {
    struct tcp_pcb *pcb = established();
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 2000, PBUF_POOL); assert(p);
    assert(p->next); /* A real chained pbuf, within the advertised 2920-byte window. */
    unsigned before = outputs;
    for (unsigned retry = 0; retry < 3; ++retry) {
        err_t e = nat_tcp_recv(NULL, pcb, p, ERR_OK);
        if (e == ERR_OK) { p = NULL; break; }
        assert(e == ERR_MEM); nat_poll();
    }
    nat_poll(); if (p) pbuf_free(p);
    assert(outputs > before);
}
static void t_bug_address_byte_order(void) {
    nat_configure(true);
    const uint8_t expected[] = {192, 0, 2, 10};
    assert(!memcmp(hello_address, expected, sizeof(expected)));
}
static void t_speedtest_uses_configured_address(void) {
    console_tcp_to(0xc000020a, 7778, 2, 0);
    assert(!tcp_pcbs[0] && outputs == 1 && packet[33] == (2 | 16));
    assert(get32(packet + 12) == 0xc000020au);
    assert(get32(packet + 16) == 0x0a00020fu);
    nat_poll();
    assert(nat_stats().speed == 1);
}
static void t_bug_dns_byte_order(void) {
    nat_configure(true);
    /* Feed a DNS-front query and inspect the real lwIP IP destination. */
    uint8_t p[29] = {0}; p[0] = 0x45; put16(p + 2, sizeof(p)); p[8] = 64; p[9] = 17;
    put32(p + 12, 0x0a00020f); put32(p + 16, 0x0a000203);
    put16(p + 10, checksum(p, 20, 0));
    put16(p + 20, 4444); put16(p + 22, 53); put16(p + 24, 9); p[28] = 1;
    /* Observe resolver as handed to the pure core using a probe op. */
    extern bool capture_dns(void *, unsigned, uint32_t, uint16_t, const uint8_t *, size_t);
    nat_ops_t ops = {0}; ops.udp_send = capture_dns;
    nat_core_init(core, &ops); nat_configure(true);
    nat_core_input(core, p, sizeof(p), clock_ms);
}
bool capture_dns(void *arg, unsigned slot, uint32_t ip, uint16_t port, const uint8_t *data, size_t len) {
    (void)arg; (void)slot; (void)port; (void)data; (void)len;
    assert(ip == 0xc0000235u); return true;
}
static void t_bug_write_output_failure_duplicates(void) {
    struct tcp_pcb *pcb = established();
    uint32_t before = pcb->snd_lbb;
    output_fail = true;
    assert(!op_tcp_write(NULL, 0, (const uint8_t *)"abc", 3));
    output_fail = false;
    assert(op_tcp_write(NULL, 0, (const uint8_t *)"abc", 3));
    assert(pcb->snd_lbb - before == 3); /* tcp_write already queued the first copy. */
}
static void t_bug_reset_listener(void) {
    assert(nat_forward_add(8080, 1)); nat_reset();
    assert(nat_forward_add(8080, 1));
}
static void t_bug_mode_disable_releases_pcbs(void) {
    established(); assert(nat_forward_add(8080, 1));
    nat_configure(false);
    assert(tcp_pcbs[0] == NULL && listeners[0] == NULL);
}
static void t_bug_accepted_data_before_poll(void) {
    struct tcp_pcb *pcb = tcp_new(); assert(pcb); pcb->state = ESTABLISHED;
    assert(accepted((void *)(uintptr_t)8080, pcb, ERR_OK) == ERR_OK);
    /* Deliver data in the same background burst, before nat_poll installs callbacks. */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 3, PBUF_RAM); assert(p);
    pbuf_take(p, "abc", 3); pbuf_ref(p);
    pcb->rcv_wnd -= 3; tcpwnd_size_t before = pcb->rcv_wnd;
    err_t e = pcb->recv(pcb->callback_arg, pcb, p, ERR_OK);
    assert(e == ERR_OK || e == ERR_MEM);
    assert(pcb->rcv_wnd == before); /* No console ACK has happened. */
    pbuf_free(p);
    if (e == ERR_MEM) pbuf_free(p);
}
static void t_bug_accepted_error_before_poll(void) {
    struct tcp_pcb *pcb = tcp_new(); assert(pcb); pcb->state = ESTABLISHED;
    assert(accepted((void *)(uintptr_t)8080, pcb, ERR_OK) == ERR_OK);
    tcp_abort(pcb); /* Incoming RST frees it before foreground consumes the queue. */
    assert(accepted_pending[0].pcb == NULL);
}
static void t_bug_listener_allocation_failure(void) {
    struct tcp_pcb *held[MEMP_NUM_TCP_PCB_LISTEN];
    for (unsigned i = 0; i < MEMP_NUM_TCP_PCB_LISTEN; ++i) {
        struct tcp_pcb *p = tcp_new(); assert(p);
        assert(tcp_bind(p, IP_ANY_TYPE, 10000 + i) == ERR_OK);
        held[i] = tcp_listen(p); assert(held[i]);
    }
    assert(!nat_forward_add(8080, 1));
    struct tcp_pcb *probe = tcp_new(); assert(probe);
    assert(tcp_bind(probe, IP_ANY_TYPE, 8080) == ERR_OK); /* Failed listen must release its original PCB. */
}
static void create_udp_flow(void) {
    /* Allocate a core UDP slot; op_udp_send itself uses the real lwIP allocator. */
    nat_ops_t ops = {0}; ops.emit = emit; ops.udp_send = op_udp_send;
    nat_core_init(core, &ops);
    uint8_t p[29] = {0}; p[0] = 0x45; put16(p + 2, 29); p[8] = 64; p[9] = 17;
    put32(p + 12, 0x0a00020f); put32(p + 16, 0xc0000235);
    put16(p + 10, checksum(p, 20, 0)); put16(p + 20, 4444); put16(p + 22, 53); put16(p + 24, 9);
    nat_core_input(core, p, sizeof(p), clock_ms);
}
static void t_bug_udp_callback_enters_link_queue(void) {
    create_udp_flow();
    struct pbuf *reply = pbuf_alloc(PBUF_RAW, 3, PBUF_RAM); assert(reply);
    assert(pbuf_take(reply, "dns", 3) == ERR_OK);
    in_callback = true; nat_udp_recv(NULL, udp_slots[0], reply, NULL, 53); in_callback = false;
    assert(!callback_emit); /* link_next_out and keyboard enqueues have no lwIP lock. */
}
static void t_tcp_error_clears_freed_pcb(void) {
    struct tcp_pcb *pcb = established();
    tcp_abort(pcb); /* lwIP frees the PCB, then invokes the registered error callback. */
    assert(tcp_pcbs[0] == NULL);
    nat_poll(); assert(nat_stats().tcp == 0);
}
static void t_pbuf_ownership_and_receive_credit(void) {
    struct tcp_pcb *pcb = established();
    struct pbuf *first = pbuf_alloc(PBUF_RAW, 3, PBUF_RAM); assert(first);
    pbuf_take(first, "abc", 3); pbuf_ref(first);
    pcb->rcv_wnd -= 3; tcpwnd_size_t before = pcb->rcv_wnd;
    assert(nat_tcp_recv(NULL, pcb, first, ERR_OK) == ERR_OK);
    assert(first->ref == 1 && pcb->rcv_wnd == before); pbuf_free(first);
    struct pbuf *next = pbuf_alloc(PBUF_RAW, 3, PBUF_RAM); assert(next);
    pbuf_take(next, "def", 3);
    assert(nat_tcp_recv(NULL, pcb, next, ERR_OK) == ERR_MEM);
    assert(next->ref == 1 && pcb->rcv_wnd == before); pbuf_free(next);
    nat_poll(); uint32_t seq = get32(packet + 24);
    console_tcp(16, seq + 3); assert(pcb->rcv_wnd == before + 3);
}
static void t_bug_connect_segment_exhaustion(void) {
    void *segments[MEMP_NUM_TCP_SEG];
    for (unsigned i = 0; i < MEMP_NUM_TCP_SEG; ++i) {
        segments[i] = memp_malloc(MEMP_TCP_SEG); assert(segments[i]);
    }
    console_tcp(2, 0);
    assert(outputs == 1 && (packet[33] & 4));
    assert(get32(packet + 12) == 0xc0000201u && get32(packet + 28) == 101);
}
static void t_bug_udp_pool_capacity(void) {
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        assert(op_udp_send(NULL, i, 0xc0000201, 53, (const uint8_t *)"q", 1));
}
static void t_bug_udp_oversize_accounting(void) {
    create_udp_flow();
    unsigned before = nat_stats().drop;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 3000, PBUF_RAM); assert(p);
    nat_udp_recv(NULL, NULL, p, NULL, 53);
    assert(nat_stats().drop == before + 1);
}
static void t_bug_udp_idle_releases_pcbs(void) {
    create_udp_flow(); assert(udp_slots[0]);
    clock_ms += 60001; nat_poll();
    assert(nat_stats().udp == 0 && udp_slots[0] == NULL);
}
static void t_pbuf_pool_exhaustion(void) {
    struct pbuf *held[PBUF_POOL_SIZE];
    for (unsigned i = 0; i < PBUF_POOL_SIZE; ++i) {
        held[i] = pbuf_alloc(PBUF_RAW, 1, PBUF_POOL); assert(held[i]);
    }
    assert(!pbuf_alloc(PBUF_RAW, 1, PBUF_POOL));
    for (unsigned i = 0; i < PBUF_POOL_SIZE; ++i) pbuf_free(held[i]);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 1, PBUF_POOL); assert(p); pbuf_free(p);
}
static void t_heap_exhaustion_retry(void) {
    struct tcp_pcb *pcb = established();
    void *held[MEM_SIZE / 128]; unsigned count = 0;
    while (count < sizeof(held) / sizeof(*held) && (held[count] = mem_malloc(128))) ++count;
    uint8_t data[1460] = {0}; uint32_t before = pcb->snd_lbb;
    assert(!op_tcp_write(NULL, 0, data, sizeof(data)) && pcb->snd_lbb == before);
    for (unsigned i = 0; i < count; ++i) mem_free(held[i]);
    assert(op_tcp_write(NULL, 0, data, sizeof(data)) && pcb->snd_lbb - before == sizeof(data));
}
/* External ICMP through real lwIP: raw_sendto leaves on the fake WiFi netif,
 * replies enter through ip_input and reach the console only from nat_poll. */
#define PICO_IP 0xc000020au
#define HOST_IP 0xac100101u
static uint8_t wifi[NAT_MTU];
static size_t wifi_len;
static unsigned wifi_sent;
static err_t wifi_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ip) {
    (void)netif; (void)ip; assert(p->tot_len <= sizeof(wifi));
    wifi_len = pbuf_copy_partial(p, wifi, p->tot_len, 0); ++wifi_sent; return ERR_OK;
}
static void console_ping(uint32_t destination, uint16_t id, uint16_t seq) {
    uint8_t p[31] = {0}; p[0] = 0x45; put16(p + 2, sizeof(p)); p[8] = 64; p[9] = 1;
    put32(p + 12, 0x0a00020f); put32(p + 16, destination); put16(p + 10, checksum(p, 20, 0));
    p[20] = 8; put16(p + 24, id); put16(p + 26, seq); memcpy(p + 28, "odd", 3);
    put16(p + 22, checksum(p + 20, 11, 0));
    cyw43_arch_lwip_begin(); nat_core_input(core, p, sizeof(p), clock_ms); cyw43_arch_lwip_end();
}
/* Deliver an ICMP message to the Pico's address as the WiFi driver would. */
static void wifi_icmp(uint32_t from, const uint8_t *icmp, size_t len) {
    uint8_t p[NAT_MTU] = {0}; p[0] = 0x45; put16(p + 2, 20 + len); p[8] = 64; p[9] = 1;
    put32(p + 12, from); put32(p + 16, PICO_IP); put16(p + 10, checksum(p, 20, 0));
    memcpy(p + 20, icmp, len);
    struct pbuf *b = pbuf_alloc(PBUF_RAW, (u16_t)(20 + len), PBUF_POOL); assert(b);
    assert(pbuf_take(b, p, (u16_t)(20 + len)) == ERR_OK);
    in_callback = true; assert(test_netif.input(b, &test_netif) == ERR_OK); in_callback = false;
}
/* The far host's answer to the request lwIP last put on WiFi. */
static void wifi_pong(void) {
    uint8_t r[NAT_MTU]; size_t len = wifi_len - 20;
    memcpy(r, wifi + 20, len); r[0] = 0; put16(r + 2, 0); put16(r + 2, checksum(r, len, 0));
    wifi_icmp(get32(wifi + 16), r, len);
}
static void assert_pool_free(void) {
    struct pbuf *held[PBUF_POOL_SIZE];
    for (unsigned i = 0; i < PBUF_POOL_SIZE; ++i) {
        held[i] = pbuf_alloc(PBUF_RAW, 1, PBUF_POOL); assert(held[i]);
    }
    for (unsigned i = 0; i < PBUF_POOL_SIZE; ++i) pbuf_free(held[i]);
}
static void t_icmp_echo_through_raw_pcb(void) {
    test_netif.output = wifi_output;
    console_ping(HOST_IP, 0x534e, 7);
    assert(wifi_sent == 1 && wifi_len == 31 && wifi[9] == 1 && icmp_pcb);
    assert(get32(wifi + 12) == PICO_IP && get32(wifi + 16) == HOST_IP);
    assert(checksum(wifi, 20, 0) == 0 && checksum(wifi + 20, 11, 0) == 0);
    assert(wifi[20] == 8 && wifi[26] == 0 && wifi[27] == 7 && !memcmp(wifi + 28, "odd", 3));
    unsigned before = outputs;
    wifi_pong();
    assert(!callback_emit && outputs == before); /* the callback only queued it */
    nat_poll();
    assert(outputs == before + 1 && packet_len == 31 && packet[9] == 1);
    assert(get32(packet + 12) == HOST_IP && get32(packet + 16) == 0x0a00020fu);
    assert(checksum(packet, 20, 0) == 0 && checksum(packet + 20, 11, 0) == 0);
    assert(packet[20] == 0 && packet[24] == 0x53 && packet[25] == 0x4e);
    assert(packet[26] == 0 && packet[27] == 7 && !memcmp(packet + 28, "odd", 3));
    assert(nat_stats().icmp == 1);
    assert_pool_free();
}
static void t_icmp_lwip_keeps_its_own(void) {
    test_netif.output = wifi_output;
    console_ping(HOST_IP, 0x534e, 1); nat_poll(); assert(icmp_pcb);
    /* A LAN host pinging the Pico is still answered by lwIP itself. */
    uint8_t request[11] = {8, 0, 0, 0, 0x12, 0x34, 0, 1, 'l', 'a', 'n'};
    put16(request + 2, checksum(request, sizeof(request), 0));
    unsigned sent = wifi_sent, before = outputs;
    wifi_icmp(0xc0000277u, request, sizeof(request));
    assert(wifi_sent == sent + 1 && wifi[20] == 0 && get32(wifi + 16) == 0xc0000277u);
    /* ICMP errors are not mapped back to the console: they are dropped. */
    uint8_t unreachable[36] = {3, 1};
    put16(unreachable + 2, checksum(unreachable, sizeof(unreachable), 0));
    wifi_icmp(0xc0000201u, unreachable, sizeof(unreachable));
    nat_poll();
    assert(outputs == before && !callback_emit);
    assert_pool_free();
}
static void t_icmp_teardown(void) {
    test_netif.output = wifi_output;
    console_ping(HOST_IP, 0x534e, 1); wifi_pong(); /* a reply still queued */
    nat_reset();
    assert(!icmp_pcb && icmp_head == icmp_tail);
    unsigned before = outputs; nat_poll(); assert(outputs == before);
    assert_pool_free();
    console_ping(HOST_IP, 0x534e, 2); wifi_pong(); assert(icmp_pcb);
    nat_configure(false); /* WiFi loss and the switch to relay mode */
    assert(!icmp_pcb && icmp_head == icmp_tail);
    assert_pool_free();
    nat_configure(true);
    console_ping(HOST_IP, 0x534e, 3); nat_poll(); assert(icmp_pcb);
    clock_ms += 15001; nat_poll();
    assert(!icmp_pcb && nat_stats().icmp == 0);
}
static void t_icmp_reply_queue_overflow(void) {
    test_netif.output = wifi_output;
    console_ping(HOST_IP, 0x534e, 1);
    for (unsigned i = 0; i < 4; ++i) wifi_pong();
    unsigned before = outputs, drops = nat_stats().drop;
    nat_poll();
    assert(outputs == before + 3 && nat_stats().drop == drops + 1);
    assert_pool_free();
}

#define TEST(name) {#name, name}
static const struct { const char *name; void (*run)(void); } tests[] = {
    TEST(t_tcp_error_clears_freed_pcb), TEST(t_pbuf_pool_exhaustion), TEST(t_heap_exhaustion_retry),
    TEST(t_bug_connect_segment_exhaustion), TEST(t_bug_udp_pool_capacity),
    TEST(t_bug_udp_idle_releases_pcbs), TEST(t_bug_udp_oversize_accounting), TEST(t_pbuf_ownership_and_receive_credit), TEST(t_bug_receive_payload),
    TEST(t_bug_receive_coalesced_pbuf), TEST(t_bug_address_byte_order), TEST(t_bug_dns_byte_order),
    TEST(t_speedtest_uses_configured_address),
    TEST(t_bug_write_output_failure_duplicates), TEST(t_bug_reset_listener),
    TEST(t_bug_mode_disable_releases_pcbs), TEST(t_bug_accepted_data_before_poll),
    TEST(t_bug_accepted_error_before_poll), TEST(t_bug_listener_allocation_failure),
    TEST(t_bug_udp_callback_enters_link_queue),
    TEST(t_icmp_echo_through_raw_pcb), TEST(t_icmp_lwip_keeps_its_own), TEST(t_icmp_teardown),
    TEST(t_icmp_reply_queue_overflow)
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); ++i) puts(tests[i].name);
        return 0;
    }
    lwip_init(); ip4_addr_t ip, mask, gw; IP4_ADDR(&ip, 192, 0, 2, 10);
    IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 192, 0, 2, 1);
    assert(netif_add(&test_netif, &ip, &mask, &gw, NULL, net_init, ip_input));
    netif_set_default(&test_netif); netif_set_up(&test_netif); netif_set_link_up(&test_netif);
    ip_addr_t resolver; IP_ADDR4(&resolver, 192, 0, 2, 53); dns_setserver(0, &resolver);
    nat_init(); nat_configure(true);
    for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); ++i) if (!strcmp(argv[1], tests[i].name)) {
        tests[i].run(); assert(locks == 0); printf("PASS %s\n", tests[i].name); return 0;
    }
    return 2;
}
