/* Lifecycle and generation checks through the real nat.c/lwIP adapter. */
#define main original_lwip_harness_main
#include "nat_lwip_harness.c"
#undef main

static uint32_t local_open(void) {
    console_tcp_to(0xc000020a, 7778, 2, 0);
    assert(packet[33] == (2 | 16) && !tcp_pcbs[0]);
    uint32_t ack = get32(packet + 24) + 1;
    console_tcp_to(0xc000020a, 7778, 16, ack);
    nat_poll();
    assert(nat_stats().speed == 1);
    return ack;
}
static void v_reset_local(void) {
    local_open();
    uint32_t generation = tcp_generation[0];
    nat_reset();
    assert(nat_stats().speed == 0 && nat_stats().tcp == 0);
    assert(tcp_generation[0] != generation && out_head == out_tail);
    nat_configure(true);
    local_open();
}
static void v_wifi_loss_and_disabled_input(void) {
    local_open();
    nat_configure(false); /* main.c uses this for both WiFi loss and relay mode. */
    assert(nat_stats().speed == 0 && nat_stats().tcp == 0 && !up);
    unsigned before = outputs;
    uint8_t frame[42] = {40, 0};
    memcpy(frame + 2, packet, 40);
    nat_ip_bytes(frame, sizeof(frame));
    nat_poll();
    assert(outputs == before && nat_stats().drop == 1);
    nat_configure(true);
    local_open();
}
static void v_remote_generation_reused_locally(void) {
    struct tcp_pcb *pcb = established();
    void *stale = pcb->callback_arg;
    console_tcp(4, 0); /* core aborts remote slot and invalidates callback token */
    assert(!tcp_pcbs[0]);
    uint32_t ack = local_open();
    nat_tcp_err(stale, ERR_RST);
    tcp_connected(stale, NULL, ERR_OK);
    nat_poll();
    assert(nat_stats().speed == 1 && !connected_pending[0].result);
    console_tcp_to(0xc000020a, 7778, 4 | 16, ack);
    nat_poll();
    assert(nat_stats().speed == 0);
    pcb = established();
    nat_tcp_err(stale, ERR_RST);
    nat_poll();
    assert(tcp_pcbs[0] == pcb && nat_stats().tcp == 1);
}
static uint32_t dns_destination;
static err_t capture_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ip) {
    dns_destination = ip4_number(ip);
    return net_output(netif, p, ip);
}
static void v_local_with_inbound_and_dns(void) {
    uint32_t ack = local_open();
    assert(nat_forward_add(8080, 1));
    struct tcp_pcb *pcb = tcp_new(); assert(pcb); pcb->state = ESTABLISHED;
    assert(accepted((void *)(uintptr_t)8080, pcb, ERR_OK) == ERR_OK);
    nat_poll();
    assert(tcp_pcbs[1] == pcb && nat_stats().speed == 1 && nat_stats().tcp == 2);
    uint8_t p[29] = {0}; p[0] = 0x45; put16(p + 2, sizeof(p)); p[8] = 64; p[9] = 17;
    put32(p + 12, 0x0a00020f); put32(p + 16, 0x0a000203);
    put16(p + 10, checksum(p, 20, 0));
    put16(p + 20, 4444); put16(p + 22, 53); put16(p + 24, 9); p[28] = 1;
    test_netif.output = capture_output;
    nat_core_input(core, p, sizeof(p), clock_ms);
    assert(udp_slots[0] && dns_destination == 0xc0000235);
    console_tcp_to(0xc000020a, 7778, 4 | 16, ack);
    nat_poll();
    assert(nat_stats().speed == 0 && nat_stats().tcp == 1);
    assert(tcp_pcbs[1] == pcb && listeners[0] && udp_slots[0]);
    nat_reset();
    assert(!tcp_pcbs[1] && !listeners[0] && !udp_slots[0]);
}
static void v_other_destination_and_port(void) {
    console_tcp_to(0xc000020a, 23, 2, 0);
    assert(tcp_pcbs[0] && tcp_pcbs[0]->remote_port == 23);
    console_tcp_to(0xc0000201, 7778, 2, 0);
    assert(tcp_pcbs[1] && tcp_pcbs[1]->remote_port == 7778);
    nat_poll();
    assert(nat_stats().speed == 0 && nat_stats().tcp == 2);
}
static const struct { const char *name; void (*run)(void); } validation_tests[] = {
    TEST(v_reset_local), TEST(v_wifi_loss_and_disabled_input),
    TEST(v_remote_generation_reused_locally), TEST(v_local_with_inbound_and_dns),
    TEST(v_other_destination_and_port)
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(validation_tests) / sizeof(*validation_tests); ++i)
            puts(validation_tests[i].name);
        return 0;
    }
    char *init[] = {argv[0], "initialize_only"};
    assert(original_lwip_harness_main(2, init) == 2);
    for (size_t i = 0; i < sizeof(validation_tests) / sizeof(*validation_tests); ++i)
        if (!strcmp(argv[1], validation_tests[i].name)) {
            validation_tests[i].run(); assert(locks == 0);
            printf("PASS %s\n", validation_tests[i].name); return 0;
        }
    return 2;
}
