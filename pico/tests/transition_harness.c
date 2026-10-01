/* NAT mode changes are not LINK lifetime changes. Real Pico link/NAT/ctrl,
 * SDK lwIP, relay, and an unmodified legacy/current console run in this executable. */
#define relay_send unused_baseline_relay_send
#define main baseline_main
#include "revalidation_lwip_harness.c"
#undef main
#undef relay_send
#include "../relay.h"
#include <signal.h>
void console_peer_init(void);
void console_peer_window(void);
unsigned console_peer_window_expected(void);
void console_peer_receive(const uint8_t *, uint8_t *);
int console_peer_ip_read(uint8_t *, int);
int console_peer_ip_write(const uint8_t *, int);
void console_peer_enable_ip(void);
unsigned console_peer_keys(void);
const uint8_t *console_peer_key_values(void);
void console_peer_open_channel(void);
int console_peer_channel_failed(void);
int console_peer_control_write(const uint8_t *, unsigned);
unsigned console_peer_hellos(void);
const char *console_peer_address(void);
unsigned console_peer_sequence(void);
unsigned console_peer_syn(void);
size_t transition_relay_output(uint8_t *, size_t);
void transition_relay_address(const uint8_t *);

static bool relay_mode;
static unsigned pico_resets, console_resets;
static uint8_t received[8192], reverse[8192];
static size_t received_len, reverse_len;
static const uint8_t relay_address[] = {198, 51, 100, 7};
static unsigned offset;

/* Same mode selection calls and record dispatch as main.c. There is no
 * test-side splice buffer, tail discard, SYN injection, or link restart. */
static void mode(bool relay) {
    relay_mode = relay;
    if (relay) {
        nat_configure(false);
        ctrl_set_forwarder(NULL, NULL);
        relay_configure("192.0.2.20", true);
    } else {
        relay_configure("", false);
        nat_configure(true);
        ctrl_set_forwarder(nat_forward_add, nat_forward_del);
    }
}
static void roundtrip(void) { mode(true); mode(false); }
static void collect(void) {
    uint8_t b[2048]; int n;
    while ((n = console_peer_ip_read(b, sizeof(b))) != 0) {
        if (n < 0) { ++console_resets; continue; }
        assert(received_len + (size_t)n <= sizeof(received));
        memcpy(received + received_len, b, n); received_len += n;
    }
}
static void accept_reply(const uint8_t *reply) {
    link_rx_t r = link_on_rx(reply); assert(r.valid);
    if (r.reset) { ++pico_resets; ctrl_reset(); relay_reset(); nat_reset(); }
    for (size_t i = 0; i < r.count; ++i) {
        const link_record_t *record = r.records + i;
        if (record->ch == CH_IP) {
            assert(reverse_len + record->len <= sizeof(reverse));
            memcpy(reverse + reverse_len, record->data, record->len);
            reverse_len += record->len;
            if (relay_mode) relay_ip_bytes(record->data, record->len);
            else nat_ip_bytes(record->data, record->len);
        } else if (record->ch == CH_CTRL) ctrl_bytes(record->data, record->len);
    }
}
static void exchange(void) {
    uint8_t f[LINK_FRAME], reply[LINK_FRAME];
    link_next_out(f); console_peer_receive(f, reply); collect(); accept_reply(reply);
}
static void pump(unsigned n) { while (n--) exchange(); }
static void setup_peer(void) {
    relay_init(); console_peer_init(); pump(2);
    assert(link_push(CH_KBD, (const uint8_t *)"!", 1)); pump(3);
    /* HELLO is produced by the foreground, exactly as on main. Finish both
     * cold-start SYN episodes before testing an established mode switch. */
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2)); pump(5);
    nat_configure(!relay_mode);
    if (relay_mode) transition_relay_address(relay_address);
    else ctrl_poll();
    pump(3); /* Finish the startup HELLO before measuring transition replies. */
    console_peer_window(); pump(5);
    assert(link_stats().window == console_peer_window_expected());
    console_peer_enable_ip(); console_peer_open_channel();
    received_len = reverse_len = pico_resets = console_resets = 0;
    assert(!console_peer_syn());
}
static void packet_bytes(uint8_t *p, size_t n, unsigned tag) {
    for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(i * 37 + tag);
    p[0] = 0x45; p[1] = 0; put16(p + 2, n); p[6] = p[7] = 0;
    p[8] = 64; p[9] = 1;
    put32(p + 12, 0x0a00020f); put32(p + 16, 0x0a000202);
    p[10] = p[11] = 0; put16(p + 10, checksum(p, 20, 0));
    p[20] = 8; p[21] = p[22] = p[23] = 0;
    put16(p + 22, checksum(p + 20, n - 20, 0));
}
static void stream_bytes(uint8_t *s, size_t n, unsigned tag) {
    s[0] = n; s[1] = n >> 8; packet_bytes(s + 2, n, tag);
}
static void assert_healthy(void) {
    assert(!pico_resets && !console_resets);
    assert(!console_peer_channel_failed());
}
/* The port owns a snapshot of the frame while its bytes cross the wire.
 * Switch at every IP-stream byte within that real frame, then deliver only
 * the complete frame to main's actual receiver. Subsequent frames must finish
 * the same datagram. Offset 0 is after its frame header has begun crossing. */
