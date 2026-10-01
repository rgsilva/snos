/* Additional independent lifetime tests. Reuse the unchanged real-peer fixture;
 * no production source substitutions. --wrap observes the LINK reset hook. */
#define main previous_validation_main
#include "final_validation_harness.c"
#undef main

static unsigned stream_resets;
void __real_ip_stream_reset(void);
void __wrap_ip_stream_reset(void) {
    ++stream_resets;
    __real_ip_stream_reset();
}
void last_relay_fail(unsigned kind);

static void t_repeated_nat_configuration_is_not_teardown(void) {
    setup_peer();
    uint8_t s[102]; stream(s, 100, 17); send_bytes(s, 31);
    assert(nat_forward_add(4321, 7));
    uint32_t generation = tcp_generation[0];
    unsigned resets_before = stream_resets;
    assert(link_push(CH_IP, s + 2, 100));
    const uint8_t error[] = {0, 0, 7}; assert(ctrl_error(error));
    size_t ip_queued = link_queued(CH_IP), ctrl_queued = link_queued(CH_CTRL);
    for (unsigned i = 0; i < 100; ++i) configure(false, true);
    assert(ipused == 31 && nat_stats().forwards == 1);
    assert(tcp_generation[0] == generation && stream_resets == resets_before);
    assert(link_queued(CH_IP) == ip_queued && link_queued(CH_CTRL) == ctrl_queued);
}
static void t_repeated_relay_configuration_is_not_teardown(void) {
    setup_peer(); start_relay();
    uint8_t s[102]; stream(s, 100, 17); send_bytes(s, 31);
    final_relay_receive(1, s + 2, 100);
    unsigned resets_before = stream_resets;
    for (unsigned i = 0; i < 100; ++i) configure(true, true);
    assert(ipused == 31 && stream_resets == resets_before);
    assert(link_queued(CH_IP) == sizeof(s));
    send_bytes(s + 31, sizeof(s) - 31);
    uint8_t output[103]; assert(final_relay_output(output, sizeof(output)) == sizeof(output));
    assert(output[0] == 1 && !memcmp(output + 1, s, sizeof(s)));
}
static void t_offline_counts_whole_packets_and_preserves_tail(void) {
    setup_peer(); configure(false, false);
    uint8_t s[42], bytes[95]; stream(s, 40, 19);
    memcpy(bytes, s, 42); memcpy(bytes + 42, s, 42); memcpy(bytes + 84, s, 11);
    uint32_t drops = nat_stats().drop;
    send_bytes(bytes, sizeof(bytes));
    assert(nat_stats().drop == drops + 2 && ipused == 11 && received_len == 0);
    for (unsigned i = 0; i < 50; ++i) configure(false, false);
    assert(nat_stats().drop == drops + 2 && ipused == 11);
    configure(false, true); send_bytes(s + 11, sizeof(s) - 11);
    assert_echo(s, sizeof(s));
}
static void t_endpoint_actions_never_reset_peer_stream(void) {
    setup_peer(); uint8_t s[102]; stream(s, 100, 23); send_bytes(s, 31);
    unsigned before = stream_resets;
    nat_reset(); assert(ipused == 31);
    configure(true, false); assert(ipused == 31);
    configure(true, true); final_relay_connect(); assert(ipused == 31);
    final_relay_eof(); assert(ipused == 31);
    final_relay_connect(); relay_reset(); assert(ipused == 31);
    relay_configure("192.0.2.99", true); assert(ipused == 31);
    configure(false, false); assert(ipused == 31);
    configure(false, true); assert(ipused == 31 && stream_resets == before);
    send_bytes(s + 31, sizeof(s) - 31); assert_echo(s, sizeof(s));
}
static void seal(uint8_t *f) {
    f[3] = 0; for (unsigned i = 0; i < LINK_FRAME; ++i) if (i != 3) f[3] += f[i];
}
static link_rx_t accept_frame(uint8_t *f) {
    link_rx_t r = link_on_rx(f);
    if (r.reset) { ctrl_reset(); relay_reset(); nat_reset(); }
    for (size_t i = 0; i < r.count; ++i)
        if (r.records[i].ch == CH_IP) nat_ip_bytes(r.records[i].data, r.records[i].len);
    return r;
}
static void t_syn_reset_is_once_before_new_payload(void) {
    setup_peer(); uint8_t old[102], fresh[42], f[LINK_FRAME];
    stream(old, 100, 3); stream(fresh, 40, 47); send_bytes(old, 31);
    unsigned before = stream_resets;
    memset(f, CH_PAD, sizeof(f)); f[0] = 77; f[1] = 255; f[2] = F_SYN | F_DATA;
    f[4] = CH_IP; f[5] = 17; memcpy(f + 6, fresh, 17); seal(f);
    link_rx_t r = accept_frame(f);
    assert(r.reset && stream_resets == before + 1 && ipused == 17);
    assert(!accept_frame(f).reset && stream_resets == before + 1 && ipused == 17);
    /* A corrupt non-SYN must not rearm the SYN edge detector. */
    f[2] = 0; f[3] ^= 1;
    assert(!accept_frame(f).valid && ipused == 17);
    f[2] = F_SYN | F_DATA; seal(f);
    assert(!accept_frame(f).reset && stream_resets == before + 1 && ipused == 17);
    /* The next payload may still carry SYN until its earlier frame is ACKed. */
    f[0] = 78; f[5] = 25; memcpy(f + 6, fresh + 17, 25); seal(f);
    assert(!accept_frame(f).reset && ipused == 0 && stream_resets == before + 1);
    assert(link_queued(CH_IP) == sizeof(fresh));
}
static void t_ack_duplicates_and_sequence_wrap_never_reset(void) {
    setup_peer(); uint8_t s[102], f[LINK_FRAME]; stream(s, 100, 13); send_bytes(s, 31);
    unsigned before = stream_resets;
    memset(f, CH_PAD, sizeof(f)); f[1] = 255;
    for (unsigned i = 0; i < 512; ++i) {
        f[0] = (uint8_t)i; f[2] = 0; seal(f);
        assert(!accept_frame(f).reset);
        f[2] = F_DATA; seal(f); /* Padding-only DATA exercises receive sequence wrap. */
        assert(!accept_frame(f).reset);
        assert(!accept_frame(f).reset);
        assert(ipused == 31 && stream_resets == before);
    }
}
static void reboot_offsets(bool relay) {
    for (unsigned offset = 1; offset < 102; ++offset) {
        setup_peer(); if (relay) start_relay();
        uint8_t old[102], fresh[42]; stream(old, 100, 7); stream(fresh, 40, 83);
        send_bytes(old, offset); assert(ipused == offset);
        unsigned before = stream_resets;
        console_peer_init(); pump(6);
        assert(resets == 1 && stream_resets == before + 1 && ipused == 0);
        console_peer_enable_ip(); configure(relay, true); send_bytes(fresh, sizeof(fresh));
        if (relay) {
            uint8_t output[43]; assert(final_relay_output(output, sizeof(output)) == sizeof(output));
            assert(output[0] == 1 && !memcmp(output + 1, fresh, sizeof(fresh)));
        } else assert_echo(fresh, sizeof(fresh));
    }
}
static void t_nat_reboot_at_every_partial_offset(void) { reboot_offsets(false); }
static void t_relay_reboot_at_every_partial_offset(void) { reboot_offsets(true); }
static void disconnect_queues(unsigned kind) {
    setup_peer(); start_relay(); uint8_t s[102], stale[42];
    stream(s, 100, 11); stream(stale, 40, 29);
    final_relay_receive(1, s + 2, 100); exchange(); assert(received_len == 58);
    final_relay_receive(1, stale + 2, 40);
    const uint8_t error[] = {0, 0, 7}; assert(ctrl_error(error));
    assert(link_push(CH_KBD, (const uint8_t *)"ABC", 3));
    unsigned keys = console_peer_keys(), before = stream_resets;
    last_relay_fail(kind);
    assert(link_queued(CH_CTRL) == 0 && link_queued(CH_IP) == 44);
    pump(10);
    assert(received_len == sizeof(s) && !memcmp(received, s, sizeof(s)));
    assert(console_peer_keys() == keys + 3);
    assert(!memcmp(console_peer_key_values() + keys, "ABC", 3));
    assert(stream_resets == before);
}
static void t_relay_tcp_error_purges_only_unstarted_network_output(void) { disconnect_queues(0); }
static void t_relay_connect_timeout_purges_only_unstarted_network_output(void) { disconnect_queues(1); }
static void t_relay_malformed_record_purges_only_unstarted_network_output(void) { disconnect_queues(2); }
static void t_relay_disconnect_preserves_started_control(void) {
    setup_peer(); start_relay();
    uint8_t old[102]; memset(old, 'x', sizeof(old));
    old[0] = 0x84; old[1] = 100; old[2] = 2;
    const uint8_t stale[] = {0x84, 2, 2, 'z'};
    unsigned before = console_peer_hellos();
    assert(link_push(CH_CTRL, old, sizeof(old))); exchange();
    assert(console_peer_hellos() == before && link_queued(CH_CTRL) == 44);
    assert(link_push(CH_CTRL, stale, sizeof(stale)));
    final_relay_eof(); assert(link_queued(CH_CTRL) == 44);
    final_relay_connect();
    const uint8_t replacement[] = {203, 0, 113, 9};
    final_relay_receive(5, replacement, sizeof(replacement));
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    pump(12);
    assert(console_peer_hellos() == before + 2);
    assert(!strcmp(console_peer_address(), "203.0.113.9"));
}
static void t_input_before_nat_init_keeps_framing(void) {
    assert(!ready && !up);
    uint8_t packet[42], bytes[95]; stream(packet, 40, 41);
    memcpy(bytes, packet, 42); memcpy(bytes + 42, packet, 42); memcpy(bytes + 84, packet, 11);
    nat_ip_bytes(bytes, sizeof(bytes));
    assert(ipused == 11 && nat_core_stats(core).drop == 2 && !locks);
    nat_init(); configure(false, true);
    assert(ipused == 11);
    nat_ip_bytes(packet + 11, sizeof(packet) - 11);
    assert(ipused == 0 && link_queued(CH_IP) == sizeof(packet));
}
static void reconnect_hello(bool address_first) {
    setup_peer(); start_relay(); final_relay_eof(); final_relay_connect();
    unsigned before = console_peer_hellos();
    const uint8_t replacement[] = {203, 0, 113, 9};
    if (address_first) final_relay_receive(5, replacement, sizeof(replacement));
    /* New explicit HELLO after reconnect; delay type-5 in the regression. */
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    pump(6);
    fprintf(stderr, "HELLO before current relay address: replies=%u address=%s\n",
            console_peer_hellos() - before, console_peer_address());
    unsigned premature = address_first ? 0 : console_peer_hellos() - before;
    if (!address_first) final_relay_receive(5, replacement, sizeof(replacement));
    pump(6);
    fprintf(stderr, "HELLO after current relay address: replies=%u address=%s\n",
            console_peer_hellos() - before, console_peer_address());
    assert(!premature && console_peer_hellos() == before + 1);
    assert(!strcmp(console_peer_address(), "203.0.113.9"));
}
static void t_relay_reconnect_waits_for_current_address(void) { reconnect_hello(false); }
static void t_relay_reconnect_address_before_hello_control(void) { reconnect_hello(true); }
static const struct { const char *name; void (*run)(void); } extra[] = {
    TEST(t_repeated_nat_configuration_is_not_teardown),
    TEST(t_repeated_relay_configuration_is_not_teardown),
    TEST(t_offline_counts_whole_packets_and_preserves_tail),
    TEST(t_endpoint_actions_never_reset_peer_stream),
    TEST(t_syn_reset_is_once_before_new_payload),
    TEST(t_ack_duplicates_and_sequence_wrap_never_reset),
    TEST(t_nat_reboot_at_every_partial_offset),
    TEST(t_relay_reboot_at_every_partial_offset),
    TEST(t_relay_tcp_error_purges_only_unstarted_network_output),
    TEST(t_relay_connect_timeout_purges_only_unstarted_network_output),
    TEST(t_relay_malformed_record_purges_only_unstarted_network_output),
    TEST(t_relay_reconnect_waits_for_current_address),
    TEST(t_relay_reconnect_address_before_hello_control),
    TEST(t_input_before_nat_init_keeps_framing),
    TEST(t_relay_disconnect_preserves_started_control),
};
int main(int argc, char **argv) {
    signal(SIGABRT, assertion_exit);
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(extra)/sizeof(*extra); ++i) puts(extra[i].name);
        return 0;
    }
    lwip_init(); ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 192, 0, 2, 10); IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 192, 0, 2, 1);
    assert(netif_add(&test_netif, &ip, &mask, &gw, NULL, net_init, ip_input));
    netif_set_default(&test_netif); netif_set_up(&test_netif); netif_set_link_up(&test_netif);
    link_init(); ctrl_reset(); relay_init();
    if (strcmp(argv[1], "t_input_before_nat_init_keeps_framing")) {
        nat_init(); configure(false, true);
    }
    for (unsigned i = 0; i < sizeof(extra)/sizeof(*extra); ++i) if (!strcmp(argv[1], extra[i].name)) {
        extra[i].run(); assert(!locks); printf("PASS %s\n", extra[i].name); return 0;
    }
    return 2;
}
