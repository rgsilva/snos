/* Real adapter + core + link + control + keyboard + SDK lwIP. No replaced NAT ops. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/priv/tcp_priv.h"
#include "kbd.h"
#include "../nat.c"
static uint32_t clock_ms = 10;
static unsigned locks;
static bool output_fail;
static struct netif test_netif;
static uint32_t udp_destinations[32];
static unsigned udp_outputs;
static uint8_t packet[NAT_MTU];
static size_t packet_len;
uint32_t get_absolute_time(void) { return clock_ms; }
u32_t sys_now(void) { return clock_ms; }
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t value) { (void)value; }
void cyw43_arch_lwip_begin(void) { ++locks; }
void cyw43_arch_lwip_end(void) { assert(locks); --locks; }
int log_puts(const char *s) { (void)s; return 0; }
bool relay_send(uint8_t type, const uint8_t *p, size_t len) {
    (void)type; (void)p; (void)len; return false;
}
static err_t net_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ip) {
    (void)netif;
    uint8_t header[20]; assert(pbuf_copy_partial(p, header, sizeof(header), 0) == sizeof(header));
    if (header[9] == 17) { assert(udp_outputs < 32); udp_destinations[udp_outputs++] = ip4_number(ip); }
    return output_fail ? ERR_MEM : ERR_OK;
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
static void input_ip(uint8_t *p, size_t len) {
    uint8_t framed[NAT_MTU + 2]; assert(len <= NAT_MTU);
    framed[0] = len; framed[1] = len >> 8; memcpy(framed + 2, p, len);
    nat_ip_bytes(framed, len + 2);
}
static void console_tcp(unsigned flags, uint32_t seq, uint32_t ack, const char *body) {
    uint8_t p[100] = {0}; size_t len = body ? strlen(body) : 0;
    p[0] = 0x45; put16(p + 2, 40 + len); p[8] = 64; p[9] = 6;
    put32(p + 12, 0x0a00020f); put32(p + 16, 0xc0000201);
    put16(p + 10, checksum(p, 20, 0));
    put16(p + 20, 5000); put16(p + 22, 80); put32(p + 24, seq);
    put32(p + 28, ack); p[32] = 0x50; p[33] = flags; put16(p + 34, 1460);
    if (len) memcpy(p + 40, body, len);
    put16(p + 36, checksum(p + 20, 20 + len, 0x0a00 + 0x020f + 0xc000 + 0x0201 + 6 + 20 + len));
    input_ip(p, 40 + len);
}
static void console_udp(uint32_t dest) {
    uint8_t p[29] = {0}; p[0] = 0x45; put16(p + 2, sizeof(p)); p[8] = 64; p[9] = 17;
    put32(p + 12, 0x0a00020f); put32(p + 16, dest); put16(p + 10, checksum(p, 20, 0));
    put16(p + 20, 4444); put16(p + 22, 53); put16(p + 24, 9); p[28] = 1;
    input_ip(p, sizeof(p));
}
/* Drain and ACK actual link frames, collecting the complete IP byte stream. */
static unsigned drain_ip(void) {
    uint8_t stream[16000]; size_t used = 0;
    while (link_queued(CH_IP) || link_queued(CH_CTRL) || link_queued(CH_KBD)) {
        uint8_t f[LINK_FRAME], ack[LINK_FRAME] = {0}; link_next_out(f);
        for (size_t o = LINK_HDR; o + 2 <= LINK_FRAME;) {
            uint8_t ch = f[o++], len = f[o++];
            if (ch == CH_PAD) break;
            assert(o + len <= LINK_FRAME);
            if (ch == CH_IP) { assert(used + len <= sizeof(stream)); memcpy(stream + used, f + o, len); used += len; }
            o += len;
        }
        ack[1] = f[0];
        for (size_t i = 0; i < sizeof(ack); ++i) if (i != 3) ack[3] += ack[i];
        assert(link_on_rx(ack).valid);
    }
    unsigned count = 0;
    for (size_t o = 0; o < used;) {
        assert(o + 2 <= used); size_t len = stream[o] | ((size_t)stream[o + 1] << 8); o += 2;
        assert(len <= NAT_MTU && o + len <= used);
        memcpy(packet, stream + o, len); packet_len = len; o += len; ++count;
        assert(checksum(packet, 20, 0) == 0);
    }
    return count;
}
static struct tcp_pcb *establish(void) {
    console_tcp(2, 100, 0, NULL); assert(tcp_pcbs[0]);
    struct tcp_pcb *pcb = tcp_pcbs[0]; pcb->state = ESTABLISHED;
    pcb->snd_wnd = pcb->cwnd = 65535;
    tcp_connected(NULL, pcb, ERR_OK); nat_poll();
    assert(drain_ip() == 1 && packet[33] == 0x12);
    console_tcp(16, 101, get32(packet + 24) + 1, NULL);
    return pcb;
}
static void receive_tcp(struct tcp_pcb *pcb, const char *data) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, strlen(data), PBUF_RAM); assert(p);
    pbuf_take(p, data, strlen(data)); cyw43_arch_lwip_begin();
    assert(nat_tcp_recv(NULL, pcb, p, ERR_OK) == ERR_OK); cyw43_arch_lwip_end();
}
static void udp_reply(void) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 3, PBUF_RAM); assert(p); pbuf_take(p, "dns", 3);
    cyw43_arch_lwip_begin(); nat_udp_recv(NULL, udp_slots[0], p, NULL, 53); cyw43_arch_lwip_end();
}
static void t_f9_reset_preserves_link_busy_deferral(void) {
    nat_reset(); struct tcp_pcb *pcb = establish();
    const uint8_t occupied[40] = {0}; assert(link_push(CH_IP, occupied, sizeof(occupied)));
    receive_tcp(pcb, "abc"); size_t before = link_queued(CH_IP); nat_poll();
    assert(link_queued(CH_IP) == before);
}
static void t_f9_reset_preserves_enqueue_failure_accounting(void) {
    nat_reset(); struct tcp_pcb *pcb = establish();
    for (unsigned i = 0; i < 32; ++i) assert(link_push(CH_KBD, (const uint8_t *)"x", 1));
    receive_tcp(pcb, "abc"); unsigned before = nat_stats().out; nat_poll();
    assert(nat_stats().out == before && nat_stats().drop == 1);
}
static void t_f10_mode_roundtrip_preserves_queued_write_credit(void) {
    establish(); assert(nat_forward_add(8080, 1)); console_udp(0xc0000235);
    nat_configure(false);
    assert(!tcp_pcbs[0] && !listeners[0] && !udp_slots[0]);
    nat_configure(true); struct tcp_pcb *pcb = establish();
    uint32_t ack = get32(packet + 24) + 1;
    output_fail = true; console_tcp(24, 101, ack, "abc"); output_fail = false;
    nat_poll(); drain_ip();
    assert(packet[33] == 16 && get32(packet + 28) == 104);
    /* A failed tcp_output must not leave a marker that swallows the next data. */
    uint32_t before = pcb->snd_lbb;
    console_tcp(24, 104, ack, "def"); nat_poll();
    assert(pcb->snd_lbb == before + 3);
}
static void t_f9_reset_then_failed_write_and_reuse_keeps_new_payload(void) {
    nat_reset(); establish();
    uint32_t ack = get32(packet + 24) + 1;
    output_fail = true; console_tcp(24, 101, ack, "old"); output_fail = false;
    /* Peer resets before the foreground retry consumes the queued-write marker. */
    console_tcp(4, 104, ack, NULL);
    struct tcp_pcb *pcb = establish(); ack = get32(packet + 24) + 1;
    uint32_t before = pcb->snd_lbb;
    console_tcp(24, 101, ack, "new"); nat_poll(); drain_ip();
    assert(packet[33] == 16 && get32(packet + 28) == 104);
    assert(pcb->snd_lbb == before + 3);
}
static void t_f10_mode_disable_discards_old_link_packets(void) {
    establish(); receive_tcp(tcp_pcbs[0], "old NAT response"); nat_poll();
    assert(link_queued(CH_IP)); nat_configure(false);
    assert(link_queued(CH_IP) == 0);
}
static void t_f10_mode_disable_invalidates_control_address(void) {
    nat_configure(false);
    const uint8_t hello[] = {4, 0}; ctrl_bytes(hello, sizeof(hello));
    assert(link_queued(CH_CTRL) == 0);
}
static void t_f18_dns_rotation_survives_foreground_configure(void) {
    ip_addr_t second; IP_ADDR4(&second, 192, 0, 2, 54); dns_setserver(1, &second);
    nat_configure(true); console_udp(0x0a000203);
    nat_configure(true); console_udp(0x0a000203);
    assert(udp_outputs == 2 && udp_destinations[0] == 0xc0000235);
    assert(udp_destinations[1] == 0xc0000236);
}
static void t_f18_dns_rotation_skips_unconfigured_servers(void) {
    nat_configure(true);
    console_udp(0x0a000203); console_udp(0x0a000203);
    assert(udp_outputs == 2 && udp_destinations[0] == 0xc0000235);
    assert(udp_destinations[1] == 0x01010101);
}
static void t_f11_callback_ring_overflow_counts_and_recovers(void) {
    console_udp(0xc0000235);
    for (unsigned i = 0; i < 4; ++i) udp_reply();
    assert(!link_queued(CH_IP) && nat_stats().drop == 1);
    nat_poll(); assert(drain_ip() == 3);
    udp_reply(); nat_poll(); assert(drain_ip() == 1);
}
static void t_f9_reset_retains_callback_ring_drop_accounting(void) {
    nat_reset(); t_f11_callback_ring_overflow_counts_and_recovers();
}
static void t_f11_keyboard_report_survives_nat_callback_handoff(void) {
    kbd_init(); console_udp(0xc0000235); udp_reply();
    const uint8_t key_a[8] = {0, 0, 4}; kbd_hid_boot_report(key_a); kbd_poll();
    assert(link_queued(CH_KBD) == 1); nat_poll();
    assert(drain_ip() == 1 && packet_len == 31 && !memcmp(packet + 28, "dns", 3));
}
/* Drive the real lwIP LAST_ACK -> closed path, which reports ERR_CLSD. */
static void t_f6_orderly_close_keeps_pending_console_payload(void) {
    struct tcp_pcb *pcb = establish();
    uint32_t console_ack = get32(packet + 24) + 1;
    receive_tcp(pcb, "last reply");
    pcb->state = CLOSE_WAIT; /* Internet FIN already delivered by lwIP. */
    cyw43_arch_lwip_begin();
    assert(nat_tcp_recv(NULL, pcb, NULL, ERR_OK) == ERR_OK);
    cyw43_arch_lwip_end();
    console_tcp(17, 101, console_ack, NULL);
    assert(pcb->state == LAST_ACK);
    uint8_t p[40] = {0}; p[0] = 0x45; put16(p + 2, 40); p[8] = 64; p[9] = 6;
    put32(p + 12, 0xc0000201); put32(p + 16, 0xc000020a);
    put16(p + 10, checksum(p, 20, 0));
    put16(p + 20, pcb->remote_port); put16(p + 22, pcb->local_port);
    put32(p + 24, pcb->rcv_nxt); put32(p + 28, pcb->snd_nxt);
    p[32] = 0x50; p[33] = 16; put16(p + 34, 65535);
    put16(p + 36, checksum(p + 20, 20, 0xc000 + 0x0201 + 0xc000 + 0x020a + 6 + 20));
    struct pbuf *incoming = pbuf_alloc(PBUF_RAW, sizeof(p), PBUF_RAM); assert(incoming);
    pbuf_take(incoming, p, sizeof(p));
    cyw43_arch_lwip_begin(); assert(ip_input(incoming, &test_netif) == ERR_OK); cyw43_arch_lwip_end();
    assert(tcp_pcbs[0] == NULL); /* The real stack completed the close. */
    nat_poll();
    assert(nat_stats().tcp == 1); /* Console still owns unacknowledged reply bytes. */
    drain_ip(); nat_poll(); assert(drain_ip() == 1);
    assert(packet_len == 50 && !memcmp(packet + 40, "last reply", 10));
}
static void t_f14_tcp_and_udp_pools_cover_advertised_capacity(void) {
    struct udp_pcb *dhcp = udp_new(), *dns = udp_new(); assert(dhcp && dns);
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i)
        assert(op_udp_send(NULL, i, 0xc0000201, 53, (const uint8_t *)"q", 1));
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) assert(nat_forward_add(8000 + i, i));
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) assert(op_tcp_connect(NULL, i, 0xc0000201, 80));
}
#define TEST(name) {#name, name}
static const struct { const char *name; void (*run)(void); } tests[] = {
    TEST(t_f9_reset_preserves_link_busy_deferral),
    TEST(t_f9_reset_preserves_enqueue_failure_accounting),
    TEST(t_f10_mode_roundtrip_preserves_queued_write_credit),
    TEST(t_f9_reset_then_failed_write_and_reuse_keeps_new_payload),
    TEST(t_f10_mode_disable_discards_old_link_packets),
    TEST(t_f10_mode_disable_invalidates_control_address),
    TEST(t_f18_dns_rotation_survives_foreground_configure),
    TEST(t_f18_dns_rotation_skips_unconfigured_servers),
    TEST(t_f11_callback_ring_overflow_counts_and_recovers),
    TEST(t_f9_reset_retains_callback_ring_drop_accounting),
    TEST(t_f11_keyboard_report_survives_nat_callback_handoff),
    TEST(t_f14_tcp_and_udp_pools_cover_advertised_capacity),
    TEST(t_f6_orderly_close_keeps_pending_console_payload),
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(tests)/sizeof(*tests); ++i) puts(tests[i].name);
        return 0;
    }
    lwip_init(); ip4_addr_t ip, mask, gw; IP4_ADDR(&ip, 192, 0, 2, 10);
    IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 192, 0, 2, 1);
    assert(netif_add(&test_netif, &ip, &mask, &gw, NULL, net_init, ip_input));
    netif_set_default(&test_netif); netif_set_up(&test_netif); netif_set_link_up(&test_netif);
    ip_addr_t resolver; IP_ADDR4(&resolver, 192, 0, 2, 53); dns_setserver(0, &resolver);
    link_init(); ctrl_reset(); nat_init(); nat_configure(true);
    for (size_t i = 0; i < sizeof(tests)/sizeof(*tests); ++i) if (!strcmp(argv[1], tests[i].name)) {
        tests[i].run(); assert(!locks); printf("PASS %s\n", tests[i].name); return 0;
    }
    return 2;
}