static void p1_pico_to_console_datagram_whole_at_every_byte(void) {
    setup_peer();
    uint8_t old[1500], fresh[40], stale[40], expected[1544];
    packet_bytes(old, sizeof(old), 7); packet_bytes(fresh, sizeof(fresh), 19);
    packet_bytes(stale, sizeof(stale), 99);
    stream_bytes(expected, sizeof(old), 7); stream_bytes(expected + 1502, sizeof(fresh), 19);
    assert(link_push(CH_IP, old, sizeof(old)));
    size_t crossed = 0; bool switched = false;
    for (unsigned iteration = 0; iteration < 80; ++iteration) {
        uint8_t f[LINK_FRAME], wire[LINK_FRAME], reply[LINK_FRAME];
        link_next_out(f); memcpy(wire, f, sizeof(wire));
        if (!switched) {
            for (size_t o = LINK_HDR; o + 2 <= LINK_FRAME && f[o] != CH_PAD;) {
                unsigned len = f[o + 1]; assert(o + 2 + len <= LINK_FRAME);
                if (f[o] == CH_IP) {
                    if (offset >= crossed && offset <= crossed + len) {
                        /* wire[0..cut) has crossed, wire[cut..64) is still
                         * owned by the port. A switch cannot replace it. */
                        size_t cut = o + 2 + offset - crossed;
                        memcpy(wire, f, cut);
                        /* Queued only AFTER the port snapshot: definitely
                         * unstarted, even when old ends within this frame. */
                        assert(link_push(CH_IP, stale, sizeof(stale)));
                        roundtrip();
                        assert(link_push(CH_IP, fresh, sizeof(fresh)));
                        memcpy(wire + cut, f + cut, LINK_FRAME - cut);
                        switched = true; break;
                    }
                    crossed += len;
                }
                o += 2 + len;
            }
        }
        console_peer_receive(wire, reply); collect(); accept_reply(reply);
    }
    assert(switched);
    if (received_len != sizeof(expected)) fprintf(stderr, "offset=%u IP bytes=%zu expected=%zu\n", offset, received_len, sizeof(expected));
    assert(received_len == sizeof(expected));
    assert(!memcmp(received, expected, sizeof(expected))); assert_healthy();
}
/* Real console frame generation -> real Pico receiver -> real selected
 * assembler. Prefix publication is deliberately split at every stream byte,
 * including length bytes; net_ip_write is a byte-stream API. */
