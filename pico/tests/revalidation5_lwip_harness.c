/* Fifth-pass cases extend the real SDK/adapter/console integration harness. */
#define main baseline_main
#include "revalidation_lwip_harness.c"
#undef main
void console_peer_init(void);
void console_peer_receive(const uint8_t *, uint8_t *);
unsigned console_peer_keys(void);
const uint8_t *console_peer_key_values(void);
void console_peer_enable_ip(void);
int console_peer_ip_write(const uint8_t *, int);
static void sync_peer(void) {
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    console_peer_init();
    link_next_out(f); console_peer_receive(f, reply);
    assert(link_on_rx(reply).valid);
    assert(link_push(CH_KBD, (const uint8_t *)"!", 1));
    link_next_out(f); console_peer_receive(f, reply);
    assert(link_on_rx(reply).valid);
    link_next_out(f); assert(!(f[2] & F_SYN));
    console_peer_receive(f, reply); assert(link_on_rx(reply).valid);
}

static bool exchange_resets(void) {
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    link_next_out(f); console_peer_receive(f, reply);
    link_rx_t r = link_on_rx(reply);
    assert(r.valid);
    /* Match main.c: a remote restart resets adapter state, not link state. */
    if (r.reset) { ctrl_reset(); nat_reset(); }
    for (size_t i = 0; i < r.count; ++i) {
        if (r.records[i].ch == CH_CTRL) ctrl_bytes(r.records[i].data, r.records[i].len);
        else if (r.records[i].ch == CH_IP) nat_ip_bytes(r.records[i].data, r.records[i].len);
    }
    return r.reset;
}
static unsigned drain_console_packet(void) {
    unsigned bytes = 0;
    for (unsigned n = 0; n < 8; ++n) {
        /* Consume records while the frame backing them is still live. */
        uint8_t f[LINK_FRAME], reply[LINK_FRAME];
        link_next_out(f); console_peer_receive(f, reply);
        link_rx_t r = link_on_rx(reply); assert(r.valid && !r.reset);
        for (size_t i = 0; i < r.count; ++i)
            if (r.records[i].ch == CH_IP) {
                bytes += r.records[i].len;
                nat_ip_bytes(r.records[i].data, r.records[i].len);
            }
    }
    return bytes;
}
static void send_fresh_console_packet(void) {
    uint8_t fresh[42] = {40, 0, 0x45};
    console_peer_enable_ip();
    assert(console_peer_ip_write(fresh, sizeof(fresh)) == sizeof(fresh));
    unsigned bytes = drain_console_packet();
    fprintf(stderr, "fresh console IP bytes delivered: %u of 42\n", bytes);
    assert(bytes == sizeof(fresh));
    assert(ipused == 0);
}
static void t_idle_syn_mode_restart_allows_new_console_data(void) {
    sync_peer();
    nat_configure(false); nat_configure(true);
    for (unsigned i = 0; i < 3; ++i) assert(!exchange_resets());
    send_fresh_console_packet();
}
static void finish_console_syn(void) {
    /* Send and ACK one console data frame, then observe its non-SYN frame. */
    send_fresh_console_packet();
    assert(!exchange_resets());
}
static void t_established_mode_restart_allows_new_console_data(void) {
    sync_peer(); finish_console_syn();
    nat_configure(false); nat_configure(true);
    for (unsigned i = 0; i < 3; ++i) assert(!exchange_resets());
    send_fresh_console_packet();
}
static void t_second_restart_before_console_data_allows_progress(void) {
    sync_peer(); finish_console_syn();
    for (unsigned round = 0; round < 2; ++round) {
        nat_configure(false); nat_configure(true);
        for (unsigned i = 0; i < 3; ++i) assert(!exchange_resets());
    }
    send_fresh_console_packet();
}
static void t_console_sequence_wrap_restart_allows_progress(void) {
    sync_peer();
    uint8_t fresh[42] = {40, 0, 0x45};
    console_peer_enable_ip();
    /* Initial console SYN base is 1. After 255 data ACKs the next sequence
     * is 0, so its restart increments back to that old SYN base, 1. */
    for (unsigned i = 0; i < 255; ++i) {
        assert(console_peer_ip_write(fresh, sizeof(fresh)) == sizeof(fresh));
        assert(drain_console_packet() == sizeof(fresh));
    }
    nat_configure(false); nat_configure(true);
    for (unsigned i = 0; i < 3; ++i) assert(!exchange_resets());
    send_fresh_console_packet();
}

