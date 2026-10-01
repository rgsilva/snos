/* Independent local-peer regressions. Reuse packet builders, never edit old tests. */
#define main original_nat_harness_main
#include "nat_harness.c"
#undef main

static void begin_speed(uint16_t port, const char *command, uint32_t *ack) {
    *ack = speed_open(port, 100, 1460);
    speed_request(port, 101, *ack, command);
    *ack += 3;
    send_tcp_to(PICO, port, 7778, ACK, 101 + strlen(command), *ack, 1460, NULL);
}
static void input(uint16_t port, uint32_t seq, uint32_t ack, uint16_t wnd,
                  unsigned flags, const char *payload) {
    send_tcp_to(PICO, port, 7778, flags, seq, ack, wnd, payload);
}
static bool occupied, reject_output;
static bool one_packet(void *arg, const uint8_t *p, size_t len) {
    if (occupied || reject_output) return false;
    emit(arg, p, len);
    occupied = true;
    return true;
}
static bool occupied_link(void *arg) { (void)arg; return occupied; }
static void single_packet_adapter(void) {
    nat_core_set_adapter(n, one_packet, occupied_link, NULL, NULL);
}
static void t_upload_fragment_duplicate_tail_wrap(void) {
    tick = UINT32_MAX - 500;
    uint32_t ack = speed_open(5500, 100, 1460);
    const char *command = "U 60\n";
    for (unsigned i = 0; command[i]; ++i) {
        char byte[] = {command[i], 0};
        input(5500, 101 + i, ack, 1460, PSH | ACK, byte);
    }
    nat_core_poll(n, tick);
    assert(last_len == 43 && !memcmp(last + 40, "OK\n", 3));
    ack += 3;
    input(5500, 106, ack, 1460, ACK, NULL);
    input(5500, 106, ack, 1460, PSH | ACK, "G");
    input(5500, 107, ack, 1460, PSH | ACK, "O\nabc");
    uint32_t start = tick;
    input(5500, 107, ack, 1460, PSH | ACK, "O\nabc"); /* retransmission */
    tick = start + 999;
    input(5500, 112, ack, 1460, PSH | ACK, "de");
    unsigned before = emitted;
    nat_core_poll(n, tick);
    assert(emitted == before);
    tick = start + 1000;
    input(5500, 114, ack, 0, PSH | ACK, "late");
    nat_core_poll(n, tick);
    assert(last[33] == ACK); /* report blocked by zero window */
    input(5500, 118, ack, 1460, ACK, NULL);
    nat_core_poll(n, ++tick);
    assert(last_len == 42 && !memcmp(last + 40, "5\n", 2));
    input(5500, 118, ack + 2, 1460, ACK, NULL);
    input(5500, 118, ack + 2, 1460, PSH | ACK, "more late");
    before = emitted;
    nat_core_poll(n, ++tick);
    assert(emitted == before);
}
static void t_download_window_stop_and_wait(void) {
    uint32_t ack;
    begin_speed(5500, "D 60\n", &ack);
    input(5500, 106, ack, 0, PSH | ACK, "GO\n");
    unsigned before = emitted;
    nat_core_poll(n, ++tick);
    assert(emitted == before);
    input(5500, 109, ack, 1460, ACK, NULL);
    nat_core_poll(n, ++tick);
    assert(last_len == 1500 && rd32(last + 24) == ack);
    before = emitted;
    nat_core_poll(n, ++tick);
    assert(emitted == before);
    input(5500, 109, ack + 1460, 0, ACK, NULL);
    before = emitted;
    nat_core_poll(n, ++tick);
    assert(emitted == before);
    input(5500, 109, ack + 1460, 1460, ACK, NULL);
    nat_core_poll(n, ++tick);
    assert(last_len == 1500 && rd32(last + 24) == ack + 1460);
}
static void t_download_small_window_progress(void) {
    uint32_t ack;
    begin_speed(5500, "D 60\n", &ack);
    input(5500, 106, ack, 128, PSH | ACK, "GO\n");
    unsigned before = emitted;
    nat_core_poll(n, ++tick);
    assert(emitted > before && last_len > 40 && last_len <= 168);
}
static void t_download_fin_discards_unsent_tail(void) {
    uint32_t ack;
    begin_speed(5500, "D 60\n", &ack);
    input(5500, 106, ack, 0, PSH | ACK, "GO\n");
    nat_core_poll(n, ++tick); /* stages a download segment, cannot send it */
    input(5500, 109, ack, 0, FIN | ACK, NULL);
    unsigned before = emitted;
    nat_core_poll(n, ++tick);
    assert(emitted > before && last[33] == (FIN | ACK));
}
static void t_download_maximum_lifetime(void) {
    uint32_t ack;
    begin_speed(5500, "D 60\n", &ack);
    input(5500, 106, ack, 1460, PSH | ACK, "GO\n");
    uint32_t start = tick;
    for (unsigned i = 0; i < 18; ++i) {
        tick = start + i * 1000;
        nat_core_poll(n, tick);
        if (last_len == 1500) {
            ack += 1460;
            input(5500, 109, ack, 1460, ACK, NULL);
        }
    }
    /* Python D closes at duration + TIMEOUT even if the client keeps reading. */
    nat_core_poll(n, ++tick);
    assert(last[33] & (FIN | RST));
}
static void t_report_maximum_lifetime(void) {
    uint32_t ack;
    begin_speed(5500, "U 60\n", &ack);
    input(5500, 106, ack, 1460, PSH | ACK, "GO\n");
    tick += 1000;
    nat_core_poll(n, tick);
    assert(last_len == 42 && !memcmp(last + 40, "0\n", 2));
    input(5500, 109, ack + 2, 1460, ACK, NULL);
    tick += 15001;
    nat_core_poll(n, tick);
    assert(last[33] & (FIN | RST));
}
static void t_local_does_not_starve_remote(void) {
    uint32_t ack;
    begin_speed(5500, "D 7200\n", &ack);
    input(5500, 108, ack, 1460, PSH | ACK, "GO\n");
    send_tcp(SYN, 100, 0, 1460, NULL);
    nat_core_tcp_connected(n, 1, 0, tick);
    uint32_t remote_ack = rd32(last + 24) + 1;
    send_tcp(ACK, 101, remote_ack, 1460, NULL);
    assert(nat_core_tcp_received(n, 1, (const uint8_t *)"remote", 6, tick));
    single_packet_adapter();
    unsigned remote_packets = 0;
    for (unsigned i = 0; i < 100; ++i) {
        occupied = false;
        tick += 1000;
        nat_core_poll(n, tick);
        assert(occupied);
        if (rd16(last + 20) == 80) { ++remote_packets; break; }
        assert(last_len == 1500);
        ack += 1460;
        input(5500, 111, ack, 1460, ACK, NULL);
    }
    assert(remote_packets > 0);
}
static void t_local_slots_leave_remote_capacity(void) {
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) {
        uint32_t ack;
        begin_speed(5500 + i, "U 60\n", &ack);
        input(5500 + i, 106, ack, 1460, PSH | ACK, "GO\n");
        tick += 1000;
        nat_core_poll(n, tick);
        assert(last_len == 42 && !memcmp(last + 40, "0\n", 2));
        input(5500 + i, 109, ack + 2, 1460, ACK, NULL);
    }
    /* Every report is acknowledged; give the Python peer's 15-second close
     * deadline, FIN retry budget, and TIME_WAIT ample time to release slots. */
    for (unsigned i = 0; i < 40; ++i) {
        tick += 2501;
        nat_core_poll(n, tick);
    }
    send_tcp(SYN, 100, 0, 1460, NULL);
    assert(connects == 1); /* Completed local tests must eventually release capacity. */
}
static void t_local_rst_fin_time_wait_idle(void) {
    uint32_t ack = speed_open(5500, 100, 1460);
    input(5500, 101, ack, 1460, RST | ACK, NULL);
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).tcp == 0 && aborts == 0);
    ack = speed_open(5500, 100, 1460);
    input(5500, 101, ack, 1460, FIN | ACK, NULL);
    nat_core_poll(n, ++tick);
    assert(last[33] == (FIN | ACK));
    input(5500, 102, ack + 1, 1460, ACK, NULL);
    uint32_t closed = tick;
    nat_core_poll(n, closed + 4000);
    assert(nat_core_stats(n).speed == 1);
    nat_core_poll(n, closed + 4001);
    assert(nat_core_stats(n).speed == 0);
    begin_speed(5500, "U 60\n", &ack);
    input(5500, 106, ack, 1460, PSH | ACK, "GO\n");
    tick += 300001;
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 0);
}
static void t_local_busy_retries(void) {
    uint32_t ack;
    begin_speed(5500, "D 60\n", &ack);
    input(5500, 106, ack, 1460, PSH | ACK, "GO\n");
    single_packet_adapter();
    reject_output = true;
    unsigned before = emitted;
    nat_core_poll(n, tick);
    assert(emitted == before);
    reject_output = false;
    nat_core_poll(n, ++tick);
    assert(last_len == 1500 && rd32(last + 24) == ack);
    before = emitted;
    for (unsigned i = 0; i < 130; ++i) {
        tick += 2501;
        input(5500, 109, ack, 0, ACK, NULL); /* refresh idle, not data ACK */
        nat_core_poll(n, tick); /* occupied link never spends data retries */
    }
    assert(emitted == before && nat_core_stats(n).speed == 1);
    for (unsigned i = 0; i < 120; ++i) {
        occupied = false;
        tick += 2501;
        input(5500, 109, ack, 0, ACK, NULL);
        nat_core_poll(n, tick);
        assert(last_len == 1500 && rd32(last + 24) == ack);
    }
    occupied = false;
    tick += 2501;
    input(5500, 109, ack, 0, ACK, NULL);
    nat_core_poll(n, tick);
    assert((last[33] & RST) && nat_core_stats(n).speed == 0);
}
static void t_local_handshake_fin_retries(void) {
    single_packet_adapter();
    reject_output = true;
    input(5500, 100, 0, 1460, SYN, NULL);
    assert(emitted == 0);
    reject_output = false;
    nat_core_poll(n, ++tick);
    assert(last[33] == (SYN | ACK));
    uint32_t ack = rd32(last + 24) + 1;
    for (unsigned i = 0; i < 8; ++i) {
        occupied = false; tick += 2501; nat_core_poll(n, tick);
        assert(last[33] == (SYN | ACK));
    }
    occupied = false; tick += 2501; nat_core_poll(n, tick);
    assert((last[33] & RST) && nat_core_stats(n).speed == 0);
    nat_core_set_adapter(n, NULL, NULL, NULL, NULL);
    ack = speed_open(5500, 100, 1460);
    input(5500, 101, ack, 1460, FIN | ACK, NULL);
    nat_core_poll(n, ++tick);
    assert(last[33] == (FIN | ACK));
    for (unsigned i = 0; i < 8; ++i) {
        tick += 2501; nat_core_poll(n, tick);
        assert(last[33] == (FIN | ACK));
    }
    tick += 2501; nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 0);
}
static void t_local_control_deadline_and_garbage(void) {
    uint32_t ack = speed_open(5500, 100, 1460);
    uint32_t start = tick;
    input(5500, 101, ack, 1460, PSH | ACK, "U");
    tick = start + 14999;
    input(5500, 102, ack, 1460, PSH | ACK, " ");
    nat_core_poll(n, tick);
    assert(last[33] == ACK);
    tick = start + 15000;
    nat_core_poll(n, tick);
    assert(last[33] == (FIN | ACK));
    input(5500, 103, ack, 1460, RST, NULL);
    begin_speed(5501, "U 60\n", &ack);
    input(5501, 106, ack, 1460, PSH | ACK, "NO\n");
    assert(last[33] == (RST | ACK));
}
static void t_upload_binary_payload_and_partial_report_ack(void) {
    uint32_t ack;
    begin_speed(5500, "U 60\n", &ack);
    input(5500, 106, ack, 1460, PSH | ACK, "GO\n");
    uint32_t start = tick;
    for (unsigned segment = 0; segment < 6; ++segment) {
        uint8_t b[1480] = {0};
        wr16(b, 5500); wr16(b + 2, 7778);
        wr32(b + 4, 109 + segment * 1460); wr32(b + 8, ack);
        b[12] = 0x50; b[13] = PSH | ACK; wr16(b + 14, 1460);
        for (unsigned i = 0; i < 1460; ++i) b[20 + i] = (uint8_t)i;
        wr16(b + 16, fold(b, sizeof(b), pseudo(SNES, PICO, 6, sizeof(b))));
        ++tick;
        send_ip(6, PICO, b, sizeof(b));
        assert(last[33] == ACK && rd32(last + 28) == 109 + (segment + 1) * 1460);
    }
    tick = start + 1000;
    nat_core_poll(n, tick);
    assert(last_len == 45 && !memcmp(last + 40, "8760\n", 5));
    input(5500, 8869, ack + 2, 1460, ACK, NULL);
    tick += 2501;
    nat_core_poll(n, tick);
    assert(last_len == 43 && rd32(last + 24) == ack + 2);
    assert(!memcmp(last + 40, "60\n", 3));
    input(5500, 8869, ack + 5, 1460, RST | ACK, NULL);
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 0);
}
static void t_invalid_requests(void) {
    const char *bad[] = {"U 0\n", "D 7201\n", "U -60\n", "X 60\n", "D  60\n", "U 60\r\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        uint32_t ack = speed_open(5500, 100, 1460);
        input(5500, 101, ack, 1460, PSH | ACK, bad[i]);
        assert(last[33] == (RST | ACK));
        nat_core_poll(n, tick);
        assert(nat_core_stats(n).speed == 0);
    }
}
static void t_overlong_control_rejected_at_limit(void) {
    uint32_t ack = speed_open(5500, 100, 1460);
    input(5500, 101, ack, 1460, PSH | ACK, "xxxxxxxxxxxxxxxxxxxxxxxx");
    assert(last[33] == (RST | ACK)); /* same 24-byte limit as speedtest.py */
}
static const test_t validation_tests[] = {
    TEST(t_upload_fragment_duplicate_tail_wrap), TEST(t_download_window_stop_and_wait),
    TEST(t_download_small_window_progress), TEST(t_download_fin_discards_unsent_tail),
    TEST(t_download_maximum_lifetime), TEST(t_report_maximum_lifetime),
    TEST(t_local_does_not_starve_remote), TEST(t_local_slots_leave_remote_capacity),
    TEST(t_local_rst_fin_time_wait_idle), TEST(t_local_busy_retries),
    TEST(t_local_handshake_fin_retries), TEST(t_local_control_deadline_and_garbage),
    TEST(t_upload_binary_payload_and_partial_report_ack), TEST(t_invalid_requests),
    TEST(t_overlong_control_rejected_at_limit)
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(validation_tests) / sizeof(*validation_tests); ++i)
            puts(validation_tests[i].name);
        return 0;
    }
    nat_ops_t ops = {emit, connect_, write_, shutdown_, abort_, udp, fadd, fdel, got, NULL};
    assert(nat_core_size() <= sizeof(store));
    nat_core_init(n, &ops);
    nat_core_set_local_address(n, PICO);
    for (size_t i = 0; i < sizeof(validation_tests) / sizeof(*validation_tests); ++i)
        if (!strcmp(argv[1], validation_tests[i].name)) {
            validation_tests[i].run();
            printf("PASS %s\n", validation_tests[i].name);
            return 0;
        }
    return 2;
}
