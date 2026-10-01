/* Behavioural peer for nat_core.c. Each scenario runs in a fresh process. */
#include "../nat_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNES 0x0a00020fu
#define GW 0x0a000202u
#define DNS 0x0a000203u
#define REMOTE 0x7f000001u
#define FIN 1
#define SYN 2
#define RST 4
#define PSH 8
#define ACK 16

typedef union { max_align_t align; unsigned char bytes[40000]; } store_t;
static store_t store;
static nat_t *n = (nat_t *)&store;
static unsigned emitted, connects, writes, forwards, removed, recved, shutdowns, aborts;
static unsigned char last[NAT_MTU], written[8192], udp_data[NAT_MTU];
static size_t last_len, written_len, udp_len;
static uint32_t connect_addr, udp_addr;
static uint16_t connect_port, udp_port;
static bool write_ok = true;
static uint32_t tick = 10;

static uint16_t rd16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static uint32_t rd32(const unsigned char *p) { return ((uint32_t)rd16(p) << 16) | rd16(p + 2); }
static void wr16(unsigned char *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void wr32(unsigned char *p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, v); }
static uint16_t fold(const unsigned char *p, size_t z, uint32_t s) {
    while (z > 1) { s += rd16(p); p += 2; z -= 2; }
    if (z) s += (unsigned)*p << 8;
    while (s >> 16) s = (s & 65535) + (s >> 16);
    return (uint16_t)~s;
}
static uint32_t pseudo(uint32_t src, uint32_t dst, unsigned proto, size_t len) {
    return (src >> 16) + (src & 65535) + (dst >> 16) + (dst & 65535) + proto + len;
}
static void emit(void *arg, const unsigned char *p, size_t len) {
    (void)arg;
    assert(len <= NAT_MTU && len >= 20 && rd16(p + 2) == len);
    assert(rd32(p + 16) == SNES && fold(p, 20, 0) == 0);
    unsigned proto = p[9];
    assert(fold(p + 20, len - 20, proto == 1 ? 0 :
                pseudo(rd32(p + 12), SNES, proto, len - 20)) == 0);
    ++emitted; last_len = len; memcpy(last, p, len);
}
static bool connect_(void *arg, unsigned i, uint32_t addr, uint16_t port) {
    (void)arg; (void)i; ++connects; connect_addr = addr; connect_port = port; return true;
}
static bool write_(void *arg, unsigned i, const unsigned char *p, size_t len) {
    (void)arg; (void)i; ++writes;
    if (!write_ok) return false;
    assert(written_len + len <= sizeof(written));
    memcpy(written + written_len, p, len); written_len += len; return true;
}
static void shutdown_(void *arg, unsigned i) { (void)arg; (void)i; ++shutdowns; }
static void abort_(void *arg, unsigned i) { (void)arg; (void)i; ++aborts; }
static bool udp(void *arg, unsigned i, uint32_t addr, uint16_t port, const unsigned char *p, size_t len) {
    (void)arg; (void)i; udp_addr = addr; udp_port = port; udp_len = len;
    memcpy(udp_data, p, len); return true;
}
static bool fadd(void *arg, uint16_t port) { (void)arg; (void)port; ++forwards; return true; }
static void fdel(void *arg, uint16_t port) { (void)arg; (void)port; ++removed; }
static void got(void *arg, unsigned i, size_t len) { (void)arg; (void)i; recved += len; }
static void send_ip(unsigned proto, uint32_t dst, const unsigned char *body, size_t len) {
    unsigned char p[NAT_MTU] = {0};
    assert(len + 20 <= sizeof(p));
    p[0] = 0x45; wr16(p + 2, 20 + len); p[8] = 64; p[9] = proto;
    wr32(p + 12, SNES); wr32(p + 16, dst); wr16(p + 10, fold(p, 20, 0));
    memcpy(p + 20, body, len); nat_core_input(n, p, len + 20, tick);
}
static void send_tcp_to(uint32_t dst, uint16_t sp, uint16_t dp, unsigned flags,
                        uint32_t seq, uint32_t ack, uint16_t window, const char *data) {
    unsigned char b[NAT_MTU] = {0}; size_t len = data ? strlen(data) : 0;
    wr16(b, sp); wr16(b + 2, dp); wr32(b + 4, seq); wr32(b + 8, ack);
    b[12] = 0x50; b[13] = flags; wr16(b + 14, window);
    if (len) memcpy(b + 20, data, len);
    wr16(b + 16, fold(b, len + 20, pseudo(SNES, dst, 6, len + 20)));
    send_ip(6, dst, b, len + 20);
}
static void send_tcp(unsigned flags, uint32_t seq, uint32_t ack, uint16_t window, const char *data) {
    send_tcp_to(REMOTE, 5000, 80, flags, seq, ack, window, data);
}
static uint32_t establish(uint32_t seq) {
    send_tcp(SYN, seq, 0, 1460, NULL);
    assert(connects && connect_addr == REMOTE && connect_port == 80);
    nat_core_tcp_connected(n, 0, 0, tick);
    assert(last[33] == (SYN | ACK) && rd32(last + 28) == seq + 1);
    uint32_t their = rd32(last + 24) + 1;
    send_tcp(ACK, seq + 1, their, 1460, NULL);
    return their;
}
static void send_udp(uint32_t dst, const char *data) {
    unsigned char b[NAT_MTU] = {0}; size_t len = strlen(data);
    wr16(b, 4444); wr16(b + 2, 5353); wr16(b + 4, len + 8);
    memcpy(b + 8, data, len);
    wr16(b + 6, fold(b, len + 8, pseudo(SNES, dst, 17, len + 8)));
    send_ip(17, dst, b, len + 8);
}

