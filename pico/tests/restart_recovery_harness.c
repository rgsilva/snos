/* Peer restarts, past restart_validation_harness.c's case. Real Pico link,
 * NAT and lwIP, and the real console receiver, as in transition_harness.c.
 *
 *   p_*  the Pico restarts under a console that lives on;
 *   c_*  the console restarts under a Pico that lives on (the mirror).
 *
 * After a restart the survivor's frames already on the wire were cut for
 * the peer that is gone, and may begin in the middle of a datagram. Each
 * property arranges for one such frame to reach the restarted side by a
 * path the simpler case does not take, and requires that the next fresh
 * datagram still arrives whole and nothing stale is parsed. */
#define TRANSITION_NO_MAIN 1
#include "transition_harness.c"

/* A 1000-byte ping whose bytes at stream offset `at` read as length 512:
 * valid as a datagram, and a plausible prefix to a parser that starts there. */
static void poisoned_stream(uint8_t *s, unsigned tag, size_t at) {
    stream_bytes(s, 1000, tag);
    s[at] = 0; s[at + 1] = 2;
    s[24] = s[25] = 0;
    put16(s + 24, checksum(s + 22, 980, 0));
}
/* The ending every p_ property shares: the console adopts the restart, the
 * restarted Pico answers the HELLO that follows (ip.c's, after the adoption:
 * one queued before it is discarded with the rest, net.c), then a fresh ping
 * crosses whole, is echoed, and leaves the Pico's assembler empty. */
static void hello_then_fresh_ping(void) {
    pump(30);
    unsigned hellos = console_peer_hellos();
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    ctrl_address((const uint8_t *)"\xc0\0\2\x0a"); pump(20);
    assert(console_peer_hellos() == hellos + 1);
    assert(!strcmp(console_peer_address(), "192.0.2.10"));
    uint8_t fresh[42]; stream_bytes(fresh, 40, 91);
    console_peer_enable_ip();
    received_len = reverse_len = 0;
    assert(console_peer_ip_write(fresh, sizeof fresh) == sizeof fresh);
    pump(80);
    if (reverse_len != sizeof fresh || received_len != sizeof fresh || ipused)
        fprintf(stderr, "fresh wire=%zu, echo=%zu, parser=%zu\n", reverse_len, received_len, ipused);
    assert(reverse_len == sizeof fresh && !memcmp(reverse, fresh, sizeof fresh));
    assert(received_len == sizeof fresh);
    assert(ipused == 0);
}
static void pico_restarts(void) { link_init(); ctrl_reset(); nat_reset(); ip_stream_reset(); }

/* Two stale console frames in flight; the restarted Pico gets the first,
 * and the second's first copy is lost with the old Pico. So the Pico first
 * sees the second as a resend -- after the console adopted the restart, with
 * an ack inside the Pico's new window. Only the console emptying its frames
 * in flight keeps that resend from carrying the old datagram's middle. */
static void p_stale_resend_after_adoption(void) {
    setup_peer();
    uint8_t old[1002];
    poisoned_stream(old, 13, 116); /* the third 58-byte fragment */
    assert(console_peer_ip_write(old, sizeof old) == sizeof old);
    pump(1);
    uint8_t f[LINK_FRAME], first[LINK_FRAME], second[LINK_FRAME];
    link_next_out(f); console_peer_receive(f, first);
    link_next_out(f); console_peer_receive(f, second); /* never arrives */
    pico_restarts();
    accept_reply(first);
    hello_then_fresh_ping();
}

/* The first stale frame's ack happens to equal the new Pico's base - 1 (the
 * console's rx_next is 0, as it is once in 256). An ack inside the window
 * proves adoption only from a frame after the first. */
static void p_stale_first_frame_aliased_ack(void) {
    setup_peer();
    while (link_stats().seq != 0) {
        assert(link_push(CH_KBD, (const uint8_t *)"a", 1)); pump(3);
    }
    uint8_t old[1002];
    poisoned_stream(old, 17, 58);
    assert(console_peer_ip_write(old, sizeof old) == sizeof old);
    pump(1);
    uint8_t f[LINK_FRAME], stale[LINK_FRAME];
    link_next_out(f); console_peer_receive(f, stale);
    assert(stale[1] == 255); /* the alias this property needs */
    pico_restarts();
    accept_reply(stale);
    hello_then_fresh_ping();
}

/* The mirror. The Pico is part-way through a datagram toward the console
 * when the console restarts (a reboot, or `net reset`); the Pico's next
 * frames were cut for the old console. After them, a fresh datagram must be
 * the first thing in the console's IP ring. */
static void console_restart_then_fresh(void) {
    console_peer_init(); /* session_init: everything the console had is gone */
    received_len = 0; console_resets = 0;
    pump(12);
    uint8_t fresh[40], expected[42];
    packet_bytes(fresh, sizeof fresh, 19); stream_bytes(expected, sizeof fresh, 19);
    assert(link_push(CH_IP, fresh, sizeof fresh));
    pump(12);
    if (received_len != sizeof expected)
        fprintf(stderr, "console IP bytes after restart=%zu, expected %zu\n",
                received_len, sizeof expected);
    assert(received_len == sizeof expected && !memcmp(received, expected, sizeof expected));
    assert(pico_resets == 1); /* the Pico saw the console's SYN, once */
}
static void c_pico_mid_datagram(void) {
    setup_peer();
    uint8_t old[1000]; packet_bytes(old, sizeof old, 7);
    assert(link_push(CH_IP, old, sizeof old));
    pump(2);
    console_restart_then_fresh();
}

/* The Pico's first stale frame acks the new console's tx_base - 1 by
 * coincidence: the Pico's rx_next is 0. */
static void c_stale_first_frame_aliased_ack(void) {
    setup_peer();
    /* Control records the Pico ignores, one frame each, until the Pico has
     * taken a multiple of 256 console frames. */
    while (link_stats().ack != 255) {
        assert(console_peer_control_write((const uint8_t *)"\x7e\0", 2)); pump(3);
    }
    uint8_t old[1000]; packet_bytes(old, sizeof old, 9);
    assert(link_push(CH_IP, old, sizeof old));
    pump(2);
    assert(link_stats().ack == 255);
    console_restart_then_fresh();
}

/* The c_ properties are the console's own receive path, so an old console
 * cannot pass them and `--list-pico` leaves them out of its run. */
static const struct { const char *name; void (*run)(void); bool console_side; } recovery[] = {
    {"p_stale_resend_after_adoption", p_stale_resend_after_adoption, false},
    {"p_stale_first_frame_aliased_ack", p_stale_first_frame_aliased_ack, false},
    {"c_pico_mid_datagram", c_pico_mid_datagram, true},
    {"c_stale_first_frame_aliased_ack", c_stale_first_frame_aliased_ack, true},
};
int main(int argc, char **argv) {
    signal(SIGABRT, assertion_exit);
    if (argc < 2) return 2;
    bool pico_only = !strcmp(argv[1], "--list-pico");
    if (pico_only || !strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof recovery / sizeof *recovery; ++i)
            if (!pico_only || !recovery[i].console_side) puts(recovery[i].name);
        return 0;
    }
    char *setup[] = {argv[0], "--setup-only"};
    assert(baseline_main(2, setup) == 2);
    for (unsigned i = 0; i < sizeof recovery / sizeof *recovery; ++i)
        if (!strcmp(argv[1], recovery[i].name)) {
            recovery[i].run(); assert(!locks);
            printf("PASS %s\n", recovery[i].name); return 0;
        }
    return 2;
}