static void reverse_prefix(const uint8_t *s, unsigned n) {
    if (n) assert(console_peer_ip_write(s, n) == (int)n);
    pump(32);
    assert(reverse_len == n && !memcmp(reverse, s, n));
}
static void p1_console_to_pico_nat_datagram_whole_at_every_byte(void) {
    setup_peer(); uint8_t s[1502]; stream_bytes(s, 1500, 31);
    reverse_prefix(s, offset);
    /* Isolate completion-after-switch from completion-before-switch. */
    if (offset == sizeof(s)) {
        assert(nat_stats().in == 1); roundtrip(); pump(4); assert_healthy(); return;
    }
    roundtrip();
    assert(console_peer_ip_write(s + offset, sizeof(s) - offset) == (int)(sizeof(s) - offset));
    pump(64); nat_poll(); pump(64);
    assert(reverse_len == sizeof(s) && !memcmp(reverse, s, sizeof(s)));
    /* ICMP echo response proves the NAT got an intact valid datagram,
     * including payload and checksums, not merely a zero assembler count. */
    assert(received_len == sizeof(s));
    assert(received[22] == 0 && checksum(received + 22, 1480, 0) == 0);
    assert(!memcmp(received + 26, s + 26, sizeof(s) - 26)); assert_healthy();
}
static void p1_console_to_relay_datagram_completed_in_new_mode(void) {
    setup_peer(); uint8_t s[1502], relay_bytes[1600]; stream_bytes(s, 1500, 43);
    reverse_prefix(s, offset); mode(true);
    assert(console_peer_ip_write(s + offset, sizeof(s) - offset) == (int)(sizeof(s) - offset));
    pump(64);
    size_t n = transition_relay_output(relay_bytes, sizeof(relay_bytes));
    assert(n == 1503 && relay_bytes[0] == 1);
    assert(!memcmp(relay_bytes + 1, s, sizeof(s))); assert_healthy();
}
static void p1_relay_to_nat_datagram_completed_in_new_mode(void) {
    /* Cold-start in relay mode, then switch once while receiving. */
    relay_init(); mode(true); link_init(); setup_peer();
    uint8_t s[1502]; stream_bytes(s, 1500, 57);
    reverse_prefix(s, offset); mode(false);
    assert(console_peer_ip_write(s + offset, sizeof(s) - offset) == (int)(sizeof(s) - offset));
    pump(64); nat_poll(); pump(64);
    assert(received_len == sizeof(s) && received[22] == 0);
    assert(checksum(received + 22, 1480, 0) == 0);
    assert(!memcmp(received + 26, s + 26, sizeof(s) - 26)); assert_healthy();
}
static void p2_started_control_record_finishes_before_new_hello(void) {
    setup_peer();
    uint8_t record[103] = {0x84, 101, 2}; memset(record + 3, 'x', 100);
    assert(link_push(CH_CTRL, record, sizeof(record)));
    exchange(); unsigned before = console_peer_hellos();
    roundtrip();
    /* New HELLO has priority, but must follow the started old control record. */
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    pump(12);
    assert(console_peer_hellos() == before + 2);
    assert(!strcmp(console_peer_address(), "192.0.2.10")); assert_healthy();
}
static void p2_inbound_control_record_finishes_in_current_mode(void) {
    setup_peer();
    const uint8_t forward[] = {0x0b, 3, 2, 0x90, 0x1f};
    assert(console_peer_control_write(forward, offset)); pump(3);
    roundtrip();
    assert(console_peer_control_write(forward + offset, sizeof(forward) - offset)); pump(8);
    assert(nat_stats().forwards == 1 && listeners[0]); assert_healthy();
}
static void keys_exactly_once(bool delivered, bool full) {
    setup_peer(); unsigned before = console_peer_keys();
    uint8_t f[LINK_FRAME], reply[LINK_FRAME], expected[132];
    unsigned first = full ? 1 : 100, total = first;
    for (unsigned i = 0; i < first; ++i) expected[i] = (uint8_t)(i + 1);
    assert(link_push(CH_KBD, expected, first)); link_next_out(f);
    if (delivered) { console_peer_receive(f, reply); collect(); } /* ACK delayed */
    if (full) {
        for (unsigned i = 0; i < 32; ++i) {
            expected[total++] = (uint8_t)(64 + i);
            assert(link_push(CH_KBD, expected + total - 1, 1));
        }
        assert(!link_push(CH_KBD, (const uint8_t *)"overflow", 8));
    }
    roundtrip();
    /* Frame loss or ACK loss must not discard, duplicate, or reorder keys. */
    link_next_out(f); console_peer_receive(f, reply); collect(); /* lose ACK again */
    pump(12);
    assert(console_peer_keys() == before + total);
    assert(!memcmp(console_peer_key_values() + before, expected, total));
}
static void p3_keyboard_inflight_and_suffix_exactly_once(void) { keys_exactly_once(false, false); }
static void p3_keyboard_lost_ack_exactly_once(void) { keys_exactly_once(true, false); }
static void p3_keyboard_full_queue_exactly_once(void) { keys_exactly_once(false, true); }
static void p3_keyboard_full_queue_delayed_ack_exactly_once(void) { keys_exactly_once(true, true); }
static void p4_switch_preserves_sequence_syn_and_live_channels(void) {
    setup_peer();
    /* Exercise all 256 sequence values by continuing the SAME link. */
    for (unsigned i = 0; i < offset; ++i) {
        assert(link_push(CH_KBD, (const uint8_t *)"advance", 7)); pump(3);
    }
    {
        uint8_t f[LINK_FRAME], after[LINK_FRAME];
        assert(link_push(CH_KBD, (const uint8_t *)"k", 1)); link_next_out(f);
        link_stats_t before = link_stats();
        unsigned console_seq = console_peer_sequence(), console_syn = console_peer_syn();
        roundtrip(); link_stats_t now = link_stats(); link_next_out(after);
        assert(now.seq == before.seq && now.ack == before.ack);
        assert(!memcmp(f, after, sizeof(f)));
        assert(console_peer_sequence() == console_seq && console_peer_syn() == console_syn);
        pump(3); assert_healthy();
    }
}
static void p4_cold_start_syn_state_survives_switch(void) {
    relay_init();
    uint8_t before[LINK_FRAME], after[LINK_FRAME]; link_next_out(before);
    roundtrip(); link_next_out(after);
    assert(!memcmp(before, after, sizeof(before)));
}
static void p5_old_flows_forwards_and_listeners_are_gone(void) {
    establish(); console_udp(0xc0000235); assert(nat_forward_add(8080, 1));
    relay_init(); mode(true);
    nat_stats_t s = nat_stats(); assert(!s.tcp && !s.udp && !s.forwards);
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) assert(!tcp_pcbs[i] && !listeners[i]);
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i) assert(!udp_slots[i]);
    struct tcp_pcb *probe = tcp_new(); assert(probe);
    assert(tcp_bind(probe, IP_ANY_TYPE, 8080) == ERR_OK); tcp_abort(probe);
    mode(false); assert(nat_forward_add(8080, 1));
}
static void p5_stray_old_flow_gets_reset(void) {
    establish(); uint32_t seq = get32(packet + 24) + 1;
    relay_init(); roundtrip(); console_tcp(16, 101, seq, NULL); nat_poll();
    assert(drain_ip() == 1 && packet[33] == 20);
    assert(get32(packet + 12) == 0xc0000201 && get32(packet + 24) == seq);
    assert(!nat_stats().tcp);
}
static void p5_next_hello_has_current_mode_address(void) {
    setup_peer();
    mode(true); transition_relay_address(relay_address);
    unsigned before = console_peer_hellos();
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2)); pump(8);
    assert(console_peer_hellos() == before + 1);
    assert(!strcmp(console_peer_address(), "198.51.100.7"));
    mode(false); before = console_peer_hellos();
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2)); pump(8);
    assert(console_peer_hellos() == before + 1);
    assert(!strcmp(console_peer_address(), "192.0.2.10")); assert_healthy();
}
static void p5_switch_resets_dns_cursor(void) {
    relay_init(); console_udp(0x0a000203); console_udp(0x0a000203);
    assert(udp_destinations[0] != udp_destinations[1]);
    roundtrip(); console_udp(0x0a000203);
    assert(udp_destinations[2] == udp_destinations[0]);
}
static void p6_rapid_switches_preserve_bidirectional_traffic(void) {
    setup_peer(); unsigned keys = console_peer_keys(), hellos = console_peer_hellos();
    uint8_t s[102]; stream_bytes(s, 100, 71);
    assert(link_push(CH_IP, s + 2, 100));
    assert(console_peer_ip_write(s, 41) == 41); pump(1);
    uint8_t f[LINK_FRAME];
    assert(link_push(CH_KBD, (const uint8_t *)"abc", 3)); link_next_out(f);
    link_stats_t before = link_stats();
    /* 64 consecutive switches with no opportunity to acknowledge a frame. */
    for (unsigned i = 0; i < 64; ++i) mode(!(i & 1));
    link_stats_t after = link_stats();
    assert(after.seq == before.seq && after.ack == before.ack);
    assert(console_peer_ip_write(s + 41, sizeof(s) - 41) == (int)(sizeof(s) - 41));
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    pump(12); nat_poll(); pump(12);
    assert(reverse_len == sizeof(s) && !memcmp(reverse, s, sizeof(s)));
    assert(received_len == 2 * sizeof(s));
    assert(!memcmp(received, s, sizeof(s)) && received[sizeof(s) + 22] == 0);
    assert(console_peer_keys() == keys + 3);
    assert(!memcmp(console_peer_key_values() + keys, "abc", 3));
    assert(console_peer_hellos() == hellos + 1); assert_healthy();
    /* Then repeat with traffic and lifecycle state between each pair. */
    for (unsigned i = 0; i < 32; ++i) {
        assert(nat_forward_add(8080, 1));
        assert(link_push(CH_KBD, (const uint8_t *)"z", 1));
        roundtrip(); pump(4);
        assert(!nat_stats().forwards && !listeners[0]); assert_healthy();
    }
    assert(console_peer_keys() == keys + 35);
}
static void p0_real_peer_without_switch_delivers_all_streams(void) {
    setup_peer(); uint8_t s[1502]; stream_bytes(s, 1500, 87);
    assert(link_push(CH_IP, s + 2, 1500));
    assert(console_peer_ip_write(s, sizeof(s)) == sizeof(s));
    unsigned keys = console_peer_keys(), hellos = console_peer_hellos();
    assert(link_push(CH_KBD, (const uint8_t *)"abc", 3));
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
    pump(64); nat_poll(); pump(64);
    assert(received_len == 2 * sizeof(s));
    assert(!memcmp(received, s, sizeof(s)));
    assert(received[sizeof(s) + 22] == 0);
    assert(!memcmp(received + sizeof(s) + 26, s + 26, sizeof(s) - 26));
    assert(reverse_len == sizeof(s) && !memcmp(reverse, s, sizeof(s)));
    assert(console_peer_keys() == keys + 3 && console_peer_hellos() == hellos + 1);
    assert_healthy();
}
static void p0_relay_without_switch_delivers_datagram_and_hello(void) {
    relay_init(); mode(true); link_init(); setup_peer();
    uint8_t s[1502], transport[1503]; stream_bytes(s, 1500, 89);
    assert(console_peer_ip_write(s, sizeof(s)) == sizeof(s)); pump(64);
    assert(transition_relay_output(transport, sizeof(transport)) == sizeof(transport));
    assert(transport[0] == 1 && !memcmp(transport + 1, s, sizeof(s)));
    transition_relay_address(relay_address);
    unsigned before = console_peer_hellos();
    assert(console_peer_control_write((const uint8_t *)"\x04\0", 2)); pump(8);
    assert(console_peer_hellos() == before + 1);
    assert(!strcmp(console_peer_address(), "198.51.100.7")); assert_healthy();
}
static void p2_control_whole_at_every_wire_byte(void) {
    setup_peer();
    uint8_t record[103] = {0x84, 101, 2}; memset(record + 3, 'x', 100);
    unsigned before = console_peer_hellos();
    assert(link_push(CH_CTRL, record, sizeof(record)));
    unsigned crossed = 0; bool switched = false;
    for (unsigned i = 0; i < 16; ++i) {
        uint8_t f[LINK_FRAME], reply[LINK_FRAME]; link_next_out(f);
        if (!switched) {
            for (size_t o = LINK_HDR; o + 2 <= LINK_FRAME && f[o] != CH_PAD;) {
                unsigned n = f[o + 1]; assert(o + 2 + n <= LINK_FRAME);
                if (f[o] == CH_CTRL) {
                    if (offset >= crossed && offset <= crossed + n) {
                        /* Mode changes while this immutable port frame is
                         * physically crossing; receiver consumes it whole. */
                        roundtrip();
                        assert(console_peer_control_write((const uint8_t *)"\x04\0", 2));
                        switched = true; break;
                    }
                    crossed += n;
                }
                o += n + 2;
            }
        }
        console_peer_receive(f, reply); collect(); accept_reply(reply);
    }
    assert(switched && console_peer_hellos() == before + 2);
    assert(!strcmp(console_peer_address(), "192.0.2.10")); assert_healthy();
}
static void p1_console_inflight_frame_finishes_across_switch(void) {
    setup_peer(); uint8_t s[102], f[LINK_FRAME], reply[LINK_FRAME];
    stream_bytes(s, 100, 91);
    assert(console_peer_ip_write(s, sizeof(s)) == sizeof(s));
    link_next_out(f); console_peer_receive(f, reply); collect();
    /* The console frame is on the cable, not yet admitted by Pico. */
    roundtrip(); accept_reply(reply); pump(16); nat_poll(); pump(16);
    assert(reverse_len == sizeof(s) && !memcmp(reverse, s, sizeof(s)));
    assert(received_len == sizeof(s) && received[22] == 0);
    assert(!memcmp(received + 26, s + 26, sizeof(s) - 26)); assert_healthy();
}
static const struct { const char *name; void (*run)(void); } properties[] = {
    TEST(p0_real_peer_without_switch_delivers_all_streams),
    TEST(p0_relay_without_switch_delivers_datagram_and_hello),
    TEST(p1_console_inflight_frame_finishes_across_switch),
    TEST(p2_control_whole_at_every_wire_byte),
    TEST(p1_pico_to_console_datagram_whole_at_every_byte),
    TEST(p1_console_to_pico_nat_datagram_whole_at_every_byte),
    TEST(p1_console_to_relay_datagram_completed_in_new_mode),
    TEST(p1_relay_to_nat_datagram_completed_in_new_mode),
    TEST(p2_started_control_record_finishes_before_new_hello),
    TEST(p2_inbound_control_record_finishes_in_current_mode),
    TEST(p3_keyboard_inflight_and_suffix_exactly_once),
    TEST(p3_keyboard_lost_ack_exactly_once),
    TEST(p3_keyboard_full_queue_exactly_once),
    TEST(p3_keyboard_full_queue_delayed_ack_exactly_once),
    TEST(p4_switch_preserves_sequence_syn_and_live_channels),
    TEST(p4_cold_start_syn_state_survives_switch),
    TEST(p5_old_flows_forwards_and_listeners_are_gone),
    TEST(p5_stray_old_flow_gets_reset),
    TEST(p5_next_hello_has_current_mode_address),
    TEST(p5_switch_resets_dns_cursor),
    TEST(p6_rapid_switches_preserve_bidirectional_traffic),
};
/* Avoid invoking the host crash reporter thousands of times. Assertions
 * still print their diagnostic and produce a failing exit status. */
static void assertion_exit(int signal_number) { (void)signal_number; _Exit(1); }
#ifndef TRANSITION_NO_MAIN
int main(int argc, char **argv) {
    signal(SIGABRT, assertion_exit);
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(properties)/sizeof(*properties); ++i) puts(properties[i].name);
        return 0;
    }
    offset = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 0;
    char *setup[] = {argv[0], "--setup-only"}; assert(baseline_main(2, setup) == 2);
    for (unsigned i = 0; i < sizeof(properties)/sizeof(*properties); ++i)
        if (!strcmp(argv[1], properties[i].name)) {
            properties[i].run(); assert(!locks); printf("PASS %s offset=%u\n", properties[i].name, offset); return 0;
        }
    return 2;
}

#endif