static void t_partial_keyboard_chunk_preserves_unframed_suffix(void) {
    sync_peer(); unsigned before = console_peer_keys();
    uint8_t f[LINK_FRAME], reply[LINK_FRAME], expected[100];
    for (unsigned i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)(i + 1);
    assert(link_push(CH_KBD, expected, sizeof(expected)));
    link_next_out(f); assert(f[5] == 58);
    console_peer_receive(f, reply); /* delivered; ACK lost */
    assert(console_peer_keys() == before + 58);
    nat_configure(false);
    for (unsigned i = 0; i < 8; ++i) {
        link_next_out(f); console_peer_receive(f, reply);
        assert(link_on_rx(reply).valid);
    }
    assert(console_peer_keys() == before + sizeof(expected));
    assert(!memcmp(console_peer_key_values() + before, expected, sizeof(expected)));
}
static void t_tcp_old_generation_rejected_with_current_pcb_and_without_pcb(void) {
    console_tcp(2, 100, 0, NULL);
    void *old = tcp_pcbs[0]->callback_arg;
    console_tcp(4, 101, 0, NULL); console_tcp(2, 200, 0, NULL);
    struct tcp_pcb *current = tcp_pcbs[0]; assert(current);
    unsigned slot; uint32_t generation;
    assert(tcp_callback_slot(current->callback_arg, current, &slot, &generation));
    assert(!tcp_callback_slot(old, current, &slot, &generation));
    assert(!tcp_callback_slot(old, NULL, &slot, &generation));
    tcp_connected(old, current, ERR_OK); assert(!connected_pending[0].result);
    nat_tcp_err(old, ERR_RST); assert(tcp_pcbs[0] == current);
}
static void t_udp_old_generation_rejected_with_current_pcb(void) {
    console_udp(0xc0000235); void *old = udp_slots[0]->recv_arg;
    clock_ms += 60001; nat_poll(); assert(!udp_slots[0]);
    console_udp(0xc0000236);
    unsigned slot; uint32_t generation;
    assert(udp_callback_slot(udp_slots[0]->recv_arg, udp_slots[0], &slot, &generation));
    assert(!udp_callback_slot(old, udp_slots[0], &slot, &generation));
}
static const struct { const char *name; void (*run)(void); } cases5[] = {
    TEST(t_idle_syn_mode_restart_allows_new_console_data),
    TEST(t_established_mode_restart_allows_new_console_data),
    TEST(t_second_restart_before_console_data_allows_progress),
    TEST(t_console_sequence_wrap_restart_allows_progress),
    TEST(t_partial_keyboard_chunk_preserves_unframed_suffix),
    TEST(t_tcp_old_generation_rejected_with_current_pcb_and_without_pcb),
    TEST(t_udp_old_generation_rejected_with_current_pcb),
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(cases5)/sizeof(*cases5); ++i) puts(cases5[i].name);
        return 0;
    }
    char *setup[] = {argv[0], "--setup-only"}; assert(baseline_main(2, setup) == 2);
    for (size_t i = 0; i < sizeof(cases5)/sizeof(*cases5); ++i)
        if (!strcmp(argv[1], cases5[i].name)) {
            cases5[i].run(); assert(!locks); printf("PASS %s\n", cases5[i].name); return 0;
        }
    return 2;
}
