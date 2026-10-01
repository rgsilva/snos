/* Link windows, end to end: the real console sender and receiver
 * (snes/kernel/net.c over link_serial.c) against the real Pico sender and
 * receiver (pico/link.c, with ctrl.c answering HELLO), over a channel that
 * loses, corrupts, repeats, skips and delays frames, at a given pair of
 * windows. What must hold: every byte each side queued arrives exactly once
 * and in order -- IP both ways, keyboard records toward the console -- and
 * once the faults stop, everything still queued gets through.
 *
 *   window_harness <console window> <pico window> <seed> [faults]
 *
 * The channel stands in for pico/port.c as well as the wire: the Pico makes
 * one reply per frame it received, and each exchange carries the oldest reply
 * not yet sent, one or two exchanges after it was made -- or, by fault,
 * repeats the last one, skips one, or loses or corrupts a frame either way.
 * The console also stops reading its IP ring for stretches, so its receiver
 * refuses frames and the Pico's window fills against it. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link.h"
#include "ctrl.h"

/* --- the console, built by console_host.py --- */
void net_init(void);
void net_service(void);
int net_hello(void);
int net_ipmode(void);
int net_ip_read(uint8_t *dst, int max);
int net_ip_write(const uint8_t *src, int n);
int net_ip_wspace(void);
int net_tune_set(int what, int value);
extern volatile uint16_t net_stat_refused, net_stat_resent;

static uint8_t regs[0x10000];
volatile uint8_t *rig_reg(unsigned long a) { return &regs[a & 0xffff]; }
volatile uint16_t snos_frames, rig_in_windex, rig_in_rindex;
void link_wram_init(void) {}
void link_wram_reset(void) {}
int link_wram_present(void) { return 0; }
int link_wram_recv(uint8_t *rx) { (void)rx; return 0; }
void link_wram_send(const uint8_t *tx) { (void)tx; }
uint8_t net_checksum(const uint8_t *f, uint16_t n) {
    uint8_t s = 0;
    for (uint16_t i = 0; i < n; ++i) if (i != 3) s += f[i];
    return s;
}
void ring_move(uint8_t *dst, const uint8_t *src, uint16_t n) { memmove(dst, src, n); }
void link_mb_move(void *dst, const void *src, uint16_t n) { memmove(dst, src, n); }
uint16_t ip_sum(const uint8_t *p, uint16_t n) { (void)p; (void)n; return 0; }
void proc_lock(void) {}
void proc_unlock(void) {}
uint8_t proc_current(void) { return 0; }
uint8_t proc_fg(void) { return 0; }
void proc_switch_now(void) {}
void kbd_idle(void) {}
void link_serial_kick(void) {}
uint8_t beam_line(void) { return 0; } /* always early in the frame: never refused */
bool relay_send(uint8_t type, const uint8_t *data, size_t len) {
    (void)type; (void)data; (void)len;
    return false;
}
void log_puts(const char *s) { (void)s; }

/* --- the streams --- */
#define STREAM 400000
static uint8_t snes_out[STREAM], pico_out[STREAM], kbd_out[STREAM];
static size_t snes_sent, snes_seen;   /* console -> Pico, IP */
static size_t pico_sent, pico_seen;   /* Pico -> console, IP (with prefixes) */
static size_t kbd_sent, kbd_seen;     /* Pico -> console, keyboard */
int kbd_push(int c) {
    assert(kbd_seen < kbd_sent && kbd_out[kbd_seen] == (uint8_t)c);
    ++kbd_seen;
    return 0;
}

static unsigned rnd(unsigned n) { return (unsigned)(random() % n); }
static int chance(unsigned per_mille) { return rnd(1000) < per_mille; }

/* --- the channel --- */
#define MAXQ 64
static struct { uint8_t f[LINK_FRAME]; unsigned eligible; } reply[MAXQ];
static unsigned rq_head, rq_count, exchange_no;
static uint8_t last_sent[LINK_FRAME];
static bool faults;
static unsigned stats_lost, stats_corrupt, stats_dup, stats_skip;

static void pico_receive(const uint8_t *f) {
    link_rx_t r = link_on_rx(f);
    for (size_t i = 0; i < r.count; ++i) {
        const link_record_t *rec = &r.records[i];
        if (rec->ch == CH_CTRL) ctrl_bytes(rec->data, rec->len);
        else if (rec->ch == CH_IP) {
            assert(snes_seen + rec->len <= snes_sent);
            assert(!memcmp(snes_out + snes_seen, rec->data, rec->len));
            snes_seen += rec->len;
        }
    }
    ctrl_poll();
    assert(rq_count < MAXQ);
    unsigned k = (rq_head + rq_count++) % MAXQ;
    link_next_out(reply[k].f);
    reply[k].eligible = exchange_no + 1 + (faults ? rnd(2) : 0);
}