static void t_icmp(void) {
    unsigned char b[] = {8, 0, 0, 0, 0x53, 0x41, 0, 3, 'o', 'd', 'd'};
    wr16(b + 2, fold(b, sizeof(b), 0));
    unsigned before = emitted;
    send_ip(1, GW, b, sizeof(b));
    assert(emitted == before + 1 && last[20] == 0 && rd32(last + 12) == GW);
    assert(last_len == 20 + sizeof(b) && !memcmp(last + 24, b + 4, sizeof(b) - 4));
}
static void t_udp(void) {
    send_udp(REMOTE, "hello over udp");
    assert(udp_addr == REMOTE && udp_port == 5353 && udp_len == 14);
    assert(!memcmp(udp_data, "hello over udp", 14));
    nat_core_udp_received(n, 0, (const unsigned char *)"pong:hello over udp", 19, tick);
    assert(emitted == 1 && rd32(last + 12) == REMOTE);
    assert(rd16(last + 20) == 5353 && rd16(last + 22) == 4444);
    assert(last_len == 47 && !memcmp(last + 28, "pong:hello over udp", 19));
}
static void t_dns_front(void) {
    nat_core_set_resolver(n, REMOTE);
    send_udp(DNS, "query");
    assert(udp_addr == REMOTE && udp_len == 5 && !memcmp(udp_data, "query", 5));
    nat_core_udp_received(n, 0, (const unsigned char *)"answer", 6, tick);
    assert(emitted == 1 && rd32(last + 12) == DNS && rd16(last + 22) == 4444);
    assert(last_len == 34 && !memcmp(last + 28, "answer", 6));
}
static void t_tcp(void) {
    uint32_t their = establish(0x1111);
    send_tcp(PSH | ACK, 0x1112, their, 1460, "hey");
    assert(written_len == 3 && !memcmp(written, "hey", 3));
    assert(rd32(last + 28) == 0x1115);
    assert(nat_core_tcp_received(n, 0, (const unsigned char *)"HEY", 3, tick));
    nat_core_poll(n, ++tick);
    assert(last_len == 43 && !memcmp(last + 40, "HEY", 3) && recved == 0);
    send_tcp(ACK, 0x1115, their + 3, 1460, NULL);
    assert(recved == 3);
    send_tcp(FIN | ACK, 0x1115, their + 3, 1460, NULL);
    assert(rd32(last + 28) == 0x1116 && shutdowns == 1);
    nat_core_tcp_eof(n, 0, tick); nat_core_poll(n, ++tick);
    assert(last[33] == (FIN | ACK) && rd32(last + 24) == their + 3);
}
static void t_tcp_data_fin(void) {
    uint32_t their = establish(0x4444);
    send_tcp(PSH | FIN | ACK, 0x4445, their, 1460, "bye");
    assert(rd32(last + 28) == 0x4449 && shutdowns == 1);
    assert(written_len == 3 && !memcmp(written, "bye", 3));
    assert(nat_core_tcp_received(n, 0, (const unsigned char *)"BYE", 3, tick));
    nat_core_poll(n, ++tick);
    assert(last_len == 43 && !memcmp(last + 40, "BYE", 3));
}
static void t_stray(void) {
    send_tcp(ACK, 0x9000, 0x8000, 1460, NULL);
    assert(emitted == 1 && last[33] == (RST | ACK) && rd32(last + 24) == 0x8000);
    unsigned before = emitted;
    send_tcp(RST, 0x9000, 0, 1460, NULL);
    assert(emitted == before);
}
static void t_refused(void) {
    send_tcp(SYN, 0x2222, 0, 1460, NULL);
    nat_core_tcp_connected(n, 0, 1, tick);
    assert(emitted == 1 && (last[33] & RST));
    assert(rd32(last + 28) == 0x2223 && aborts == 1);
}
static void t_forward(void) {
    assert(nat_core_forward_add(n, 8080, 1));
    assert(nat_core_inbound_accept(n, 8080, tick) == 0);
    assert(last[33] == SYN && rd16(last + 22) == 8080 && rd32(last + 12) == GW);
    uint16_t port = rd16(last + 20); uint32_t their = rd32(last + 24) + 1;
    send_tcp_to(GW, 8080, port, SYN | ACK, 0x3333, their, 1460, NULL);
    assert(last[33] == ACK && rd32(last + 28) == 0x3334);
    send_tcp_to(GW, 8080, port, PSH | ACK, 0x3334, their, 1460, "response");
    assert(written_len == 8 && !memcmp(written, "response", 8));
    nat_core_forward_del(n, 8080, 1);
    assert(forwards == 1 && removed == 1);
}
static void t_forward_cap(void) {
    assert(nat_core_forward_add(n, 8080, 1));
    for (unsigned i = 0; i < 4; ++i) assert(nat_core_inbound_accept(n, 8080, tick) >= 0);
    assert(nat_core_inbound_accept(n, 8080, tick) == -1);
    nat_core_poll(n, tick); assert(nat_core_stats(n).tcp == 4 && emitted == 4);
    nat_core_tcp_error(n, 0);
    assert(nat_core_inbound_accept(n, 8080, tick) >= 0);
}
static void t_syn_reuse(void) {
    establish(0x2222);
    send_tcp(SYN, 0x9999, 0, 1460, NULL);
    assert(connects == 2 && aborts == 1);
    nat_core_tcp_connected(n, 0, 0, tick);
    assert(last[33] == (SYN | ACK) && rd32(last + 28) == 0x999a);
}
static void t_udp_oversize(void) {
    unsigned char b[3000]; memset(b, 'x', sizeof(b));
    send_udp(REMOTE, "ask");
    unsigned before = nat_core_stats(n).drop;
    nat_core_udp_received(n, 0, b, sizeof(b), tick);
    assert(emitted == 0 && nat_core_stats(n).drop == before + 1);
    nat_core_udp_received(n, 0, b, NAT_MTU - 28, tick);
    assert(emitted == 1 && last_len == NAT_MTU);
}
static void t_backpressure(void) {
    uint32_t their = establish(0x4444);
    write_ok = false;
    send_tcp(PSH | ACK, 0x4445, their, 1460, "stuck");
    unsigned before = emitted;
    send_tcp(PSH | ACK, 0x444a, their, 1460, "more");
    assert(emitted == before && written_len == 0);
    write_ok = true; nat_core_poll(n, ++tick);
    assert(written_len == 5 && !memcmp(written, "stuck", 5));
    assert(rd32(last + 28) == 0x444a);
    send_tcp(PSH | ACK, 0x444a, their, 1460, "more");
    assert(written_len == 9 && !memcmp(written, "stuckmore", 9));
}
static void t_garbage(void) {
    unsigned char p[40] = {0};
    nat_core_input(n, p, 0, tick); nat_core_input(n, p, sizeof(p), tick);
    p[0] = 0x45; p[3] = 200; nat_core_input(n, p, 20, tick);
    unsigned before = nat_core_stats(n).drop;
    send_ip(99, GW, (const unsigned char *)"unknown", 7);
    assert(nat_core_stats(n).drop == before + 1 && emitted == 0);
    t_icmp();
}
static void t_window_and_receive_credit(void) {
    uint32_t their = establish(100);
    send_tcp(ACK, 101, their, 0, NULL);
    assert(nat_core_tcp_received(n, 0, (const unsigned char *)"abcdef", 6, tick));
    unsigned before = emitted;
    nat_core_poll(n, ++tick); assert(emitted == before && recved == 0);
    assert(!nat_core_tcp_received(n, 0, (const unsigned char *)"more", 4, tick));
    send_tcp(ACK, 101, their, 5, NULL);
    nat_core_poll(n, ++tick); assert(emitted == before);
    send_tcp(ACK, 101, their, 6, NULL); nat_core_poll(n, ++tick);
    assert(last_len == 46 && !memcmp(last + 40, "abcdef", 6));
    send_tcp(ACK, 101, their + 2, 0, NULL); assert(recved == 2);
    tick += 2501; nat_core_poll(n, tick);
    assert(last_len == 44 && rd32(last + 24) == their + 2 && !memcmp(last + 40, "cdef", 4));
    send_tcp(ACK, 101, their + 2, 0, NULL); assert(recved == 2);
    send_tcp(ACK, 101, their + 99, 0, NULL); assert(recved == 2);
    send_tcp(ACK, 101, their + 6, 1460, NULL); assert(recved == 6);
}
static void t_sequence_wrap(void) {
    uint32_t their = establish(0xfffffffeu);
    send_tcp(PSH | ACK | FIN, 0xffffffffu, their, 1460, "abc");
    assert(rd32(last + 28) == 3 && shutdowns == 1 && written_len == 3);
    send_tcp(PSH | ACK | FIN, 0xffffffffu, their, 1460, "abc");
    assert(rd32(last + 28) == 3 && shutdowns == 1 && written_len == 3);
}
static void t_fin_grace_and_time_wait(void) {
    int slot = nat_core_inbound_accept(n, 8080, tick); assert(slot == 0);
    uint16_t port = rd16(last + 20); uint32_t their = rd32(last + 24) + 1;
    send_tcp_to(GW, 8080, port, SYN | ACK, 100, their, 1460, NULL);
    nat_core_tcp_eof(n, 0, tick); unsigned before = emitted;
    nat_core_poll(n, tick + 749); assert(emitted == before);
    tick += 751; nat_core_poll(n, tick); assert(last[33] == (FIN | ACK));
    send_tcp_to(GW, 8080, port, ACK, 101, their + 1, 1460, NULL);
    nat_core_poll(n, tick + 4000); assert(nat_core_stats(n).tcp == 1);
    nat_core_poll(n, tick + 4001); assert(nat_core_stats(n).tcp == 0);
}
static void t_retry_limits(void) {
    send_tcp(SYN, 100, 0, 1460, NULL); nat_core_tcp_connected(n, 0, 0, tick);
    unsigned before = emitted;
    nat_core_poll(n, tick + 2500); assert(emitted == before);
    for (unsigned i = 0; i < 8; ++i) {
        tick += 2501; nat_core_poll(n, tick); assert(last[33] == (SYN | ACK));
    }
    tick += 2501; nat_core_poll(n, tick);
    assert(last[33] & RST); assert(nat_core_stats(n).tcp == 0);
}
static void t_fin_retry_limits(void) {
    establish(100); nat_core_tcp_eof(n, 0, tick); nat_core_poll(n, tick);
    for (unsigned i = 0; i < 8; ++i) {
        tick += 2501; nat_core_poll(n, tick);
        assert(last[33] == (FIN | ACK) && aborts == 0);
    }
    tick += 2501; nat_core_poll(n, tick);
    assert(aborts == 1 && nat_core_stats(n).tcp == 0);
}
static void t_forward_owner_lifetime(void) {
    assert(nat_core_forward_add(n, 8080, 1));
    assert(nat_core_forward_add(n, 8080, 2)); assert(forwards == 1);
    nat_core_forward_del(n, 8080, 7); assert(removed == 0);
    nat_core_forward_del(n, 8080, 1); assert(removed == 0);
    nat_core_forward_del(n, 8080, 2); assert(removed == 1);
}
static void t_data_retry_limits(void) {
    uint32_t their = establish(100);
    assert(nat_core_tcp_received(n, 0, (const unsigned char *)"abc", 3, tick));
    nat_core_poll(n, tick);
    for (unsigned i = 0; i < 120; ++i) {
        tick += 2501;
        /* Keep the independent idle timeout from masking the retry limit. */
        send_tcp(ACK, 101, their, 0, NULL);
        nat_core_poll(n, tick);
        assert(last_len == 43 && !memcmp(last + 40, "abc", 3));
    }
    tick += 2501; send_tcp(ACK, 101, their, 0, NULL); nat_core_poll(n, tick);
    assert((last[33] & RST) && nat_core_stats(n).tcp == 0);
}
static void t_all_flows_busy(void) {
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i)
        send_tcp_to(REMOTE, 5000 + i, 80, SYN, 100, 0, 1460, NULL);
    nat_core_poll(n, tick); assert(nat_core_stats(n).tcp == NAT_TCP_MAX);
    unsigned before = nat_core_stats(n).drop;
    send_tcp_to(REMOTE, 6000, 80, SYN, 100, 0, 1460, NULL);
    assert(connects == NAT_TCP_MAX && nat_core_stats(n).drop == before + 1);
    nat_core_reset(n); assert(aborts == NAT_TCP_MAX);
}
/* Regressions below intentionally assert required behaviour and expose bugs. */
static void t_bug_async_refused(void) {
    send_tcp(SYN, 100, 0, 1460, NULL);
    nat_core_tcp_error(n, 0); /* lwIP reports refusal through tcp_err, not connected. */
    assert(emitted == 1 && (last[33] & RST));
}
static void t_bug_forward_after_five_minutes(void) {
    tick = 300001;
    assert(nat_core_inbound_accept(n, 8080, tick) == 0);
    nat_core_poll(n, tick);
    assert(nat_core_stats(n).tcp == 1);
}
static void t_bug_data_before_handshake(void) {
    send_tcp(SYN, 100, 0, 1460, NULL); nat_core_tcp_connected(n, 0, 0, tick);
    send_tcp(SYN, 100, 0, 1460, NULL); /* SYN retransmission updates peer_wnd. */
    assert(nat_core_tcp_received(n, 0, (const unsigned char *)"banner", 6, tick));
    unsigned before = emitted; nat_core_poll(n, ++tick);
    assert(emitted == before); /* The console has not acknowledged the SYN yet. */
}
static void t_bug_reset_releases_forwards(void) {
    assert(nat_core_forward_add(n, 8080, 1));
    nat_core_reset(n);
    assert(removed == 1);
}

