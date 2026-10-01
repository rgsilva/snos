/* Actual Pico sender/receiver and actual console net.c; no NAT substitutes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../link.h"
void console_peer_init(void);
void console_peer_receive(const uint8_t *, uint8_t *);
void console_peer_enable_ip(void);
int console_peer_ip_write(const uint8_t *, int);
static void sum(uint8_t *f) {
    f[3] = 0;
    for (unsigned i = 0; i < LINK_FRAME; ++i) if (i != 3) f[3] += f[i];
}
static link_rx_t exchange(uint8_t *f, uint8_t *reply) {
    link_next_out(f); console_peer_receive(f, reply);
    return link_on_rx(reply);
}
static void setup(void) {
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    link_init(); console_peer_init();
    for (unsigned i = 0; i < 4; ++i) assert(exchange(f, reply).valid);
    assert(link_push(CH_KBD, (const uint8_t *)"!", 1));
    for (unsigned i = 0; i < 4; ++i) assert(exchange(f, reply).valid);
    /* Explicitly finish the console's SYN even on the pre-fix parent, which
     * did not automatically enqueue HELLO. Isolate the new ACK predicate. */
    uint8_t packet[42] = {40, 0, 0x45};
    console_peer_enable_ip();
    assert(console_peer_ip_write(packet, sizeof(packet)) == sizeof(packet));
    for (unsigned i = 0; i < 4; ++i) assert(exchange(f, reply).valid);
}
static void progress(void) {
    uint8_t packet[42] = {40, 0, 0x45}, f[LINK_FRAME], reply[LINK_FRAME];
    console_peer_enable_ip();
    assert(console_peer_ip_write(packet, sizeof(packet)) == sizeof(packet));
    unsigned delivered = 0;
    for (unsigned i = 0; i < 12; ++i) {
        link_rx_t r = exchange(f, reply); assert(r.valid && !r.reset);
        for (size_t j = 0; j < r.count; ++j)
            if (r.records[j].ch == CH_IP) delivered += r.records[j].len;
    }
    fprintf(stderr, "fresh IP delivered: %u/42\n", delivered);
    assert(delivered == sizeof(packet));
}
static void bare_restart(void) {
    setup(); link_restart();
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    link_rx_t r = exchange(f, reply);
    assert((f[2] & (F_SYN | F_DATA)) == F_SYN);
    assert(reply[1] == (uint8_t)(f[0] - 1));
    assert(r.valid && !r.reset && r.count == 1);
    progress();
}
static void restart_data(unsigned loss) {
    setup(); link_restart();
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    if (loss) {
        link_next_out(f); assert((f[2] & (F_SYN | F_DATA)) == F_SYN);
        /* loss 1: console misses first SYN. loss 2: Pico misses its reply. */
        if (loss == 2) console_peer_receive(f, reply);
    }
    assert(link_push(CH_KBD, (const uint8_t *)"k", 1));
    link_next_out(f); assert((f[2] & (F_SYN | F_DATA)) == (F_SYN | F_DATA));
    console_peer_receive(f, reply);
    fprintf(stderr, "restart seq=%u, console ACK=%u, gate expects=%u\n",
            f[0], reply[1], (uint8_t)(f[0] - 1));
    assert(reply[2] & F_SYN);
    assert(reply[1] == f[0]); /* Real receiver consumes the restart's data. */
    link_rx_t r = link_on_rx(reply); assert(r.valid && !r.reset);
    progress(); /* Do not accept forward keyboard delivery as reverse progress. */
}
static void queued_keyboard(void) { restart_data(0); }
static void missed_first_syn(void) { restart_data(1); }
static void missed_first_reply(void) { restart_data(2); }
static void two_restarts(unsigned miss) {
    setup();
    uint8_t f[LINK_FRAME], first[LINK_FRAME], second[LINK_FRAME];
    link_restart(); link_next_out(f);
    if (!miss) console_peer_receive(f, first);
    link_restart(); link_next_out(f);
    if (!miss) {
        link_rx_t r = link_on_rx(first);
        assert(r.valid && !r.reset && !r.count);
    }
    console_peer_receive(f, second);
    assert(second[1] == (uint8_t)(f[0] - 1));
    link_rx_t r = link_on_rx(second); assert(r.valid && !r.reset && r.count);
    progress();
}
static void two_quick(void) { two_restarts(0); }
static void two_quick_missed(void) { two_restarts(1); }
static void all_sequence_bases(void) {
    /* Superset of immediately old ACKs: t-1 and t, both bearing SYN+DATA.
     * Test every Pico base, including the extra syn_seq avoidance increment. */
    for (unsigned base = 0; base < 256; ++base) {
        uint8_t f[LINK_FRAME], reply[LINK_FRAME], old[LINK_FRAME];
        link_init(); console_peer_init();
        for (unsigned i = 0; i < base; ++i) {
            assert(link_push(CH_KBD, (const uint8_t *)"x", 1));
            link_next_out(f); memset(reply, CH_PAD, sizeof(reply));
            reply[0] = 0; reply[1] = f[0]; reply[2] = 0; sum(reply);
            assert(link_on_rx(reply).valid);
        }
        link_restart(); link_next_out(f);
        for (unsigned age = 0; age < 2; ++age) {
            memset(old, CH_PAD, sizeof(old));
            old[0] = 7; old[1] = (uint8_t)(base - age); old[2] = F_SYN | F_DATA;
            old[4] = CH_CTRL; old[5] = 2; old[6] = 4; old[7] = 0; sum(old);
            link_rx_t r = link_on_rx(old); assert(r.valid && !r.reset && !r.count);
        }
        console_peer_receive(f, reply);
        assert(reply[1] == (uint8_t)(f[0] - 1));
        link_rx_t r = link_on_rx(reply); assert(r.valid && !r.reset && r.count == 1);
    }
}
static void unobserved_restart_wrap(void) {
    uint8_t f[LINK_FRAME], old[LINK_FRAME];
    link_init(); console_peer_init();
    /* Actual console emits SYN+HELLO with ACK 0; keep its outstanding frame.
     * It has never seen any subsequent lifetime, and is still retrying it. */
    assert(link_push(CH_KBD, (const uint8_t *)"x", 1));
    link_next_out(f); console_peer_receive(f, old);
    assert(old[1] == 0 && (old[2] & F_SYN));
    assert(link_on_rx(old).valid); assert(link_stats().seq == 1);
    for (unsigned i = 0; i < 128; ++i) link_restart();
    link_next_out(f);
    fprintf(stderr, "128 unobserved restarts: seq=%u, old ACK=%u\n", f[0], old[1]);
    assert(f[0] == 1);
    link_rx_t r = link_on_rx(old);
    fprintf(stderr, "old lifetime records admitted: %zu\n", r.count);
    assert(r.valid && !r.reset && r.count == 0);
}
#define TEST(n) {#n, n}
static const struct { const char *name; void (*run)(void); } cases[] = {
    TEST(bare_restart), TEST(queued_keyboard), TEST(missed_first_syn),
    TEST(missed_first_reply), TEST(two_quick), TEST(two_quick_missed),
    TEST(all_sequence_bases), TEST(unobserved_restart_wrap),
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) {
        if (!strcmp(argv[1], "--list")) puts(cases[i].name);
        else if (!strcmp(argv[1], cases[i].name)) {
            cases[i].run(); printf("PASS %s\n", cases[i].name); return 0;
        }
    }
    return strcmp(argv[1], "--list") ? 2 : 0;
}
