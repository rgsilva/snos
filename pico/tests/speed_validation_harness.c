/* Independent local-peer regressions using the core harness packet builders. */
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
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t ack;
        begin_speed(5500 + i, "U 60\n", &ack);
        input(5500 + i, 106, ack, 1460, PSH | ACK, "GO\n");
        tick += 1000;
        nat_core_poll(n, tick);
        assert(last_len == 42 && !memcmp(last + 40, "0\n", 2));
        input(5500 + i, 109, ack + 2, 1460, ACK, NULL);
    }
    unsigned before = emitted;
    input(5502, 100, 0, 1460, SYN, NULL);
    assert(emitted == before && connects == 0); /* Third local peer refused. */
    for (unsigned i = 0; i < NAT_TCP_MAX - 2; ++i)
        send_tcp_to(REMOTE, 5000 + i, 80, SYN, 100, 0, 1460, NULL);
    nat_core_poll(n, tick);
    assert(connects == NAT_TCP_MAX - 2 && nat_core_stats(n).tcp == NAT_TCP_MAX);
    assert(nat_core_stats(n).speed == 2);
    /* Reports are acknowledged. Even clients that ignore FIN must release
     * their local slots after the grace period and bounded FIN retries. */
    for (unsigned i = 0; i < 40; ++i) {
        tick += 2501;
        nat_core_poll(n, tick);
    }
    assert(nat_core_stats(n).speed == 0);
    speed_open(5502, 100, 1460);
    speed_open(5503, 100, 1460);
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 2);
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
    uint32_t start = tick;
    single_packet_adapter();
    reject_output = true;
    unsigned before = emitted;
    nat_core_poll(n, tick);
    assert(emitted == before);
    reject_output = false;
    nat_core_poll(n, ++tick);
    assert(last_len == 1500 && rd32(last + 24) == ack);
    before = emitted;
    tick = start + 3000;
    nat_core_poll(n, tick); /* Busy link defers RTO without spending a retry. */
    assert(emitted == before && nat_core_stats(n).speed == 1);
    occupied = false;
    tick += 2500;
    nat_core_poll(n, tick);
    assert(emitted == before); /* RTO is strict > 2500 ms. */
    nat_core_poll(n, ++tick);
    assert(emitted == before + 1 && last_len == 1500 && rd32(last + 24) == ack);
    occupied = false;
    tick = start + 16000;
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
/* Second-pass checks use only the public core API and wire observations. */
static uint32_t remote_open(unsigned slot, uint16_t port) {
    send_tcp_to(REMOTE, port, 80, SYN, 100, 0, 1460, NULL);
    nat_core_tcp_connected(n, slot, 0, tick);
    assert(last[33] == (SYN | ACK) && rd16(last + 22) == port);
    uint32_t ack = rd32(last + 24) + 1;
    send_tcp_to(REMOTE, port, 80, ACK, 101, ack, 1460, NULL);
    return ack;
}
static void t_download_fin_discards_partial_window_tail(void) {
    uint32_t ack;
    begin_speed(5500, "D 7200\n", &ack);
    input(5500, 108, ack, 128, PSH | ACK, "GO\n");
    nat_core_poll(n, ++tick);
    assert(last_len == 168);
    /* FIN can arrive before the ACK of the 128 transmitted bytes. The other
     * 1332 bytes have never been sent and must not delay close. */
    input(5500, 111, ack, 0, FIN | ACK, NULL);
    input(5500, 112, ack + 128, 0, ACK, NULL);
    unsigned before = emitted;
    nat_core_poll(n, ++tick);
    assert(emitted == before + 1 && last[33] == (FIN | ACK));
}
static void t_report_deadline_wraps_to_zero(void) {
    uint32_t ack;
    tick = UINT32_MAX - 16000u;
    begin_speed(5500, "U 60\n", &ack);
    input(5500, 106, ack, 1460, PSH | ACK, "GO\n");
    tick += 1000;
    nat_core_poll(n, tick); /* now + 15000 == 0, a valid absolute deadline */
    assert(last_len == 42 && !memcmp(last + 40, "0\n", 2));
    input(5500, 109, ack + 2, 1460, ACK, NULL);
    tick += 14999;
    unsigned before = emitted;
    nat_core_poll(n, tick);
    assert(emitted == before);
    ++tick;
    nat_core_poll(n, tick);
    assert(emitted == before + 1 && last[33] == (FIN | ACK));
}
static void t_report_zero_window_has_send_timeout(void) {
    uint32_t ack;
    begin_speed(5500, "U 60\n", &ack);
    input(5500, 106, ack, 0, PSH | ACK, "GO\n");
    tick += 1000;
    nat_core_poll(n, tick);
    /* Allow BOTH Python's 15 s sendall timeout and a full 15 s tail wait.
     * Even this upper bound must not be extended by refreshing TCP_IDLE. */
    for (unsigned i = 0; i < 31; ++i) {
        tick += 1000;
        input(5500, 109 + i, ack, 0, PSH | ACK, "x");
        nat_core_poll(n, tick);
    }
    assert(last[33] & (FIN | RST));
}
static void t_handshake_zero_window_has_deadline(void) {
    uint32_t ack = speed_open(5500, 100, 1460);
    input(5500, 101, ack, 0, PSH | ACK, "U 60\n");
    uint32_t start = tick;
    nat_core_poll(n, tick); /* OK is staged behind a zero window. */
    /* Allow the separate 15 s OK-send and 15 s GO-read waits in Python. */
    tick = start + 30001;
    unsigned before = emitted;
    nat_core_poll(n, tick);
    assert(emitted > before && (last[33] & (FIN | RST)));
}
static void t_download_exact_deadline_and_clock_wrap(void) {
    const uint32_t starts[] = {100, UINT32_MAX - 500};
    const unsigned frames[] = {60, 61, 7200};
    for (unsigned w = 0; w < 2; ++w) {
        for (unsigned f = 0; f < 3; ++f) {
            uint32_t ack;
            tick = starts[w];
            char command[24];
            snprintf(command, sizeof(command), "D %u\n", frames[f]);
            begin_speed(5500, command, &ack);
            uint32_t seq = 101 + (uint32_t)strlen(command);
            input(5500, seq, ack, 0, PSH | ACK, "GO\n");
            uint32_t end = tick + frames[f] * 1000u / 60u + 15000u;
            nat_core_poll(n, tick);
            unsigned before = emitted;
            tick = end - 1;
            nat_core_poll(n, tick);
            assert(emitted == before && nat_core_stats(n).speed == 1);
            tick = end;
            nat_core_poll(n, tick);
            assert(emitted == before + 1 && last[33] == (FIN | ACK));
            input(5500, seq + 3, ack + 1, 0, RST | ACK, NULL);
        }
    }
}
static void t_report_small_window_tail_deadline(void) {
    uint32_t ack;
    begin_speed(5500, "U 60\n", &ack);
    input(5500, 106, ack, 1, PSH | ACK, "GO\n");
    tick += 1000;
    nat_core_poll(n, tick);
    assert(last_len == 41 && last[40] == '0');
    input(5500, 109, ack + 1, 0, ACK, NULL);
    tick += 10000;
    input(5500, 109, ack + 1, 1, ACK, NULL);
    nat_core_poll(n, tick);
    assert(last_len == 41 && last[40] == '\n' && rd32(last + 24) == ack + 1);
    input(5500, 109, ack + 2, 1, ACK, NULL);
    uint32_t end = tick + 15000;
    unsigned before = emitted;
    tick = end - 1;
    nat_core_poll(n, tick);
    assert(emitted == before);
    tick = end;
    nat_core_poll(n, tick);
    assert(emitted == before + 1 && last[33] == (FIN | ACK));
}
static void retry_fairness(unsigned kind, bool local_source, bool local_victim, bool poll_busy) {
    uint32_t source_ack, victim_ack = 0;
    if (local_source) {
        begin_speed(5500, "D 7200\n", &source_ack);
        input(5500, 108, source_ack, 1460, PSH | ACK, "GO\n");
    } else source_ack = remote_open(0, 5600);
    if (local_victim) {
        begin_speed(5501, "D 7200\n", &victim_ack);
        input(5501, 108, victim_ack, 1460, PSH | ACK, "GO\n");
    } else if (kind == 1) {
        send_tcp_to(REMOTE, 5601, 80, SYN, 100, 0, 1460, NULL);
        nat_core_tcp_connected(n, 1, 0, tick);
        victim_ack = rd32(last + 24) + 1; /* Deliberately lose SYN ACK. */
    } else victim_ack = remote_open(1, 5601);
    /* Isolate the victim's first transmission. */
    if (local_source) input(5500, 111, source_ack, 0, ACK, NULL);
    if (kind == 0) {
        if (!local_victim)
            assert(nat_core_tcp_received(n, 1, (const uint8_t *)"victim", 6, tick));
        nat_core_poll(n, ++tick);
        assert(rd16(last + 22) == (local_victim ? 5501 : 5601));
        assert(last[33] == (PSH | ACK)); /* Lose data / its ACK. */
    } else if (kind == 2) {
        nat_core_tcp_eof(n, 1, tick);
        nat_core_poll(n, ++tick);
        assert(last[33] == (FIN | ACK) && rd16(last + 22) == 5601);
    }
    if (local_source) input(5500, 111, source_ack, 1460, ACK, NULL);
    single_packet_adapter();
    occupied = true; /* Victim's initial packet is still draining. */
    unsigned retried = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        if (poll_busy) {
            tick += 50;
            nat_core_poll(n, tick); /* Main loop also polls while the frame drains. */
        }
        occupied = false;
        tick += poll_busy ? 50 : 100; /* One packet opportunity every 100 ms. */
        if (!local_source)
            (void)nat_core_tcp_received(n, 0, (const uint8_t *)"source", 6, tick);
        nat_core_poll(n, tick);
        if (!occupied) continue;
        if (rd16(last + 22) == (local_victim ? 5501 : 5601)) {
            unsigned expected_flags = kind == 0 ? (PSH | ACK) :
                                      kind == 1 ? (SYN | ACK) : (FIN | ACK);
            assert(last[33] == expected_flags);
            assert(rd32(last + 24) == victim_ack - (kind == 1));
            ++retried;
            break;
        }
        assert(rd16(last + 22) == (local_source ? 5500 : 5600));
        source_ack += (uint32_t)(last_len - 40);
        if (local_source) input(5500, 111, source_ack, 1460, ACK, NULL);
        else send_tcp_to(REMOTE, 5600, 80, ACK, 101, source_ack, 1460, NULL);
    }
    if (!retried)
        fprintf(stderr, "No victim retry in 1000 link opportunities over 100 seconds\n");
    assert(retried > 0); /* A ready source cannot suppress retries for 100 s. */
}
static void t_local_source_remote_data_retry_fair(void) { retry_fairness(0, true, false, false); }
static void t_remote_source_remote_data_retry_fair(void) { retry_fairness(0, false, false, false); }
static void t_remote_source_local_data_retry_fair(void) { retry_fairness(0, false, true, false); }
static void t_local_source_remote_syn_retry_fair(void) { retry_fairness(1, true, false, false); }
static void t_local_source_remote_fin_retry_fair(void) { retry_fairness(2, true, false, false); }

