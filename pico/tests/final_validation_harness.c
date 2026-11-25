/* Final validation: production configuration cadence, offline stream bytes,
 * peer reset, reconnect queues, and callback scheduling around teardown.
 * Production NAT, LINK, ctrl, relay, SDK lwIP and main's console are compiled.
 * Only clock/netif/IRQ boundaries and the TCP handshake are host fixtures. */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/priv/tcp_priv.h"
#include "../nat.c"
#include "../relay.h"

static uint32_t clock_ms = 10;
static unsigned locks, resets;
static void (*before_lock)(void), (*on_unlock)(void);
static struct netif test_netif;
static bool relay_mode;
static uint8_t received[8192];
static size_t received_len;
static const uint8_t relay_address[] = {198, 51, 100, 7};
uint32_t get_absolute_time(void) { return clock_ms; }
u32_t sys_now(void) { return clock_ms; }
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t value) { (void)value; }
void cyw43_arch_lwip_begin(void) {
    if (!locks && before_lock) {
        void (*run)(void) = before_lock; before_lock = NULL;
        ++locks; run(); --locks;
    }
    ++locks;
}
void cyw43_arch_lwip_end(void) {
    assert(locks);
    /* SDK background work can run on the outermost lock release. */
    if (locks == 1 && on_unlock) {
        void (*run)(void) = on_unlock; on_unlock = NULL; run();
    }
    --locks;
}
int log_event(const char *s, ...) { (void)s; return 0; }
static err_t net_output(struct netif *n, struct pbuf *p, const ip4_addr_t *ip) {
    (void)n; (void)p; (void)ip; return ERR_OK;
}
static err_t net_init(struct netif *n) {
    n->output = net_output; n->mtu = 1500; n->name[0] = 'f'; n->name[1] = 'v';
    return ERR_OK;
}
void console_peer_init(void);
void console_peer_receive(const uint8_t *, uint8_t *);
void console_peer_enable_ip(void);
int console_peer_ip_read(uint8_t *, int);
int console_peer_ip_write(const uint8_t *, int);
int console_peer_control_write(const uint8_t *, unsigned);
unsigned console_peer_hellos(void);
const char *console_peer_address(void);
unsigned console_peer_keys(void);
const uint8_t *console_peer_key_values(void);
void final_relay_connect(void);
void final_relay_receive(uint8_t, const uint8_t *, size_t);
void final_relay_eof(void);
size_t final_relay_output(uint8_t *, size_t);

