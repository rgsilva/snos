/* The unchanged apps/speedtest/speedtest.c, on the real kernel network calls.
 *
 * Only the COP boundary is replaced: each sys_* below does what traps.c's case
 * for it does (including SYS_NET_WRITE's copy through a 128-byte local) and
 * charges the bench clock for the syscall, so the application's own CPU time
 * competes with the link inside a video frame exactly as it does on the
 * console. The costs live in linkbench_harness.c's table. SYS_NET_WRITE is
 * net_write_far straight from the application's buffer, as traps.c does it. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SNOS_ABI_H
#define SNOS_APPLIB_H
#define SNOS_ARGV_MAX 128
#define SYS_ERR 0xffffu
#define NET_ST_OPENING 1
#define NET_ST_OPEN 2
#define NET_ST_EOF 3
#define SL_OPEN_OK 0
#define SL_OPEN_TIMEOUT 1
#define SL_OPEN_CANCELLED 2
#define SL_OPEN_FAILED 3

/* The kernel, compiled from snes/kernel (see linkbench.py). */
int net_read(int ch, uint8_t *dst, int max);
int net_write_far(int ch, const uint8_t *src, int n);
int net_wspace(int ch);
int net_state(int ch);
int net_connect(const char *hostport);
void net_close(int ch);
extern volatile uint16_t net_stat_rx_frames, net_stat_resent, net_stat_bad_sum;
extern volatile uint16_t snos_frames;

/* The bench (linkbench_harness.c). */
void bench_charge_syscall(unsigned which, unsigned bytes);
void bench_kbd_idle(void);
const char *bench_app_args(void);
void bench_app_output(const char *s);
enum { SC_FRAMES, SC_YIELD, SC_POLLKEY, SC_STATE, SC_CLOSE, SC_STATS, SC_CONNECT,
       SC_READ, SC_WRITE, SC_WSPACE, SC_PUTS };

static struct { uint16_t p0, p1; } bench_args;
#define SNOS_ARGS bench_args

static uint16_t sys_frames(void) {
    bench_charge_syscall(SC_FRAMES, 0);
    return snos_frames;
}
static uint16_t sys_yield(void) {
    bench_charge_syscall(SC_YIELD, 0);
    bench_kbd_idle();
    return 0;
}
static uint16_t sys_pollkey(void) {
    bench_charge_syscall(SC_POLLKEY, 0);
    return SYS_ERR;
}
static uint16_t sys_net_state(uint16_t ch) {
    bench_charge_syscall(SC_STATE, 0);
    return (uint16_t)net_state((int)ch);
}
static uint16_t sys_net_close(uint16_t ch) {
    bench_charge_syscall(SC_CLOSE, 0);
    net_close((int)ch);
    return 0;
}
static uint16_t sys_net_stats(void) {
    bench_charge_syscall(SC_STATS, 0);
    bench_args.p0 = net_stat_resent;
    bench_args.p1 = net_stat_bad_sum;
    return net_stat_rx_frames;
}
static uint16_t sys_net_connect(const char *host) {
    char hp[256];
    int r;
    bench_charge_syscall(SC_CONNECT, 0);
    snprintf(hp, sizeof hp, "%s", host);
    r = net_connect(hp);
    return r < 0 ? SYS_ERR : (uint16_t)r;
}
static uint16_t sys_net_read(uint16_t ch, void *buf, uint16_t max) {
    int n = net_read((int)ch, (uint8_t *)buf, (int)max);
    bench_charge_syscall(SC_READ, (unsigned)(n > 0 ? n : 0));
    return (uint16_t)n;
}
static uint16_t sys_net_write(uint16_t ch, const void *buf, uint16_t len) {
    int n = net_write_far((int)ch, (const uint8_t *)buf, (int)len);
    bench_charge_syscall(SC_WRITE, (unsigned)(n > 0 ? n : 0));
    return (uint16_t)n;
}
static uint16_t sys_net_wspace(uint16_t ch) {
    bench_charge_syscall(SC_WSPACE, 0);
    return (uint16_t)net_wspace((int)ch);
}
static uint16_t sys_puts(const char *s) {
    bench_charge_syscall(SC_PUTS, 0);
    bench_app_output(s);
    return 0;
}
static uint16_t sys_putc(uint16_t c) {
    char text[2] = {(char)c, 0};
    return sys_puts(text);
}
static void sl_putu32(uint32_t value) {
    char text[24];
    snprintf(text, sizeof text, "%u", (unsigned)value);
    sys_puts(text);
}
static uint16_t sys_putu(uint16_t value) {
    sl_putu32(value);
    return 0;
}
static uint16_t snos_getargs(char *buf, uint16_t max) {
    snprintf(buf, max, "%s", bench_app_args());
    return (uint16_t)strlen(buf);
}
/* apps/lib/snoslib.c's sl_wait_open, over the calls above. */
static uint16_t sl_wait_open(uint16_t ch, uint16_t frames, int allow_cancel) {
    uint16_t deadline = (uint16_t)(sys_frames() + frames);
    uint16_t st;
    for (;;) {
        st = sys_net_state(ch);
        if (st != NET_ST_OPENING) break;
        if ((int16_t)(sys_frames() - deadline) >= 0) {
            sys_net_close(ch);
            return SL_OPEN_TIMEOUT;
        }
        if (allow_cancel && sys_pollkey() != SYS_ERR) {
            sys_net_close(ch);
            return SL_OPEN_CANCELLED;
        }
        sys_yield();
    }
    if (st != NET_ST_OPEN && st != NET_ST_EOF) {
        sys_net_close(ch);
        return SL_OPEN_FAILED;
    }
    return SL_OPEN_OK;
}
/* apps/lib/snoslib.c's sl_hostport. */
static void sl_hostport(char *dst, int cap, const char *src, const char *suffix) {
    int i = 0, has = 0;
    while (src[i] && i < cap - 1) {
        if (src[i] == ':') has = 1;
        dst[i] = src[i];
        i++;
    }
    if (!has) {
        int j = 0;
        while (suffix[j] && i < cap - 1) dst[i++] = suffix[j++];
    }
    dst[i] = 0;
}

#define main speedtest_main
/* 16-bit console integer promotions differ from this host. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../../apps/speedtest/speedtest.c"
#pragma GCC diagnostic pop
#undef main

int bench_run_speedtest(void) { return speedtest_main(); }
