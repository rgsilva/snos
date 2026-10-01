/* Actual adapter/core/link and SDK lwIP, plus the actual console receiver. */
#define main baseline_main
#include "revalidation_lwip_harness.c"
#undef main
void console_peer_init(void);
void console_peer_receive(const uint8_t *, uint8_t *);
unsigned console_peer_keys(void);
void console_peer_enable_ip(void);
int console_peer_ip_write(const uint8_t *, int);
void console_peer_open_channel(void);
int console_peer_channel_failed(void);

static void ack(const uint8_t *f) {
    uint8_t a[LINK_FRAME] = {0}; a[1] = f[0];
    for (unsigned i = 0; i < LINK_FRAME; ++i) if (i != 3) a[3] += a[i];
    assert(link_on_rx(a).valid);
}
static void sync_peer(void) {
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    console_peer_init();
    /* First exchange consumes the console's own startup SYN. */
    link_next_out(f); console_peer_receive(f, reply);
    assert(link_on_rx(reply).valid);
    assert(link_push(CH_KBD, (const uint8_t *)"!", 1));
    link_next_out(f); console_peer_receive(f, reply);
    assert(link_on_rx(reply).valid);
    link_next_out(f); assert(!(f[2] & F_SYN));
    console_peer_receive(f, reply); assert(link_on_rx(reply).valid);
}
static void t_restart_does_not_duplicate_delivered_keyboard(void) {
    sync_peer(); unsigned before = console_peer_keys();
    uint8_t f[LINK_FRAME], reply[LINK_FRAME], old[10] = {0};
    assert(link_push(CH_KBD, (const uint8_t *)"a", 1));
    assert(link_push(CH_IP, old, sizeof(old)));
    link_next_out(f); console_peer_receive(f, reply);
    assert(console_peer_keys() == before + 1);
    /* The frame arrived, but its ACK has not reached Pico when WiFi drops. */
    nat_configure(false); link_next_out(f); console_peer_receive(f, reply);
    fprintf(stderr, "key deliveries across restart: %u\n", console_peer_keys() - before);
    assert(console_peer_keys() == before + 1);
}
static void t_restart_replays_undelivered_keyboard(void) {
    sync_peer(); unsigned before = console_peer_keys();
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    assert(link_push(CH_KBD, (const uint8_t *)"a", 1));
    link_next_out(f); /* This frame is lost. */
    nat_configure(false); link_next_out(f); console_peer_receive(f, reply);
    assert(console_peer_keys() == before + 1);
}
static void t_restart_full_refilled_queue_keeps_inflight_key(void) {
    sync_peer(); unsigned before = console_peer_keys();
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    assert(link_push(CH_KBD, (const uint8_t *)"a", 1));
    link_next_out(f); /* lost; its consumed chunk slot is now free */
    for (unsigned i = 0; i < 32; ++i)
        assert(link_push(CH_KBD, (const uint8_t *)"b", 1));
    nat_configure(false);
    for (unsigned i = 0; i < 4; ++i) {
        link_next_out(f); console_peer_receive(f, reply); assert(link_on_rx(reply).valid);
    }
    fprintf(stderr, "key deliveries from 33 accepted bytes: %u\n", console_peer_keys() - before);
    assert(console_peer_keys() == before + 33);
}
static void t_normal_syn_ack_does_not_fail_live_channel(void) {
    uint8_t f[LINK_FRAME], reply[LINK_FRAME]; console_peer_init();
    assert(link_push(CH_KBD, (const uint8_t *)"a", 1));
    link_next_out(f); assert(f[2] & F_SYN); console_peer_receive(f, reply);
    console_peer_open_channel();
    /* A retransmission of the adopted base is harmless. */
    console_peer_receive(f, reply); assert(!console_peer_channel_failed());
    ack(f); assert(link_push(CH_KBD, (const uint8_t *)"b", 1));
    link_next_out(f); assert(!(f[2] & F_SYN)); console_peer_receive(f, reply);
    assert(!console_peer_channel_failed());
    assert(console_peer_keys() == 2);
}
static void t_saved_connect_generation_is_rejected_after_reuse(void) {
    console_tcp(2, 100, 0, NULL);
    struct tcp_pcb *p = tcp_pcbs[0]; assert(p);
    tcp_connected(p->callback_arg, p, ERR_OK);
    connected_t saved = connected_pending[0];
    console_tcp(4, 101, 0, NULL); console_tcp(2, 200, 0, NULL);
    /* Independently exercise the comparison, beyond eager clearing. */
    connected_pending[0] = saved;
    unsigned before = nat_stats().out; nat_poll();
    assert(nat_stats().out == before);
}
static void t_saved_accept_generation_is_rejected_after_reuse(void) {
    assert(nat_forward_add(8080, 1));
    struct tcp_pcb *p = tcp_new(); assert(p); p->state = ESTABLISHED;
    assert(accepted((void *)(uintptr_t)8080, p, ERR_OK) == ERR_OK);
    accepted_t saved = accepted_pending[0]; assert(saved.slot == 0);
    tcp_abort(p); console_tcp(2, 200, 0, NULL);
    accepted_pending[0] = saved;
    unsigned before = nat_stats().out; nat_poll();
    assert(nat_stats().out == before);
}
/* Guard-contract probes, not a claim that serialized lwIP delays callbacks
 * beyond PCB destruction. A context pointer must retain its original epoch
 * if this API is intended to defend against stale callbacks at all. */