typedef struct { const char *name; void (*run)(void); } test_t;
#define TEST(name) {#name, name}
static const test_t tests[] = {
    TEST(t_icmp), TEST(t_udp), TEST(t_dns_front), TEST(t_tcp), TEST(t_tcp_data_fin),
    TEST(t_stray), TEST(t_refused), TEST(t_forward), TEST(t_forward_cap), TEST(t_syn_reuse),
    TEST(t_udp_oversize), TEST(t_backpressure), TEST(t_garbage),
    TEST(t_window_and_receive_credit), TEST(t_sequence_wrap), TEST(t_fin_grace_and_time_wait),
    TEST(t_fin_retry_limits), TEST(t_forward_owner_lifetime), TEST(t_data_retry_limits), TEST(t_all_flows_busy), TEST(t_retry_limits), TEST(t_bug_async_refused), TEST(t_bug_forward_after_five_minutes),
    TEST(t_bug_data_before_handshake), TEST(t_bug_reset_releases_forwards)
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); ++i) puts(tests[i].name);
        return 0;
    }
    nat_ops_t ops = {emit, connect_, write_, shutdown_, abort_, udp, fadd, fdel, got, NULL};
    assert(nat_core_size() <= sizeof(store)); nat_core_init(n, &ops);
    for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); ++i) if (!strcmp(argv[1], tests[i].name)) {
        tests[i].run(); printf("PASS %s\n", tests[i].name); return 0;
    }
    return 2;
}