static void t_busy_local_source_remote_data_retry_fair(void) { retry_fairness(0, true, false, true); }
static void t_busy_remote_source_remote_data_retry_fair(void) { retry_fairness(0, false, false, true); }
static void t_busy_remote_source_local_data_retry_fair(void) { retry_fairness(0, false, true, true); }
static void t_busy_local_source_remote_syn_retry_fair(void) { retry_fairness(1, true, false, true); }
static void t_busy_local_source_remote_fin_retry_fair(void) { retry_fairness(2, true, false, true); }
static void t_input_ack_cannot_starve_ready_remote(void) {
    uint32_t victim = remote_open(0, 5600);
    uint32_t uploader = remote_open(1, 5601);
    uint32_t source = remote_open(2, 5602);
    assert(nat_core_tcp_received(n, 0, (const uint8_t *)"victim", 6, tick));
    single_packet_adapter();
    unsigned delivered = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        tick += 100;
        occupied = false;
        send_tcp_to(REMOTE, 5601, 80, PSH | ACK, 101 + i, uploader, 1460, "u");
        assert(last[33] == ACK && rd16(last + 22) == 5601);
        /* The immediate ACK drains. There is now a second packet opportunity
         * before any more console input, with two remote responses ready. */
        occupied = false;
        assert(nat_core_tcp_received(n, 2, (const uint8_t *)"source", 6, tick));
        nat_core_poll(n, ++tick);
        assert(occupied);
        if (rd16(last + 22) == 5600) {
            assert(rd32(last + 24) == victim && !memcmp(last + 40, "victim", 6));
            ++delivered;
            break;
        }
        assert(rd16(last + 22) == 5602 && last_len == 46);
        source += 6;
        send_tcp_to(REMOTE, 5602, 80, ACK, 101, source, 1460, NULL);
    }
    if (!delivered)
        fprintf(stderr, "No victim data in 1000 opportunities after input ACKs drained\n");
    assert(delivered > 0);
}
static void t_round_robin_stream_integrity_all_slots(void) {
    uint32_t ack[NAT_TCP_MAX];
    unsigned offset[NAT_TCP_MAX] = {0}, visits[NAT_TCP_MAX] = {0};
    uint8_t data[2920];
    /* Two local downloads and six distinguishable remote byte streams. */
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) {
        if (i < 2) {
            begin_speed(5500 + i, "D 7200\n", &ack[i]);
            input(5500 + i, 108, ack[i], 0, PSH | ACK, "GO\n");
        } else ack[i] = remote_open(i, 5500 + i);
    }
    for (unsigned i = 0; i < 2; ++i)
        input(5500 + i, 111, ack[i], 128, ACK, NULL);
    for (unsigned i = 2; i < NAT_TCP_MAX; ++i) {
        for (unsigned j = 0; j < sizeof(data); ++j) data[j] = (uint8_t)(j + i * 31);
        assert(nat_core_tcp_received(n, i, data, sizeof(data), tick));
    }
    single_packet_adapter();
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
        unsigned seen = 0;
        for (unsigned opportunity = 0; opportunity < NAT_TCP_MAX; ++opportunity) {
            occupied = false;
            unsigned before = emitted;
            nat_core_poll(n, ++tick);
            assert(occupied && emitted == before + 1);
            unsigned i = rd16(last + 22) - 5500;
            assert(i < NAT_TCP_MAX && !(seen & (1u << i)));
            seen |= 1u << i;
            assert(rd32(last + 24) == ack[i] && last[33] == (PSH | ACK));
            size_t len = last_len - 40;
            assert(len > 0 && len <= (i < 2 ? 128u : 1460u));
            for (unsigned j = 0; j < len; ++j)
                assert(last[40 + j] == (i < 2 ? 'x' : (uint8_t)(offset[i] + j + i * 31)));
            offset[i] += len; ack[i] += len; ++visits[i];
            send_tcp_to(i < 2 ? PICO : REMOTE, 5500 + i, i < 2 ? 7778 : 80,
                        ACK, i < 2 ? 111 : 101, ack[i], i < 2 ? 128 : 1460, NULL);
            if (i >= 2 && offset[i] % sizeof(data) == 0) {
                for (unsigned j = 0; j < sizeof(data); ++j)
                    data[j] = (uint8_t)(offset[i] + j + i * 31);
                assert(nat_core_tcp_received(n, i, data, sizeof(data), tick));
            }
        }
        assert(seen == (1u << NAT_TCP_MAX) - 1);
    }
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) assert(visits[i] == 32);
}
static void t_remote_busy_deferral_and_full_retry_budget(void) {
    uint32_t ack = remote_open(0, 5600);
    assert(nat_core_tcp_received(n, 0, (const uint8_t *)"abc", 3, tick));
    single_packet_adapter();
    nat_core_poll(n, tick);
    unsigned before = emitted;
    for (unsigned i = 0; i < 130; ++i) {
        tick += 2501;
        send_tcp_to(REMOTE, 5600, 80, ACK, 101, ack, 0, NULL);
        nat_core_poll(n, tick);
    }
    assert(emitted == before && nat_core_stats(n).tcp == 1);
    for (unsigned i = 0; i < 120; ++i) {
        occupied = false;
        tick += 2501;
        send_tcp_to(REMOTE, 5600, 80, ACK, 101, ack, 0, NULL);
        nat_core_poll(n, tick);
        assert(emitted == before + i + 1 && last_len == 43);
        assert(rd32(last + 24) == ack && !memcmp(last + 40, "abc", 3));
    }
    occupied = false;
    tick += 2501;
    send_tcp_to(REMOTE, 5600, 80, ACK, 101, ack, 0, NULL);
    nat_core_poll(n, tick);
    assert((last[33] & RST) && nat_core_stats(n).tcp == 0);
}
/* Third-pass checks: mixed scheduler work, rejected output and hard lifetimes. */
static void t_mixed_ready_flows_one_rotation(void) {
    uint32_t ack[8];
    ack[0] = remote_open(0, 5700); /* continuously replenished new data */
    ack[1] = remote_open(1, 5701); /* data retransmission */
    send_tcp_to(REMOTE, 5702, 80, SYN, 100, 0, 1460, NULL);
    nat_core_tcp_connected(n, 2, 0, tick);
    ack[2] = rd32(last + 24); /* SYN retransmission */
    ack[3] = remote_open(3, 5703); /* FIN retransmission */
    ack[4] = remote_open(4, 5704); /* first FIN */
    ack[5] = remote_open(5, 5705); /* first data */
    begin_speed(5706, "D 7200\n", &ack[6]);
    input(5706, 108, ack[6], 0, PSH | ACK, "GO\n");
    begin_speed(5707, "U 7200\n", &ack[7]);
    input(5707, 108, ack[7], 0, PSH | ACK, "GO\n");
    assert(nat_core_tcp_received(n, 1, (const uint8_t *)"retry", 5, tick));
    nat_core_tcp_eof(n, 3, tick);
    nat_core_poll(n, tick);
    single_packet_adapter();
    occupied = true;
    nat_core_poll(n, tick + 1); /* normal main-loop busy observation before RTO */
    tick += 120001; /* upload report becomes ready; all retries are due */
    assert(nat_core_tcp_received(n, 0, (const uint8_t *)"new", 3, tick));
    assert(nat_core_tcp_received(n, 5, (const uint8_t *)"other", 5, tick));
    nat_core_tcp_eof(n, 4, tick);
    input(5706, 111, ack[6], 128, ACK, NULL);
    input(5707, 111, ack[7], 1460, ACK, NULL);
    unsigned seen = 0;
    for (unsigned opportunity = 0; opportunity < NAT_TCP_MAX; ++opportunity) {
        occupied = false;
        unsigned before = emitted;
        nat_core_poll(n, ++tick);
        assert(occupied && emitted == before + 1);
        unsigned i = rd16(last + 22) - 5700;
        assert(i < 8 && !(seen & (1u << i)));
        seen |= 1u << i;
        assert(rd32(last + 24) == ack[i]);
        if (i == 2) assert(last[33] == (SYN | ACK));
        else if (i == 3 || i == 4) assert(last[33] == (FIN | ACK));
        else {
            assert(last[33] == (PSH | ACK));
            const char *want = i == 0 ? "new" : i == 1 ? "retry" :
                               i == 5 ? "other" : i == 7 ? "0\n" : NULL;
            if (want) assert(last_len == 40 + strlen(want) &&
                             !memcmp(last + 40, want, strlen(want)));
            else {
                assert(last_len == 168);
                for (unsigned j = 40; j < last_len; ++j) assert(last[j] == 'x');
            }
        }
        /* Keep slot zero eligible: it must not get a second turn before
         * every other flow that was ready at entry has had its first. */
        if (i == 0) {
            ack[0] += 3;
            send_tcp_to(REMOTE, 5700, 80, ACK, 101, ack[0], 1460, NULL);
            assert(nat_core_tcp_received(n, 0, (const uint8_t *)"new", 3, tick));
        }
    }
    assert(seen == 255);
}
static void t_rejected_output_keeps_sequence_and_rotation(void) {
    uint32_t ack[2];
    ack[0] = remote_open(0, 5700);
    ack[1] = remote_open(1, 5701);
    for (unsigned i = 0; i < 2; ++i)
        assert(nat_core_tcp_received(n, i, (const uint8_t *)"abc", 3, tick));
    single_packet_adapter();
    reject_output = true;
    unsigned before = emitted;
    for (unsigned i = 0; i < 8; ++i) nat_core_poll(n, ++tick);
    assert(emitted == before);
    reject_output = false;
    unsigned seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        occupied = false;
        nat_core_poll(n, ++tick);
        unsigned slot = rd16(last + 22) - 5700;
        assert(slot < 2 && !(seen & (1u << slot)));
        seen |= 1u << slot;
        assert(rd32(last + 24) == ack[slot] && last_len == 43);
        assert(!memcmp(last + 40, "abc", 3));
    }
    assert(seen == 3);
}
static void t_fin_partial_ack_retries_only_outstanding_prefix(void) {
    uint32_t ack;
    begin_speed(5700, "D 7200\n", &ack);
    input(5700, 108, ack, 128, PSH | ACK, "GO\n");
    nat_core_poll(n, ++tick);
    assert(last_len == 168);
    input(5700, 111, ack + 37, 0, FIN | ACK, NULL);
    tick += 2501;
    nat_core_poll(n, tick);
    assert(last_len == 131 && rd32(last + 24) == ack + 37);
    for (unsigned i = 40; i < last_len; ++i) assert(last[i] == 'x');
    input(5700, 112, ack + 128, 0, ACK, NULL);
    nat_core_poll(n, ++tick);
    assert(last[33] == (FIN | ACK) && rd32(last + 24) == ack + 128);
}
static void blocked_deadline(bool report, bool wrap) {
    uint32_t ack;
    tick = wrap ? (uint32_t)(0u - 15001u - (report ? 1000u : 0u)) : 100;
    if (report) {
        begin_speed(5700, "U 60\n", &ack);
        input(5700, 106, ack, 0, PSH | ACK, "GO\n");
        tick += 1000;
    } else {
        ack = speed_open(5700, 100, 1460);
        ++tick;
        input(5700, 101, ack, 0, PSH | ACK, "U 60\n");
    }
    uint32_t end = tick + 15000;
    if (wrap) assert(end == 0);
    single_packet_adapter();
    reject_output = true; /* even the timeout's RST cannot enter the link */
    nat_core_poll(n, tick);
    unsigned before = emitted;
    tick = end - 1;
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 1);
    tick = end;
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 0 && emitted == before);
    reject_output = false;
    occupied = false;
    speed_open(5700, 100, 1460); /* slot and same tuple can immediately be reused */
}
static void t_blocked_handshake_release_exact_deadline(void) { blocked_deadline(false, false); }
static void t_blocked_handshake_release_zero_deadline(void) { blocked_deadline(false, true); }
static void t_blocked_report_release_exact_deadline(void) { blocked_deadline(true, false); }
static void t_blocked_report_release_zero_deadline(void) { blocked_deadline(true, true); }
static void t_local_syn_busy_lifetime_wrap_and_reuse(void) {
    tick = (uint32_t)(0u - 30000u);
    single_packet_adapter();
    reject_output = true;
    input(5700, 100, 0, 1460, SYN, NULL);
    tick = UINT32_MAX;
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 1);
    tick = 0;
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 0 && emitted == 0);
    reject_output = false;
    speed_open(5700, 100, 1460);
}
static void t_local_fin_busy_lifetime_wrap_and_reuse(void) {
    uint32_t ack = speed_open(5700, 100, 1460);
    tick = (uint32_t)(0u - 30000u);
    input(5700, 101, ack, 1460, FIN | ACK, NULL);
    single_packet_adapter();
    reject_output = true;
    tick = UINT32_MAX;
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 1);
    tick = 0;
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 0);
    reject_output = false;
    speed_open(5700, 100, 1460);
}
static void t_abort_download_preserves_remote_stream(void) {
    uint32_t ack, remote;
    begin_speed(5700, "D 7200\n", &ack);
    input(5700, 108, ack, 1460, PSH | ACK, "GO\n");
    remote = remote_open(1, 5701);
    assert(nat_core_tcp_received(n, 1, (const uint8_t *)"remote", 6, tick));
    single_packet_adapter();
    nat_core_poll(n, ++tick);
    assert(rd16(last + 22) == 5701); /* cursor follows the earlier OK */
    send_tcp_to(REMOTE, 5701, 80, ACK, 101, remote + 2, 1460, NULL);
    occupied = false;
    nat_core_poll(n, ++tick);
    assert(rd16(last + 22) == 5700 && last_len == 1500);
    input(5700, 111, ack, 0, RST | ACK, NULL); /* cancel with local data in flight */
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).speed == 0 && nat_core_stats(n).tcp == 1);
    occupied = false;
    tick += 2501;
    nat_core_poll(n, tick);
    assert(last_len == 44 && rd16(last + 22) == 5701);
    assert(rd32(last + 24) == remote + 2 && !memcmp(last + 40, "mote", 4));
    send_tcp_to(REMOTE, 5701, 80, ACK, 101, remote + 6, 1460, NULL);
    assert(recved == 9); /* three OK bytes and six remote bytes, once each */
}
static void t_upload_lost_ack_does_not_duplicate_count(void) {
    uint32_t ack;
    begin_speed(5700, "U 60\n", &ack);
    input(5700, 106, ack, 1460, PSH | ACK, "GO\n");
    single_packet_adapter();
    reject_output = true;
    input(5700, 109, ack, 1460, PSH | ACK, "payload");
    reject_output = false;
    input(5700, 109, ack, 1460, PSH | ACK, "payload");
    assert(last[33] == ACK && rd32(last + 28) == 116);
    occupied = false;
    tick += 1000;
    nat_core_poll(n, tick);
    assert(last_len == 42 && !memcmp(last + 40, "7\n", 2));
}

