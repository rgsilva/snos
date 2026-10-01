#define main baseline_main
#include "nat_harness.c"
#undef main
static bool wire_busy;
static bool busy_probe(void *arg) { (void)arg; return wire_busy; }
static void install_busy_probe(void) { nat_core_set_adapter(n, NULL, busy_probe, NULL, NULL); }

static void t_data_retry_ceiling_emits_only_reset(void) {
    uint32_t seq = establish(100);
    assert(nat_core_tcp_received(n, 0, (const uint8_t *)"abc", 3, tick));
    nat_core_poll(n, tick);
    for (unsigned i = 0; i < 120; ++i) {
        tick += 2501;
        send_tcp(ACK, 101, seq, 1460, NULL); /* refresh idle, no data ACK */
        nat_core_poll(n, tick);
    }
    unsigned before = emitted;
    tick += 2501; send_tcp(ACK, 101, seq, 1460, NULL); nat_core_poll(n, tick);
    assert(emitted == before + 1 && (last[33] & RST));
}
static void t_fin_retry_ceiling_emits_nothing(void) {
    establish(100); nat_core_tcp_eof(n, 0, tick); nat_core_poll(n, tick);
    assert(last[33] == (FIN | ACK));
    for (unsigned i = 0; i < 8; ++i) { tick += 2501; nat_core_poll(n, tick); }
    unsigned before = emitted;
    tick += 2501; nat_core_poll(n, tick);
    assert(emitted == before && nat_core_stats(n).tcp == 0);
}
static void t_handshake_busy_defers_full_rto(void) {
    send_tcp(SYN, 100, 0, 1460, NULL); nat_core_tcp_connected(n, 0, 0, tick);
    install_busy_probe(); unsigned before = emitted;
    wire_busy = true; tick += 3000; nat_core_poll(n, tick);
    wire_busy = false; nat_core_poll(n, ++tick); assert(emitted == before);
    tick += 2501; nat_core_poll(n, tick); assert(emitted == before + 1);
}
static void t_fin_busy_defers_full_rto(void) {
    establish(100); nat_core_tcp_eof(n, 0, tick); nat_core_poll(n, tick);
    install_busy_probe(); unsigned before = emitted;
    wire_busy = true; tick += 3000; nat_core_poll(n, tick);
    wire_busy = false; nat_core_poll(n, ++tick); assert(emitted == before);
    tick += 2501; nat_core_poll(n, tick); assert(emitted == before + 1);
}
static const test_t round3[] = {
    TEST(t_data_retry_ceiling_emits_only_reset),
    TEST(t_fin_retry_ceiling_emits_nothing),
    TEST(t_handshake_busy_defers_full_rto),
    TEST(t_fin_busy_defers_full_rto),
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(round3)/sizeof(*round3); ++i) puts(round3[i].name);
        return 0;
    }
    nat_ops_t ops = {emit, connect_, write_, shutdown_, abort_, udp, fadd, fdel, got, NULL};
    assert(nat_core_size() <= sizeof(store)); nat_core_init(n, &ops);
    for (size_t i = 0; i < sizeof(round3)/sizeof(*round3); ++i)
        if (!strcmp(argv[1], round3[i].name)) {
            round3[i].run(); printf("PASS %s\n", round3[i].name); return 0;
        }
    return 2;
}