static void t_tcp_callback_context_rejects_old_lifetime(void) {
    console_tcp(2, 100, 0, NULL);
    void *old_arg = tcp_pcbs[0]->callback_arg;
    console_tcp(4, 101, 0, NULL); console_tcp(2, 200, 0, NULL);
    struct tcp_pcb unrelated = {0}; unsigned slot; uint32_t generation;
    assert(!tcp_callback_slot(old_arg, &unrelated, &slot, &generation));
}
static void t_udp_callback_context_rejects_old_lifetime(void) {
    console_udp(0xc0000235); void *old_arg = udp_slots[0]->recv_arg;
    clock_ms += 60001; nat_poll(); assert(!udp_slots[0]);
    console_udp(0xc0000236);
    struct udp_pcb unrelated = {0}; unsigned slot; uint32_t generation;
    assert(!udp_callback_slot(old_arg, &unrelated, &slot, &generation));
}
static void t_restart_discards_console_inflight_ip_tail(void) {
    sync_peer(); console_peer_enable_ip();
    uint8_t stream[102] = {100, 0, 0x45}, f[LINK_FRAME], reply[LINK_FRAME];
    /* Payload bytes at the fragment boundary look like a legal new length. */
    stream[58] = 100;
    assert(console_peer_ip_write(stream, sizeof(stream)) == sizeof(stream));
    link_next_out(f); console_peer_receive(f, reply);
    link_rx_t r = link_on_rx(reply); assert(r.valid && r.count == 1);
    assert(r.records[0].ch == CH_IP && r.records[0].len == 58);
    nat_ip_bytes(r.records[0].data, r.records[0].len); assert(ipused == 58);
    /* ACK the prefix and let console build the tail, which is lost. */
    link_next_out(f); console_peer_receive(f, reply);
    assert(reply[2] & F_DATA);
    nat_configure(false); nat_configure(true); assert(ipused == 0);
    link_next_out(f); console_peer_receive(f, reply);
    r = link_on_rx(reply); assert(r.valid);
    for (size_t i = 0; i < r.count; ++i)
        if (r.records[i].ch == CH_IP) nat_ip_bytes(r.records[i].data, r.records[i].len);
    fprintf(stderr, "old tail retained in fresh Pico assembler: %zu bytes\n", ipused);
    /* The application learns the new HELLO address and submits a new packet. */
    console_peer_enable_ip();
    uint8_t fresh[42] = {40, 0, 0x45};
    assert(console_peer_ip_write(fresh, sizeof(fresh)) == sizeof(fresh));
    link_next_out(f); console_peer_receive(f, reply); r = link_on_rx(reply);
    for (size_t i = 0; i < r.count; ++i)
        if (r.records[i].ch == CH_IP) nat_ip_bytes(r.records[i].data, r.records[i].len);
    fprintf(stderr, "assembler after complete fresh packet: %zu bytes\n", ipused);
    assert(ipused == 0 && "fresh packet spliced into pre-restart console tail");
}
static const struct { const char *name; void (*run)(void); } cases[] = {
    TEST(t_restart_does_not_duplicate_delivered_keyboard),
    TEST(t_restart_replays_undelivered_keyboard),
    TEST(t_restart_full_refilled_queue_keeps_inflight_key),
    TEST(t_normal_syn_ack_does_not_fail_live_channel),
    TEST(t_saved_connect_generation_is_rejected_after_reuse),
    TEST(t_saved_accept_generation_is_rejected_after_reuse),
    TEST(t_tcp_callback_context_rejects_old_lifetime),
    TEST(t_udp_callback_context_rejects_old_lifetime),
    TEST(t_restart_discards_console_inflight_ip_tail),
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(cases)/sizeof(*cases); ++i) puts(cases[i].name);
        return 0;
    }
    char *setup[] = {argv[0], "--setup-only"}; assert(baseline_main(2, setup) == 2);
    for (size_t i = 0; i < sizeof(cases)/sizeof(*cases); ++i)
        if (!strcmp(argv[1], cases[i].name)) {
            cases[i].run(); assert(!locks); printf("PASS %s\n", cases[i].name); return 0;
        }
    return 2;
}
