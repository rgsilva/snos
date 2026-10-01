/* Run the unchanged console app against real nat_core. Only COP/stream/clock
 * boundaries are simulated: 511 usable TX bytes, 2047 usable RX bytes, uIP's
 * 1460/zero advertised window, and ip_close's abort (RST) behavior. */
#define main original_nat_harness_main
#include "nat_harness.c"
#undef main

#define SNOS_ABI_H
#define SNOS_APPLIB_H
#define SNOS_ARGV_MAX 128
#define SYS_ERR 0xffffu
#define NET_ST_OPENING 1
#define NET_ST_OPEN 2
#define SL_OPEN_OK 0
#define SL_OPEN_CANCELLED 2
static struct { uint16_t p0, p1; } test_args;
#define SNOS_ARGS test_args
static uint16_t sys_frames(void), sys_yield(void), sys_pollkey(void);
static uint16_t sys_net_state(uint16_t ch), sys_net_close(uint16_t ch);
static uint16_t sys_net_stats(void), sys_net_connect(const char *host);
static uint16_t sys_net_read(uint16_t ch, void *buf, uint16_t max);
static uint16_t sys_net_write(uint16_t ch, const void *buf, uint16_t len);
static uint16_t sys_net_wspace(uint16_t ch), sys_putu(uint16_t v);
static uint16_t sys_puts(const char *s), sys_putc(uint16_t c);
static uint16_t snos_getargs(char *buf, uint16_t max);
static uint16_t sl_wait_open(uint16_t ch, uint16_t timeout, int cancel);
static void sl_putu32(uint32_t value);
static void sl_hostport(char *dst, int cap, const char *src, const char *suffix);
#define main speed_console_main
/* 16-bit console integer promotions differ from this host. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../../apps/speedtest/speedtest.c"
#pragma GCC diagnostic pop
#undef main

static struct stream {
    bool open, ack_pending;
    uint32_t seq, ack, start, counted, read_count;
    uint16_t port, frames;
    unsigned rxlen, txlen, control_len;
    char kind, control[24];
    uint8_t rx[2047], tx[511];
} streams[4];
static unsigned opened, frame, max_open;
static char transcript[4096];
static size_t transcript_len;
static unsigned seconds = 1;
static unsigned cancel_after_opened;
static uint16_t window(const struct stream *s) { return s->rxlen <= 587 ? 1460 : 0; }
static void stream_send(struct stream *s, unsigned flags, const uint8_t *p, size_t len) {
    uint8_t b[NAT_MTU] = {0};
    wr16(b, s->port); wr16(b + 2, 7778); wr32(b + 4, s->seq); wr32(b + 8, s->ack);
    b[12] = 0x50; b[13] = flags; wr16(b + 14, window(s));
    if (len) memcpy(b + 20, p, len);
    wr16(b + 16, fold(b, len + 20, pseudo(SNES, PICO, 6, len + 20)));
    send_ip(6, PICO, b, len + 20);
}
static void console_emit(void *arg, const uint8_t *p, size_t len) {
    emit(arg, p, len);
    struct stream *s = NULL;
    for (unsigned i = 0; i < opened; ++i)
        if (streams[i].port == rd16(p + 22)) s = &streams[i];
    assert(s && s->open && !(p[33] & (FIN | RST)));
    unsigned off = 20 + (p[32] >> 4) * 4;
    if (p[33] & SYN) {
        s->ack = rd32(p + 24) + 1;
        return;
    }
    if (len > off && rd32(p + 24) == s->ack) {
        assert(len - off <= sizeof(s->rx) - s->rxlen);
        memcpy(s->rx + s->rxlen, p + off, len - off);
        s->rxlen += len - off;
        s->ack += len - off;
        s->ack_pending = true;
    }
}
/* Independent observer of bytes put on the console-to-peer wire. */
static void observe(struct stream *s, const uint8_t *p, unsigned len) {
    for (unsigned i = 0; i < len; ++i) {
        if (!s->start) {
            assert(s->control_len < sizeof(s->control) - 1);
            s->control[s->control_len++] = p[i];
            if (p[i] == '\n') {
                s->control[s->control_len] = 0;
                if (!s->kind) {
                    unsigned frames;
                    assert(sscanf(s->control, "%c %u", &s->kind, &frames) == 2);
                    assert(s->kind == 'D' || s->kind == 'U');
                    s->frames = frames;
                } else {
                    assert(!strcmp(s->control, "GO\n"));
                    s->start = tick;
                }
                s->control_len = 0;
            }
        } else {
            assert(s->kind == 'U' && p[i] == 'x');
            if (tick - s->start < s->frames * 1000u / 60u) ++s->counted;
        }
    }
}
static void service(void) {
    ++frame;
    assert(frame < 30000);
    tick = 10 + frame * 1000 / 60;
    for (unsigned i = 0; i < opened; ++i) {
        struct stream *s = &streams[i];
        if (!s->open) continue;
        if (s->txlen) {
            unsigned len = s->txlen < 128 ? s->txlen : 128;
            observe(s, s->tx, len);
            stream_send(s, PSH | ACK, s->tx, len);
            s->seq += len;
            memmove(s->tx, s->tx + len, s->txlen - len);
            s->txlen -= len;
            s->ack_pending = false;
        } else if (s->ack_pending) {
            stream_send(s, ACK, NULL, 0);
            s->ack_pending = false;
        }
    }
    nat_core_poll(n, tick);
}
static uint16_t sys_frames(void) { service(); return frame; }
static uint16_t sys_yield(void) { service(); return 0; }
static uint16_t sys_pollkey(void) {
    if (cancel_after_opened && opened == cancel_after_opened &&
        streams[opened - 1].start && tick - streams[opened - 1].start >= 100)
        return 'q';
    return SYS_ERR;
}
static uint16_t sys_net_state(uint16_t ch) { return streams[ch].open ? NET_ST_OPEN : 0; }
static uint16_t sys_net_close(uint16_t ch) {
    stream_send(&streams[ch], RST | ACK, NULL, 0);
    streams[ch].open = false;
    return 0;
}
static uint16_t sys_net_stats(void) { return emitted; }
static uint16_t sys_net_connect(const char *target) {
    assert(!strcmp(target, "192.0.2.10:7778") && opened < 4);
    unsigned ch = opened++;
    struct stream *s = &streams[ch];
    s->open = true; s->port = 5500 + ch; s->seq = 100;
    stream_send(s, SYN, NULL, 0);
    s->seq++;
    stream_send(s, ACK, NULL, 0);
    unsigned active = 0;
    for (unsigned i = 0; i < opened; ++i) active += streams[i].open;
    if (active > max_open) max_open = active;
    return ch;
}
static uint16_t sys_net_read(uint16_t ch, void *buf, uint16_t max) {
    struct stream *s = &streams[ch];
    unsigned len = s->rxlen < max ? s->rxlen : max;
    memcpy(buf, s->rx, len);
    memmove(s->rx, s->rx + len, s->rxlen - len);
    s->rxlen -= len;
    if (len) s->ack_pending = true;
    if (s->kind == 'D' && s->start) {
        for (unsigned i = 0; i < len; ++i) assert(((uint8_t *)buf)[i] == 'x');
        s->read_count += len;
    }
    return len;
}
static uint16_t sys_net_write(uint16_t ch, const void *buf, uint16_t len) {
    struct stream *s = &streams[ch];
    if (len > sizeof(s->tx) - s->txlen) len = sizeof(s->tx) - s->txlen;
    memcpy(s->tx + s->txlen, buf, len); s->txlen += len;
    return len;
}
static uint16_t sys_net_wspace(uint16_t ch) { return sizeof(streams[ch].tx) - streams[ch].txlen; }
static uint16_t sys_puts(const char *s) {
    assert(transcript_len + strlen(s) < sizeof(transcript));
    strcpy(transcript + transcript_len, s); transcript_len += strlen(s); return 0;
}
static uint16_t sys_putc(uint16_t c) { char text[] = {c, 0}; return sys_puts(text); }
static void sl_putu32(uint32_t value) {
    char text[24]; snprintf(text, sizeof(text), "%u", value); sys_puts(text);
}
static uint16_t sys_putu(uint16_t value) { sl_putu32(value); return 0; }
static uint16_t snos_getargs(char *buf, uint16_t max) {
    snprintf(buf, max, "192.0.2.10 %u", seconds); return strlen(buf);
}
static uint16_t sl_wait_open(uint16_t ch, uint16_t timeout, int cancel) {
    (void)timeout; (void)cancel; assert(streams[ch].open); return SL_OPEN_OK;
}
static void sl_hostport(char *dst, int cap, const char *src, const char *suffix) {
    assert(strlen(src) + strlen(suffix) < (size_t)cap);
    strcpy(dst, src); strcat(dst, suffix);
}
int main(int argc, char **argv) {
    if (argc == 2) {
        if (!strcmp(argv[1], "cancel-download")) cancel_after_opened = 1;
        else if (!strcmp(argv[1], "cancel-upload")) cancel_after_opened = 2;
        else if (!strcmp(argv[1], "cancel-duplex")) cancel_after_opened = 4;
        else seconds = (unsigned)atoi(argv[1]);
    }
    nat_ops_t ops = {console_emit, connect_, write_, shutdown_, abort_, udp,
                     fadd, fdel, got, NULL};
    nat_core_init(n, &ops); nat_core_set_local_address(n, PICO);
    int result = speed_console_main();
    puts(transcript);
    if (cancel_after_opened) {
        assert(result == 1 && opened == cancel_after_opened);
        assert(strstr(transcript, "speedtest: cancelled\n"));
        for (unsigned i = 0; i < opened; ++i) assert(!streams[i].open);
        nat_core_poll(n, ++tick);
        assert(nat_core_stats(n).tcp == 0 && connects == 0 && writes == 0);
        printf("PASS actual_console_app_%s\n", argv[1]);
        return 0;
    }
    assert(result == 0 && opened == 4 && max_open == 2);
    assert(streams[2].start >= streams[3].start); /* upload GO precedes download GO */
    const char *labels[] = {"download: ", "upload: ", "both download: ", "both upload: "};
    for (unsigned i = 0; i < 4; ++i) {
        char *line = strstr(transcript, labels[i]);
        unsigned rate_value, count_value;
        assert(line && sscanf(line + strlen(labels[i]), "%u B/s (%u bytes)",
                              &rate_value, &count_value) == 2);
        unsigned expected = streams[i].kind == 'U' ? streams[i].counted : streams[i].read_count;
        assert(expected > 0 && count_value == expected && rate_value == expected / seconds);
        assert(!streams[i].open);
    }
    nat_core_poll(n, ++tick);
    assert(nat_core_stats(n).tcp == 0 && connects == 0 && writes == 0);
    printf("PASS actual_console_app_%us\n", seconds);
    return 0;
}