static const test_t validation_tests[] = {
    TEST(t_upload_fragment_duplicate_tail_wrap), TEST(t_download_window_stop_and_wait),
    TEST(t_download_small_window_progress), TEST(t_download_fin_discards_unsent_tail),
    TEST(t_download_maximum_lifetime), TEST(t_report_maximum_lifetime),
    TEST(t_local_does_not_starve_remote), TEST(t_local_slots_leave_remote_capacity),
    TEST(t_local_rst_fin_time_wait_idle), TEST(t_local_busy_retries),
    TEST(t_local_handshake_fin_retries), TEST(t_local_control_deadline_and_garbage),
    TEST(t_upload_binary_payload_and_partial_report_ack), TEST(t_invalid_requests),
    TEST(t_overlong_control_rejected_at_limit),
    TEST(t_download_fin_discards_partial_window_tail),
    TEST(t_report_deadline_wraps_to_zero), TEST(t_report_zero_window_has_send_timeout),
    TEST(t_local_source_remote_data_retry_fair), TEST(t_remote_source_remote_data_retry_fair),
    TEST(t_remote_source_local_data_retry_fair), TEST(t_local_source_remote_syn_retry_fair),
    TEST(t_local_source_remote_fin_retry_fair), TEST(t_round_robin_stream_integrity_all_slots),
    TEST(t_remote_busy_deferral_and_full_retry_budget),
    TEST(t_busy_local_source_remote_data_retry_fair),
    TEST(t_busy_remote_source_remote_data_retry_fair),
    TEST(t_busy_remote_source_local_data_retry_fair),
    TEST(t_busy_local_source_remote_syn_retry_fair),
    TEST(t_busy_local_source_remote_fin_retry_fair),
    TEST(t_input_ack_cannot_starve_ready_remote),
    TEST(t_handshake_zero_window_has_deadline),
    TEST(t_download_exact_deadline_and_clock_wrap), TEST(t_report_small_window_tail_deadline),
    TEST(t_mixed_ready_flows_one_rotation), TEST(t_rejected_output_keeps_sequence_and_rotation),
    TEST(t_fin_partial_ack_retries_only_outstanding_prefix),
    TEST(t_blocked_handshake_release_exact_deadline), TEST(t_blocked_handshake_release_zero_deadline),
    TEST(t_blocked_report_release_exact_deadline), TEST(t_blocked_report_release_zero_deadline),
    TEST(t_local_syn_busy_lifetime_wrap_and_reuse), TEST(t_local_fin_busy_lifetime_wrap_and_reuse),
    TEST(t_abort_download_preserves_remote_stream), TEST(t_upload_lost_ack_does_not_duplicate_count)
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