/* Exact mode/configuration ordering of main.c wifi_poll: called repeatedly. */
static void configure(bool relay, bool wifi_up) {
    relay_mode = relay;
    if (relay) {
        nat_configure(false); ctrl_set_forwarder(NULL, NULL);
        relay_configure("192.0.2.20", wifi_up);
    } else {
        relay_configure("", false); nat_configure(wifi_up);
        ctrl_set_forwarder(nat_forward_add, nat_forward_del);
    }
}
static void exchange(void) {
    uint8_t f[LINK_FRAME], reply[LINK_FRAME], bytes[2048];
    link_next_out(f); console_peer_receive(f, reply);
    int n;
    while ((n = console_peer_ip_read(bytes, sizeof(bytes))) != 0) {
        if (n < 0) continue;
        assert(received_len + n <= sizeof(received));
        memcpy(received + received_len, bytes, n); received_len += n;
    }
    link_rx_t r = link_on_rx(reply); assert(r.valid);
    if (r.reset) { ++resets; ctrl_reset(); relay_reset(); nat_reset(); }
    for (size_t i = 0; i < r.count; ++i) {
        link_record_t *p = r.records + i;
        if (p->ch == CH_CTRL) ctrl_bytes(p->data, p->len);
        else if (p->ch == CH_IP) {
            if (relay_mode) relay_ip_bytes(p->data, p->len);
            else nat_ip_bytes(p->data, p->len);
        }
    }
}
static void pump(unsigned count) { while (count--) exchange(); }
static void setup_peer(void) {
    console_peer_init(); pump(2);
    assert(link_push(CH_KBD, (const uint8_t *)"!", 1)); pump(3);
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2)); pump(5);
    configure(false, true); ctrl_poll(); pump(3);
    console_peer_enable_ip(); received_len = 0; resets = 0;
}
static void put16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static uint16_t sum(const uint8_t *p, size_t n) {
    unsigned s = 0;
    while (n > 1) { s += (p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) s += *p << 8;
    while (s >> 16) s = (s & 65535) + (s >> 16);
    return ~s;
}
static void stream(uint8_t *s, size_t len, uint8_t tag) {
    s[0] = len; s[1] = len >> 8;
    uint8_t *p = s + 2; memset(p, tag, len);
    p[0] = 0x45; p[1] = 0; put16(p + 2, len); p[6] = p[7] = 0;
    p[8] = 64; p[9] = 1; p[10] = p[11] = 0;
    memcpy(p + 12, "\x0a\0\x02\x0f\x0a\0\x02\x02", 8);
    put16(p + 10, sum(p, 20));
    p[20] = 8; p[21] = p[22] = p[23] = 0;
    put16(p + 22, sum(p + 20, len - 20));
}
static void send_bytes(const uint8_t *p, size_t n) {
    assert(console_peer_ip_write(p, n) == (int)n); pump(8);
}
static void assert_echo(const uint8_t *s, size_t n) {
    nat_poll(); pump(16);
    fprintf(stderr, "echo bytes=%zu expected=%zu; assembler=%zu\n", received_len, n, ipused);
    assert(received_len == n);
    assert(received[22] == 0 && sum(received + 22, n - 22) == 0);
    assert(!memcmp(received + 26, s + 26, n - 26));
}
static void start_relay(void) {
    configure(true, true); final_relay_connect();
    final_relay_receive(5, relay_address, 4);
}
static void hello(bool repeat_configure) {
    setup_peer(); start_relay();
    unsigned before = console_peer_hellos();
    /* A normal idle foreground iteration after the server's address record. */
    if (repeat_configure) configure(true, true);
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    for (unsigned i = 0; i < 12; ++i) {
        exchange();
        if (repeat_configure) configure(true, true);
        relay_poll();
    }
    fprintf(stderr, "HELLO replies=%u expected=1\n", console_peer_hellos() - before);
    assert(console_peer_hellos() == before + 1);
    assert(!strcmp(console_peer_address(), "198.51.100.7"));
}
static void t_relay_hello_control(void) { hello(false); }
static void t_relay_hello_after_foreground_configure(void) { hello(true); }
static void t_relay_to_nat_prefix_and_suffix_span_wifi_rejoin(void) {
    setup_peer(); start_relay(); uint8_t s[102]; stream(s, 100, 37);
    send_bytes(s, 31); assert(ipused == 31);
    /* mode nat / wifi_changed leaves WiFi down while LINK keeps ACKing. */
    configure(false, false); send_bytes(s + 31, 31);
    configure(false, true); send_bytes(s + 62, sizeof(s) - 62);
    assert_echo(s, sizeof(s));
}
static void t_offline_completion_does_not_poison_next_datagram(void) {
    setup_peer(); uint8_t old[102], fresh[42];
    stream(old, 100, 37); stream(fresh, 40, 93);
    send_bytes(old, 31); configure(false, false);
    send_bytes(old + 31, sizeof(old) - 31);
    configure(false, true); send_bytes(fresh, sizeof(fresh));
    assert_echo(fresh, sizeof(fresh));
}
static void peer_reset(bool relay, bool partial) {
    setup_peer();
    if (relay) start_relay();
    uint8_t old[102], fresh[42]; stream(old, 100, 37); stream(fresh, 40, 93);
    if (partial) { send_bytes(old, 31); assert(ipused == 31); }
    /* Genuine console reboot sends a real SYN through actual net_service. */
    console_peer_init(); pump(6); assert(resets == 1);
    console_peer_enable_ip();
    configure(relay, true); send_bytes(fresh, sizeof(fresh));
    if (!relay) assert_echo(fresh, sizeof(fresh));
    else {
        uint8_t output[1503]; size_t n = final_relay_output(output, sizeof(output));
        fprintf(stderr, "relay bytes=%zu expected=43; assembler=%zu\n", n, ipused);
        assert(n == sizeof(fresh) + 1 && output[0] == 1);
        assert(!memcmp(output + 1, fresh, sizeof(fresh)));
    }
}
static void t_nat_echo_control(void) {
    setup_peer(); uint8_t s[102]; stream(s, 100, 37);
    send_bytes(s, sizeof(s)); assert_echo(s, sizeof(s));
}
static void t_relay_to_nat_rejoin_without_offline_bytes(void) {
    setup_peer(); start_relay(); uint8_t s[102]; stream(s, 100, 37);
    send_bytes(s, 31); configure(false, false); configure(false, true);
    send_bytes(s + 31, sizeof(s) - 31); assert_echo(s, sizeof(s));
}
static void t_console_reboot_without_partial_stream(void) { peer_reset(false, false); }
static void t_console_reboot_clears_nat_stream(void) { peer_reset(false, true); }
static void t_console_reboot_clears_relay_stream(void) { peer_reset(true, true); }
static void reconnect_output(bool started, bool change_host) {
    setup_peer(); start_relay();
    uint8_t old[102], stale[42], fresh[42];
    stream(old, 100, 37); stream(stale, 40, 61); stream(fresh, 40, 93);
    if (started) {
        final_relay_receive(1, old + 2, sizeof(old) - 2);
        exchange(); assert(received_len == 58);
    }
    final_relay_receive(1, stale + 2, sizeof(stale) - 2);
    if (change_host) relay_configure("192.0.2.21", true);
    else final_relay_eof();
    final_relay_connect(); final_relay_receive(5, relay_address, 4);
    final_relay_receive(1, fresh + 2, sizeof(fresh) - 2); pump(16);
    size_t prefix = started ? sizeof(old) : 0;
    fprintf(stderr, "reconnect bytes=%zu expected=%zu\n", received_len, prefix + sizeof(fresh));
    assert(received_len == prefix + sizeof(fresh));
    if (started) assert(!memcmp(received, old, sizeof(old)));
    assert(!memcmp(received + prefix, fresh, sizeof(fresh)));
}
static void t_relay_host_change_preserves_started_purges_unstarted(void) { reconnect_output(true, true); }
static void t_relay_reconnect_purges_unstarted(void) { reconnect_output(false, false); }
static void t_relay_reconnect_preserves_started_purges_unstarted(void) { reconnect_output(true, false); }

static void *saved_udp_arg;
static struct udp_pcb *saved_udp_pcb;
static void deliver_udp(void) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 3, PBUF_RAM); assert(p);
    assert(pbuf_take(p, "dns", 3) == ERR_OK);
    nat_udp_recv(saved_udp_arg, saved_udp_pcb, p, NULL, 53);
}
static void callback_at_teardown(bool after) {
    setup_peer();
    uint8_t s[31] = {29, 0, 0x45};
    uint8_t *p = s + 2; put16(p + 2, 29); p[8] = 64; p[9] = 17;
    memcpy(p + 12, "\x0a\0\x02\x0f\xc0\0\x02\x35", 8);
    put16(p + 10, sum(p, 20)); put16(p + 20, 4444); put16(p + 22, 53);
    put16(p + 24, 9); p[28] = 1; nat_ip_bytes(s, sizeof(s));
    assert(udp_slots[0]); saved_udp_pcb = udp_slots[0]; saved_udp_arg = saved_udp_pcb->recv_arg;
    if (after) on_unlock = deliver_udp; else before_lock = deliver_udp;
    nat_configure(false);
    assert(!before_lock && !on_unlock && !udp_slots[0]);
    assert(out_head == out_tail && !link_queued(CH_IP));
    nat_configure(true); nat_poll(); pump(6);
    assert(received_len == 0);
}
static void t_callback_before_teardown_is_purged(void) { callback_at_teardown(false); }
static void t_callback_after_teardown_is_rejected(void) { callback_at_teardown(true); }
static void t_purge_preserves_started_chunk_after_priority_hello(void) {
    setup_peer(); uint8_t s[102], stale[42]; stream(s, 100, 37); stream(stale, 40, 61);
    assert(link_push(CH_IP, s + 2, 100)); exchange(); assert(received_len == 58);
    /* HELLO displaces IP from the queue head and clears the cached started flag. */
    const uint8_t hello[] = {0x84, 2, 2, 'x'};
    assert(link_push_hello(hello, sizeof(hello)));
    assert(link_push(CH_IP, stale + 2, 40));
    unsigned keys = console_peer_keys();
    assert(link_push(CH_KBD, (const uint8_t *)"abc", 3));
    configure(true, true); configure(false, true); pump(12);
    assert(received_len == sizeof(s) && !memcmp(received, s, sizeof(s)));
    assert(console_peer_keys() == keys + 3);
    assert(!memcmp(console_peer_key_values() + keys, "abc", 3));
}
#define TEST(name) {#name, name}
static const struct { const char *name; void (*run)(void); } cases[] = {
    TEST(t_nat_echo_control),
    TEST(t_relay_to_nat_rejoin_without_offline_bytes),
    TEST(t_console_reboot_without_partial_stream),
    TEST(t_relay_hello_control),
    TEST(t_relay_hello_after_foreground_configure),
    TEST(t_relay_to_nat_prefix_and_suffix_span_wifi_rejoin),
    TEST(t_offline_completion_does_not_poison_next_datagram),
    TEST(t_console_reboot_clears_nat_stream),
    TEST(t_console_reboot_clears_relay_stream),
    TEST(t_relay_host_change_preserves_started_purges_unstarted),
    TEST(t_relay_reconnect_purges_unstarted),
    TEST(t_relay_reconnect_preserves_started_purges_unstarted),
    TEST(t_callback_before_teardown_is_purged),
    TEST(t_callback_after_teardown_is_rejected),
    TEST(t_purge_preserves_started_chunk_after_priority_hello),
};
static void assertion_exit(int sig) { (void)sig; _Exit(1); }
int main(int argc, char **argv) {
    signal(SIGABRT, assertion_exit);
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) puts(cases[i].name);
        return 0;
    }
    lwip_init(); ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 192, 0, 2, 10); IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 192, 0, 2, 1);
    assert(netif_add(&test_netif, &ip, &mask, &gw, NULL, net_init, ip_input));
    netif_set_default(&test_netif); netif_set_up(&test_netif); netif_set_link_up(&test_netif);
    link_init(); ctrl_reset(); nat_init(); relay_init(); configure(false, true);
    for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) if (!strcmp(argv[1], cases[i].name)) {
        cases[i].run(); assert(!locks); printf("PASS %s\n", cases[i].name); return 0;
    }
    return 2;
}