void link_serial_xfer(const uint8_t *tx, uint8_t *rx, uint16_t n) {
    assert(n == LINK_FRAME);
    ++exchange_no;
    /* What the console clocks in: the oldest reply due, or a repeat. */
    if (rq_count && reply[rq_head].eligible <= exchange_no && !(faults && chance(20))) {
        if (faults && rq_count > 1 && chance(10)) {
            rq_head = (rq_head + 1) % MAXQ; /* staged, then replaced unsent */
            --rq_count;
            ++stats_skip;
        }
        memcpy(last_sent, reply[rq_head].f, LINK_FRAME);
        rq_head = (rq_head + 1) % MAXQ;
        --rq_count;
    } else {
        ++stats_dup;
    }
    memcpy(rx, last_sent, LINK_FRAME);
    if (faults && chance(15)) {
        rx[LINK_HDR + rnd(LINK_FRAME - LINK_HDR)] ^= (uint8_t)(1 + rnd(255));
        ++stats_corrupt;
    }
    /* What the Pico clocks in. A frame lost on the wire is a short burst:
     * no reply is made for it. */
    if (faults && chance(15)) {
        ++stats_lost;
        return;
    }
    uint8_t f[LINK_FRAME];
    memcpy(f, tx, LINK_FRAME);
    if (faults && chance(15)) {
        f[rnd(LINK_FRAME)] ^= (uint8_t)(1 + rnd(255));
        ++stats_corrupt;
    }
    pico_receive(f);
}

static void console_drain(void) {
    uint8_t buf[300];
    int got;
    for (;;) {
        got = net_ip_read(buf, 1 + (int)rnd(sizeof buf));
        if (got == -1) {
            /* The peer-restart report (net.c's iprx_resync): only the Pico's
             * own start-up SYN may cause one here, before any data. */
            assert(pico_seen == 0);
            continue;
        }
        if (got == 0) return;
        assert(pico_seen + (size_t)got <= pico_sent);
        assert(!memcmp(pico_out + pico_seen, buf, (size_t)got));
        pico_seen += (size_t)got;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    int wc = atoi(argv[1]), wp = atoi(argv[2]);
    srandom((unsigned)atoi(argv[3]));
    bool with_faults = argc < 5 || strcmp(argv[4], "clean");
    const uint8_t addr[4] = {192, 168, 1, 50};

    regs[0x4200] = 0x81;
    link_init();
    ctrl_address(addr);
    uint8_t first[LINK_FRAME];
    link_next_out(first);
    memcpy(last_sent, first, LINK_FRAME);
    net_init();
    net_tune_set(0, wc);
    /* HELLO, which arms the console's IP mode. */
    for (unsigned i = 0; i < 400 && !net_ipmode(); ++i) {
        if (i % 50 == 10) net_hello();
        net_service();
    }
    assert(net_ipmode());
    link_set_window((unsigned)wp);
    faults = with_faults;

    unsigned starve = 0;
    for (unsigned step = 0; step < 40000; ++step) {
        bool feeding = step < 30000;
        if (step == 30000) faults = false;
        if (feeding && net_ip_wspace() > 0 && chance(300)) {
            int n = 1 + (int)rnd(200);
            if (n > net_ip_wspace()) n = net_ip_wspace();
            if (snes_sent + (size_t)n < STREAM) {
                for (int i = 0; i < n; ++i) snes_out[snes_sent + (size_t)i] = (uint8_t)rnd(256);
                assert(net_ip_write(snes_out + snes_sent, n) == n);
                snes_sent += (size_t)n;
            }
        }
        if (feeding && chance(300) && link_queued(CH_IP) < 4000) {
            size_t n = 1 + rnd(1400);
            if (pico_sent + n + 2 < STREAM) {
                uint8_t *p = pico_out + pico_sent;
                p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
                for (size_t i = 0; i < n; ++i) p[2 + i] = (uint8_t)rnd(256);
                if (link_push(CH_IP, p + 2, n)) pico_sent += n + 2;
            }
        }
        if (feeding && chance(30)) {
            size_t n = 1 + rnd(20);
            if (kbd_sent + n < STREAM) {
                for (size_t i = 0; i < n; ++i) kbd_out[kbd_sent + i] = (uint8_t)(' ' + rnd(90));
                if (link_push(CH_KBD, kbd_out + kbd_sent, n)) kbd_sent += n;
            }
        }
        if (starve) --starve;
        else if (faults && chance(5)) starve = 50 + rnd(200);
        if (!starve) console_drain();
        rig_in_rindex = rig_in_windex; /* the shell reads its keys */
        net_service();
    }
    console_drain();
    printf("windows %d/%d: %zu bytes up, %zu down, %zu keys; lost %u corrupt %u repeat %u skip %u"
           " refused %u resent %u\n", wc, wp, snes_seen, pico_seen, kbd_seen, stats_lost,
           stats_corrupt, stats_dup, stats_skip, net_stat_refused, net_stat_resent);
    assert(snes_seen == snes_sent && pico_seen == pico_sent && kbd_seen == kbd_sent);
    assert(snes_sent > 100000 && pico_sent > 100000);
    return 0;
}
