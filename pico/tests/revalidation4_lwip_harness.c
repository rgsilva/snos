/* Actual adapter/core/link and SDK lwIP, plus the actual console receiver. */
#define main baseline_main
#include "revalidation_lwip_harness.c"
#undef main
void console_peer_init(void);
void console_peer_receive(const uint8_t *, uint8_t *);
unsigned console_peer_keys(void);
const uint8_t *console_peer_key_values(void);
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

static const struct { const char *name; void (*run)(void); } cases[] = {
    TEST(t_restart_does_not_duplicate_delivered_keyboard),
    TEST(t_normal_syn_ack_does_not_fail_live_channel),
    TEST(t_saved_connect_generation_is_rejected_after_reuse),
    TEST(t_saved_accept_generation_is_rejected_after_reuse),
    TEST(t_tcp_callback_context_rejects_old_lifetime),
    TEST(t_udp_callback_context_rejects_old_lifetime),
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
