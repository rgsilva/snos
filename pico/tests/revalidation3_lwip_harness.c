/* Reuse fixtures, leaving every first/second-pass assertion unchanged. */
#define main baseline_main
#include "revalidation_lwip_harness.c"
#undef main

static void ack_frame(const uint8_t *frame) {
    uint8_t ack[LINK_FRAME] = {0};
    ack[1] = frame[0];
    for (size_t i = 0; i < sizeof(ack); ++i) if (i != 3) ack[3] += ack[i];
    assert(link_on_rx(ack).valid);
}
static void synchronize(void) {
    uint8_t f[LINK_FRAME];
    assert(link_push(CH_KBD, (const uint8_t *)"!", 1));
    link_next_out(f); ack_frame(f);
    link_next_out(f); assert(!(f[2] & F_SYN));
}

static void t_purge_keeps_queued_bluetooth_keys(void) {
    synchronize();
    uint8_t old[100] = {0}, f[LINK_FRAME];
    assert(link_push(CH_IP, old, sizeof(old)));
    kbd_init(); const uint8_t key[8] = {0, 0, 4};
    kbd_hid_boot_report(key); kbd_poll();
    nat_configure(false); link_next_out(f);
    assert(f[4] == CH_KBD && f[5] == 1 && f[6] == 'a');
}
static void t_connect_completion_does_not_leak_after_abort(void) {
    console_tcp(2, 100, 0, NULL);
    struct tcp_pcb *pcb = tcp_pcbs[0]; assert(pcb);
    pcb->state = ESTABLISHED;
    tcp_connected(NULL, pcb, ERR_OK); /* callback before foreground poll */
    console_tcp(4, 101, 0, NULL); /* old console flow dies */
    console_tcp(2, 200, 0, NULL); /* same slot, new connection */
    assert(tcp_pcbs[0] && tcp_pcbs[0]->state == SYN_SENT);
    unsigned before = nat_stats().out;
    nat_poll();
    assert(nat_stats().out == before && "new connection is not connected yet");
}
static void t_connect_completion_does_not_leak_after_error(void) {
    console_tcp(2, 100, 0, NULL);
    struct tcp_pcb *pcb = tcp_pcbs[0]; assert(pcb);
    pcb->state = ESTABLISHED;
    tcp_connected(NULL, pcb, ERR_OK);
    tcp_abort(pcb); /* real lwIP frees it and calls nat_tcp_err */
    assert(!tcp_pcbs[0]); nat_poll(); drain_ip();
    /* To exercise reuse BEFORE process_pending_tcp, repeat on a fresh flow. */
    console_tcp(2, 100, 0, NULL); pcb = tcp_pcbs[0]; assert(pcb);
    pcb->state = ESTABLISHED; tcp_connected(NULL, pcb, ERR_OK); tcp_abort(pcb);
    console_tcp(2, 200, 0, NULL);
    unsigned before = nat_stats().out; nat_poll();
    assert(nat_stats().out == before && "stale error-path completion leaked");
}
static void t_detached_drains_fin_and_reuses_slot(void) {
    t_f6_orderly_close_keeps_pending_console_payload();
    uint32_t seq = get32(packet + 24);
    /* Allocate the newly freed PCB memory to an unrelated connection. */
    struct tcp_pcb *other = tcp_new(); assert(other);
    other->rcv_wnd = 123;
    console_tcp(16, 102, seq + 10, NULL);
    assert(other->rcv_wnd == 123); /* no credit through the freed pointer */
    nat_poll(); assert(drain_ip() == 1 && packet[33] == 17);
    console_tcp(16, 102, seq + 11, NULL);
    clock_ms += 4000; nat_poll(); assert(nat_stats().tcp == 1);
    ++clock_ms; nat_poll(); assert(nat_stats().tcp == 0);
    assert(other->rcv_wnd == 123); tcp_abort(other);
    assert(establish());
}
static void t_detached_unacked_reply_expires_idle(void) {
    t_f6_orderly_close_keeps_pending_console_payload();
    clock_ms += 300000; nat_poll(); assert(nat_stats().tcp == 1);
    ++clock_ms; nat_poll(); assert(nat_stats().tcp == 0 && !tcp_pcbs[0]);
}
static void t_detached_busy_reply_expires_idle(void) {
    t_f6_orderly_close_keeps_pending_console_payload();
    uint8_t occupied[100] = {0}; assert(link_push(CH_IP, occupied, sizeof(occupied)));
    clock_ms += 300001; nat_poll(); assert(nat_stats().tcp == 0 && !tcp_pcbs[0]);
}
static void t_detached_reset_and_reuse_safe(void) {
    t_f6_orderly_close_keeps_pending_console_payload();
    nat_configure(false); nat_configure(true);
    assert(!nat_stats().tcp); assert(establish());
}
static void t_hooks_survive_repeated_production_reset_paths(void) {
    for (unsigned i = 0; i < 3; ++i) {
        nat_reset(); nat_configure(false); nat_configure(true);
        t_f9_reset_retains_callback_ring_drop_accounting();
    }
}
static const struct { const char *name; void (*run)(void); } round3[] = {
    TEST(t_purge_keeps_queued_bluetooth_keys),
    TEST(t_connect_completion_does_not_leak_after_abort),
    TEST(t_connect_completion_does_not_leak_after_error),
    TEST(t_detached_drains_fin_and_reuses_slot),
    TEST(t_detached_unacked_reply_expires_idle),
    TEST(t_detached_busy_reply_expires_idle),
    TEST(t_detached_reset_and_reuse_safe),
    TEST(t_hooks_survive_repeated_production_reset_paths),
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(round3)/sizeof(*round3); ++i) puts(round3[i].name);
        return 0;
    }
    /* baseline_main initializes the real stack even for an unknown name. */
    char *setup[] = {argv[0], "--setup-only"}; assert(baseline_main(2, setup) == 2);
    for (size_t i = 0; i < sizeof(round3)/sizeof(*round3); ++i)
        if (!strcmp(argv[1], round3[i].name)) {
            round3[i].run(); assert(!locks); printf("PASS %s\n", round3[i].name); return 0;
        }
    return 2;
}
