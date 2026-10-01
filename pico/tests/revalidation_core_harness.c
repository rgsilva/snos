/* Reuse the original peer without changing any of its assertions. */
#define main original_core_main
#include "nat_harness.c"
#undef main

static bool wire_busy;
static bool busy_probe(void *arg) { (void)arg; return wire_busy; }
static void install_busy_probe(void) {
    nat_core_set_adapter(n, NULL, busy_probe, NULL, NULL);
}
static void t_f16_rto_starts_after_busy_link_drains(void) {
    establish(100); install_busy_probe();
    assert(nat_core_tcp_received(n, 0, (const unsigned char *)"abc", 3, tick));
    nat_core_poll(n, tick);
    unsigned before = emitted;
    wire_busy = true; tick += 3000; nat_core_poll(n, tick);
    assert(emitted == before);
    wire_busy = false; nat_core_poll(n, ++tick);
    /* slirp starts a fresh RTO when the wire clears: allow the ACK to return. */
    assert(emitted == before);
    tick += 2501; nat_core_poll(n, tick); assert(emitted == before + 1);
}
static void t_f16_permanently_busy_link_still_expires_idle_flow(void) {
    establish(100); install_busy_probe();
    assert(nat_core_tcp_received(n, 0, (const unsigned char *)"abc", 3, tick));
    nat_core_poll(n, tick); wire_busy = true;
    nat_core_poll(n, tick + 300000); assert(nat_core_stats(n).tcp == 1);
    nat_core_poll(n, tick + 300001); assert(nat_core_stats(n).tcp == 0 && aborts == 1);
}
static void t_f16_retry_limit_does_not_emit_an_extra_segment(void) {
    send_tcp(SYN, 100, 0, 1460, NULL); nat_core_tcp_connected(n, 0, 0, tick);
    for (unsigned i = 0; i < 8; ++i) { tick += 2501; nat_core_poll(n, tick); }
    unsigned before = emitted;
    tick += 2501; nat_core_poll(n, tick);
    assert((last[33] & RST) && emitted == before + 1);
}
static void t_f4_coalesced_receive_delivers_both_segments_with_credit(void) {
    uint32_t seq = establish(100);
    unsigned char body[2920];
    for (size_t i = 0; i < sizeof(body); ++i) body[i] = (unsigned char)i;
    assert(nat_core_tcp_received(n, 0, body, sizeof(body), tick));
    nat_core_poll(n, ++tick);
    assert(last_len == 1500 && !memcmp(last + 40, body, 1460) && recved == 0);
    send_tcp(ACK, 101, seq + 1460, 1460, NULL);
    assert(recved == 1460); nat_core_poll(n, ++tick);
    assert(last_len == 1500 && rd32(last + 24) == seq + 1460);
    assert(!memcmp(last + 40, body + 1460, 1460));
    send_tcp(ACK, 101, seq + 2920, 1460, NULL); assert(recved == 2920);
}
static const test_t revalidation_tests[] = {
    TEST(t_f16_rto_starts_after_busy_link_drains),
    TEST(t_f16_permanently_busy_link_still_expires_idle_flow),
    TEST(t_f16_retry_limit_does_not_emit_an_extra_segment),
    TEST(t_f4_coalesced_receive_delivers_both_segments_with_credit),
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(revalidation_tests)/sizeof(*revalidation_tests); ++i)
            puts(revalidation_tests[i].name);
        return 0;
    }
    nat_ops_t ops = {emit, connect_, write_, shutdown_, abort_, udp, fadd, fdel, got, NULL};
    assert(nat_core_size() <= sizeof(store)); nat_core_init(n, &ops);
    for (size_t i = 0; i < sizeof(revalidation_tests)/sizeof(*revalidation_tests); ++i)
        if (!strcmp(argv[1], revalidation_tests[i].name)) {
            revalidation_tests[i].run(); printf("PASS %s\n", revalidation_tests[i].name); return 0;
        }
    return 2;
}
