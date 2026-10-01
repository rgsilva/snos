/* Link throughput bench (docs/link-throughput.md).
 *
 * The real console link and stack -- snes/kernel/net.c, link.c,
 * link_serial.c, ip.c and uIP, with apps/speedtest/speedtest.c on top --
 * coupled to the real Pico link layer (pico/link.c), control channel
 * (pico/ctrl.c) and NAT core with its built-in speedtest peer
 * (pico/nat_core.c), through a timing model of the controller-port cable.
 *
 * What is modelled, and nothing else:
 *   - time, in 21.477 MHz master cycles: 1364 per scanline, 262 lines,
 *     vblank (and NMI) at line 225;
 *   - the vblank auto-joypad read: it clocks port 2 every frame, and the
 *     Pico's gap detector restarts its shifter ~64 us after it ends;
 *   - pico/port.c's TX staging: the frame shifted out in an exchange is the
 *     one latched at the most recent restart before its first clock -- the
 *     DMA restart at the end of the previous exchange, an auto-read gap, or
 *     a kick gap -- and the Pico's main loop needs `pico_us` to turn a
 *     received frame into the next port_set_tx();
 *   - the cost of every piece of console work (the `cost` table, from the
 *     -O0 listings and Mesen measurements of the LINK_HW ROM), charged as
 *     the real code runs, so an NMI lands where it would and a foreground
 *     exchange either fits before vblank or does not;
 *   - kbd_idle()'s spin, which is input.c's logic restated (it reads the V
 *     counter); the decision whether a foreground exchange may start is the
 *     kernel's own link_serial_fg_fits() when the kernel has one.
 *
 * Everything that decides what is on the wire is the production code. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link.h"
#include "ctrl.h"
#include "nat_core.h"

/* ------------------------------------------------------------- the clock */

#define MC_LINE 1364ull
#define LINES 262ull
#define MC_FRAME (MC_LINE * LINES)
#define VBLANK_LINE 225ull
#define MC_PER_US 21.47727
#define AR_START 200ull   /* auto-joypad read: begins this long after NMI */
#define AR_LEN 4224ull    /* ...and clocks the port for this long */
#define PIO_GAP 1376ull   /* the PIO's 64.08 us countdown */
#define PIO_IRQ 300ull    /* gap IRQ to restart() done, a generous ~14 us */

typedef struct { const char *name; double v; } cost_t;
/* Master cycles unless the name says otherwise. Overridable as name=value. */
static cost_t cost[] = {
    /* Measured on the LINK_HW ROM under Mesen (make LINK_HW=1 test-hwspeed
     * HWPROF=1), NMI time excluded from foreground spans; every figure
     * includes the ~3% DRAM refresh steals. */
    {"nmi_entry", 1500},     /* shim, snos_frames++ */
    {"pad_latch", 300},      /* after the auto-read finished (4593 incl. the wait) */
    {"nmi_tail", 24000},     /* proc_tick (21942), kill switch, shim exit */
    {"svc_fixed", 6200},     /* net_service's own C, beyond what is charged below */
    {"recv_copy", 0},        /* link_serial_recv: an MVN now, charged as one */
    {"frame_ok", 3000},      /* link_serial_send after the exchange, checksum aside */
    {"checksum_byte", 47},   /* net_checksum, per byte */
    {"move_fixed", 400},     /* ring_move / link_mb_move call */
    {"move_byte", 46},       /* MVN */
    {"xfer_entry", 200},     /* link_serial_xfer up to its quiet loop */
    {"xfer_quiet", 6800},    /* the >= 300 us of silence */
    {"xfer_byte", 1644},     /* 8 bit cells + byte overhead (104858 first-to-last) */
    {"kick", 450},           /* link_serial_kick: eight bare $4017 reads */
    {"idle_entry", 2000},    /* kbd_idle: proc_switch_now and the tests */
    {"spin", 500},           /* one turn of kbd_idle's spin (net_fg_wanted 352) */
    {"after_spin", 29000},   /* osk_redraw + con_flush with nothing dirty (28035) */
    {"ips_fixed", 40000},    /* one ip_service pass, uip_process excluded */
    {"uip_in_fixed", 30000}, /* uip_input on a datagram (32k for a 40-byte ACK)... */
    {"uip_in_byte", 7},      /* ...per byte beyond ip_sum and the ring copy (179k/1500) */
    {"uip_out_fixed", 25000},/* a segment out: header, ip_output... */
    {"uip_out_byte", 7},     /* ...per byte beyond ip_sum and the copy (79k for 511) */
    {"sys_fixed", 11800},    /* COP dispatch (SYS_FRAMES 12.7k, NET_STATE 12.3k) */
    {"sys_write_byte", 150}, /* SYS_NET_WRITE: far_copy into tmp[128] (43k for 128) */
    {"sys_read_fixed", 3000},/* net_read beyond the MVN */
    {"app_loop", 1000},      /* the app's own C per syscall */
    {"pico_us", 300},        /* Pico main loop: received frame to port_set_tx */
    {"pico_us_slow", 3000},  /* ...and its occasional long turn */
    {"pico_slow_every", 0},  /* one turn in N is slow (0: never) */
};
#define NCOST (sizeof cost / sizeof *cost)
static double C(const char *name) {
    for (size_t i = 0; i < NCOST; ++i)
        if (!strcmp(cost[i].name, name)) return cost[i].v;
    fprintf(stderr, "no cost %s\n", name);
    abort();
}
/* Hot entries, resolved once. */
static uint64_t c_move_fixed, c_move_byte, c_checksum_byte, c_uip_in_fixed, c_uip_in_byte,
    c_uip_out_fixed, c_uip_out_byte, c_spin;

static uint64_t now;          /* master cycles since frame 0 line 0 */
static uint64_t next_nmi = VBLANK_LINE * MC_LINE;
static bool in_nmi;
static uint8_t regs[0x10000]; /* the S-CPU registers the kernel pokes */
volatile uint8_t *rig_reg(unsigned long a) { return &regs[a & 0xffff]; }
static bool nmi_enabled(void) { return (regs[0x4200] & 0x80) != 0; }

volatile uint16_t snos_frames;
volatile uint16_t rig_in_windex, rig_in_rindex;

static struct {
    uint64_t nmi, nmi_late, nmi_late_max, nmi_span, masked_vblank, collisions;
    uint64_t xfer_nmi, xfer_fg, kicks, fg_frames_with[8];
    uint64_t t_nmi, t_xfer, t_ips, t_app, t_idle;
} st;

static void run_nmi(void);
static void charge(uint64_t c) {
    while (!in_nmi && nmi_enabled() && now + c >= next_nmi) {
        if (next_nmi > now) {
            c -= next_nmi - now;
            now = next_nmi;
        } else if (now > next_nmi) {
            ++st.nmi_late;
            if (now - next_nmi > st.nmi_late_max) st.nmi_late_max = now - next_nmi;
        }
        run_nmi();
    }
    if (!in_nmi && !nmi_enabled() && now + c >= next_nmi && now < next_nmi) ++st.masked_vblank;
    now += c;
}
static unsigned line_now(void) { return (unsigned)((now % MC_FRAME) / MC_LINE); }

/* ------------------------------------------------------- the console side */

void net_init(void);
void net_service(void);
void net_service_fg(void);
int net_fg_wanted(void);
void ip_init(void);
void ip_service(void);
int ip_up(void);
int net_ip_read(uint8_t *dst, int max);
int net_ip_write(const uint8_t *src, int n);
int net_ip_wspace(void);
extern volatile uint16_t net_stat_rx_frames, net_stat_resent, net_stat_bad_sum, net_stat_refused,
    net_gw_version;
extern volatile uint16_t ip_stat_drop, ip_stat_desync;
extern char net_text[];
extern unsigned char uip_buf[];
extern void *uip_sappdata;
int bench_run_speedtest(void);
/* Present once the kernel can run more than one exchange a frame. */
int link_serial_fg_fits(uint16_t line) __attribute__((weak));
void net_tune_set(int what, int value) __attribute__((weak));
int net_tune_get(int what) __attribute__((weak));

/* The emulator mailbox: never claimed here. */
void link_wram_init(void) {}
void link_wram_reset(void) {}
int link_wram_present(void) { return 0; }
int link_wram_recv(uint8_t *rx) { (void)rx; return 0; }
void link_wram_send(const uint8_t *tx) { (void)tx; }

/* kernel/netasm.s, in C, with their cost. */
uint8_t net_checksum(const uint8_t *f, uint16_t n) {
    uint8_t s = 0;
    for (uint16_t i = 0; i < n; ++i)
        if (i != 3) s += f[i];
    charge(c_checksum_byte * n);
    return s;
}
static bool tracing(void) { return getenv("BENCH_TRACE") != NULL; }
static void tl(const char *what) {
    if (tracing())
        fprintf(stderr, "tl %6llu %-14s line %3u\n", (unsigned long long)(st.xfer_nmi + st.xfer_fg),
                what, (unsigned)((now % MC_FRAME) / MC_LINE));
}
static void move_cost(const void *dst, uint16_t n) {
    charge(c_move_fixed + c_move_byte * n);
    if (dst == uip_sappdata) {
        tl("seg out start");
        charge(c_uip_out_fixed + c_uip_out_byte * n);
        tl("seg out done");
    }
}
void ring_move(uint8_t *dst, const uint8_t *src, uint16_t n) {
    memmove(dst, src, n);
    move_cost(dst, n);
}
void link_mb_move(void *dst, const void *src, uint16_t n) {
    memmove(dst, src, n);
    move_cost(dst, n);
    /* ip_deliver's copy into uIP's buffer: the datagram is about to go
     * through uip_input, whose checksum loop is C. */
    if (dst == (void *)uip_buf) {
        tl("uip in start");
        charge(c_uip_in_fixed + c_uip_in_byte * n);
        tl("uip in done");
    }
}
uint16_t ip_sum(const uint8_t *p, uint16_t n) {
    uint32_t s = 0;
    for (uint16_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    charge(c_checksum_byte * n);
    return (uint16_t)s;
}
static unsigned keys;
int kbd_push(int c) { (void)c; ++keys; return 0; }
void proc_lock(void) {}
void proc_unlock(void) {}
uint8_t proc_current(void) { return 0; }
uint8_t proc_fg(void) { return 0; }
void proc_switch_now(void) {}
void kbd_idle(void);

/* ---------------------------------------------------------- the Pico side */

typedef union { max_align_t align; unsigned char bytes[40000]; } store_t;
static store_t store;
static nat_t *core = (nat_t *)&store;
static const uint8_t pico_ip[4] = {192, 168, 1, 50};
static uint32_t pico_ms;
static uint32_t pico_addr(void) {
    return (uint32_t)pico_ip[0] << 24 | (uint32_t)pico_ip[1] << 16 | (uint32_t)pico_ip[2] << 8 |
           pico_ip[3];
}
uint32_t get_absolute_time(void) { return pico_ms; }
bool relay_send(uint8_t type, const uint8_t *data, size_t len) {
    (void)type; (void)data; (void)len;
    return false;
}
void log_puts(const char *s) { (void)s; }

/* pico/nat.c's adapter, minus lwIP: the local speedtest flow never leaves
 * nat_core, so only the link-facing half matters here. */
static bool emit_try(void *arg, const uint8_t *p, size_t len) {
    (void)arg;
    if (getenv("BENCH_TRACE"))
        fprintf(stderr, "trace %6llu down %zu flags %02x\n",
                (unsigned long long)(st.xfer_nmi + st.xfer_fg), len, len >= 40 ? p[33] : 0);
    return link_push(CH_IP, p, len);
}
static void emit(void *arg, const uint8_t *p, size_t len) { (void)emit_try(arg, p, len); }
static bool link_busy(void *arg) {
    (void)arg;
    return link_queued(CH_IP) != 0;
}
static bool no_connect(void *a, unsigned s, uint32_t ip, uint16_t port) {
    (void)a; (void)s; (void)ip; (void)port;
    return false;
}
static bool no_write(void *a, unsigned s, const uint8_t *d, size_t n) {
    (void)a; (void)s; (void)d; (void)n;
    return false;
}
static void no_slot(void *a, unsigned s) { (void)a; (void)s; }
static bool no_udp(void *a, unsigned s, uint32_t ip, uint16_t port, const uint8_t *d, size_t n) {
    (void)a; (void)s; (void)ip; (void)port; (void)d; (void)n;
    return false;
}
static bool no_fwd(void *a, uint16_t port) { (void)a; (void)port; return false; }
static void no_fdel(void *a, uint16_t port) { (void)a; (void)port; }
static void no_recved(void *a, unsigned s, size_t n) { (void)a; (void)s; (void)n; }
static uint8_t ipbuf[LINK_MTU + 2];
static size_t ipused;
void ip_stream_reset(void) { ipused = 0; }
static uint64_t raw_rx_pico; /* raw mode: IP stream bytes the Pico received */
static bool raw_mode;
static void ip_stream_bytes(const uint8_t *data, size_t len) {
    if (raw_mode) {
        raw_rx_pico += len;
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        ipbuf[ipused++] = data[i];
        if (ipused < 2) continue;
        size_t n = ipbuf[0] | ((size_t)ipbuf[1] << 8);
        assert(n && n <= LINK_MTU);
        if (ipused == n + 2) {
            if (getenv("BENCH_TRACE"))
                fprintf(stderr, "trace %6llu up   %zu flags %02x\n",
                        (unsigned long long)(st.xfer_nmi + st.xfer_fg), n,
                        n >= 40 ? ipbuf[2 + 33] : 0);
            nat_core_input(core, ipbuf + 2, n, pico_ms);
            ipused = 0;
        }
    }
}

/* pico/port.c, as a timeline: replies with the time each became pending.
 * Two staging policies. "latest" is port.c before link windows: every
 * restart latches the newest pending frame, so a frame latched but not yet
 * shifted out can be replaced unsent. "fifo" latches replies in order and
 * replaces the latched frame only once it has gone out whole -- a repeat of
 * the last frame fills an exchange the Pico had no answer ready for. */
#define REPLIES 64
static struct { uint64_t ready; uint8_t f[LINK_FRAME]; } reply[REPLIES];
static unsigned reply_head, reply_count;
static uint8_t latched[LINK_FRAME];
static bool staging_fifo, latched_sent = true;
static uint64_t skipped, filled;
static uint64_t last_xfer_end, last_kick_end;
static unsigned pico_turns;
static uint64_t raw_tx_pico;
static void pico_set_tx(const uint8_t *f, uint64_t ready) {
    unsigned i = (reply_head + reply_count) % REPLIES;
    if (reply_count == REPLIES) {
        reply_head = (reply_head + 1) % REPLIES;
        --reply_count;
    }
    reply[i].ready = ready;
    memcpy(reply[i].f, f, LINK_FRAME);
    ++reply_count;
}
/* restart(): stage what the policy allows at time t. `sent` says the
 * restart follows a whole exchange, so the latched frame went out. */
static void pico_restage(uint64_t t, bool sent) {
    if (sent) latched_sent = true;
    if (staging_fifo) {
        if (latched_sent && reply_count && reply[reply_head].ready <= t) {
            memcpy(latched, reply[reply_head].f, LINK_FRAME);
            reply_head = (reply_head + 1) % REPLIES;
            --reply_count;
            latched_sent = false;
        }
        return;
    }
    while (reply_count && reply[reply_head].ready <= t) {
        if (!latched_sent && (latched[2] & F_DATA)) ++skipped;
        memcpy(latched, reply[reply_head].f, LINK_FRAME);
        reply_head = (reply_head + 1) % REPLIES;
        --reply_count;
        latched_sent = false;
    }
}
static void pico_main_turn(const uint8_t *rx, uint64_t t_end) {
    double us = C("pico_us");
    unsigned every = (unsigned)C("pico_slow_every");
    if (every && ++pico_turns % every == 0) us = C("pico_us_slow");
    uint64_t t = t_end + (uint64_t)(us * MC_PER_US);
    pico_ms = (uint32_t)(t / (MC_PER_US * 1000.0));
    link_rx_t r = link_on_rx(rx);
    if (r.reset) {
        ctrl_reset();
        nat_core_reset(core);
    }
    for (size_t i = 0; i < r.count; ++i) {
        if (r.records[i].ch == CH_CTRL) ctrl_bytes(r.records[i].data, r.records[i].len);
        else if (r.records[i].ch == CH_IP) ip_stream_bytes(r.records[i].data, r.records[i].len);
    }
    /* wifi_poll -> nat_configure(true) restates both on every pass. */
    ctrl_address(pico_ip);
    nat_core_set_local_address(core, pico_addr());
    if (raw_mode) {
        static uint8_t dgram[1460];
        while (link_queued(CH_IP) < 4 * LINK_FRAME) {
            dgram[0] = 0x45;
            if (!link_push(CH_IP, dgram, sizeof dgram)) break;
            raw_tx_pico += sizeof dgram + 2;
        }
    } else {
        nat_core_poll(core, pico_ms);
    }
    ctrl_poll();
    uint8_t tx[LINK_FRAME];
    link_next_out(tx);
    pico_set_tx(tx, t);
}

/* ------------------------------------------------------- the wire, watched */

typedef struct {
    uint64_t frames, data, fresh, repeat, empty, payload, ip_payload;
    int high; /* highest sequence seen, unwrapped */
    bool seen;
} dir_t;
static dir_t to_pico, to_snes;
static int unwrap(dir_t *d, uint8_t seq) {
    if (!d->seen) return seq;
    int base = d->high & ~0xff;
    int s = base | seq;
    if (s < d->high - 128) s += 256;
    else if (s > d->high + 128) s -= 256;
    return s;
}
static void watch(dir_t *d, const uint8_t *f) {
    ++d->frames;
    if (!(f[2] & F_DATA)) {
        ++d->empty;
        return;
    }
    ++d->data;
    int s = unwrap(d, f[0]);
    if (!d->seen || s > d->high) {
        if (d->seen && s > d->high + 1) {}
        d->high = s;
        d->seen = true;
        ++d->fresh;
        for (unsigned o = LINK_HDR; o + 2 <= LINK_FRAME;) {
            uint8_t ch = f[o], n = f[o + 1];
            if (ch == CH_PAD || o + 2u + n > LINK_FRAME) break;
            d->payload += n;
            if (ch == CH_IP) d->ip_payload += n;
            o += 2u + n;
        }
    } else {
        ++d->repeat;
    }
}

/* One exchange: kernel/linkxfer.s. */
void link_serial_kick(void) {
    charge((uint64_t)C("kick"));
    last_kick_end = now;
    ++st.kicks;
}
static bool overlaps_autoread(uint64_t a, uint64_t b) {
    uint64_t f = a / MC_FRAME;
    for (uint64_t k = f ? f - 1 : 0; k <= b / MC_FRAME + 1; ++k) {
        uint64_t s = k * MC_FRAME + VBLANK_LINE * MC_LINE + AR_START;
        if (s < b && s + AR_LEN > a) return true;
    }
    return false;
}
static uint64_t c_nmi_entry, c_pad_latch, c_nmi_tail, c_svc_fixed, c_recv_copy, c_frame_ok;
void link_serial_xfer(const uint8_t *tx, uint8_t *rx, uint16_t n) {
    assert(n == LINK_FRAME);
    /* net_service's own C before the exchange, which has no hook of its
     * own, and link_serial_send's check after it. */
    charge(c_svc_fixed + c_recv_copy);
    uint64_t t0 = now;
    charge((uint64_t)C("xfer_entry"));
    /* xs_busy: an auto-read in progress is waited out. */
    uint64_t vb = (now / MC_FRAME) * MC_FRAME + VBLANK_LINE * MC_LINE;
    if (vb > now) vb -= MC_FRAME;
    if (now >= vb + AR_START && now < vb + AR_START + AR_LEN) charge(vb + AR_START + AR_LEN - now);
    uint64_t start = now;
    uint64_t first = start + (uint64_t)C("xfer_quiet") + 100;
    uint64_t end = start + (uint64_t)C("xfer_quiet") + (uint64_t)C("xfer_byte") * n;
    /* What the Pico shifts out: latched at the restarts before `first`. */
    pico_restage(last_xfer_end + PIO_IRQ, true);
    uint64_t ar = (first / MC_FRAME) * MC_FRAME + VBLANK_LINE * MC_LINE;
    if (ar > first) ar -= MC_FRAME;
    ar += AR_START + AR_LEN + PIO_GAP + PIO_IRQ;
    uint64_t kick = last_kick_end + PIO_GAP + PIO_IRQ;
    bool kicked = last_kick_end > last_xfer_end && kick < first;
    if (ar > last_xfer_end && ar < first && (!kicked || ar < kick)) pico_restage(ar, false);
    if (kicked) pico_restage(kick, false);
    if (ar > last_xfer_end && ar < first && kicked && ar >= kick) pico_restage(ar, false);
    if (latched_sent) ++filled;
    memcpy(rx, latched, LINK_FRAME);
    bool collided = overlaps_autoread(start, end);
    if (collided) {
        ++st.collisions;
        rx[LINK_HDR] ^= 0x55; /* the auto-read's clocks are in the frame */
    }
    watch(&to_snes, latched);
    watch(&to_pico, tx);
    if (!collided) pico_main_turn(tx, end);
    charge(end - now);
    last_xfer_end = end;
    if (in_nmi) ++st.xfer_nmi;
    else ++st.xfer_fg;
    st.t_xfer += now - t0;
    charge(c_frame_ok);
}

/* ------------------------------------------------------------- the NMI */

static unsigned fg_this_frame;
static void run_nmi(void) {
    uint64_t vb = next_nmi, t0 = now;
    in_nmi = true;
    ++st.nmi;
    ++snos_frames;
    if (fg_this_frame < 8) ++st.fg_frames_with[fg_this_frame];
    fg_this_frame = 0;
    charge(c_nmi_entry);
    if (now < vb + AR_START + AR_LEN) now = vb + AR_START + AR_LEN; /* pad_latch's wait */
    charge(c_pad_latch);
    net_service();
    charge(c_nmi_tail);
    in_nmi = false;
    next_nmi = vb + MC_FRAME;
    while (now >= next_nmi) {
        ++st.nmi_span; /* the handler ran into the next vblank */
        next_nmi += MC_FRAME;
    }
    st.t_nmi += now - t0;
}

/* --------------------------------------------------------- the foreground */

/* input.c's fg_poll_room: the bench always has the headroom. */
static int fg_poll_fits(void) {
    if (!link_serial_fg_fits) return 0; /* LINK_HW: "only ever fits from the NMI" */
    return link_serial_fg_fits((uint16_t)line_now());
}
static bool raw_console_feed(void);
void kbd_idle(void) {
    uint16_t f = snos_frames;
    uint64_t t0 = now;
    charge((uint64_t)C("idle_entry"));
    if (net_fg_wanted()) {
        while (snos_frames == f) {
            if (net_fg_wanted() && fg_poll_fits()) {
                ++fg_this_frame;
                net_service_fg();
            } else {
                charge(c_spin);
            }
        }
    } else {
        while (snos_frames == f) charge(next_nmi > now ? next_nmi - now : c_spin);
    }
    charge((uint64_t)C("after_spin"));
    st.t_idle += now - t0;
    uint64_t t1 = now;
    if (raw_mode) raw_console_feed();
    else ip_service();
    /* The pass's own C -- resolver, pending connects, forward retries, the
     * timers and the per-connection poll loop -- is mostly after the
     * receive loop, so it is charged after. */
    charge((uint64_t)C("ips_fixed"));
    st.t_ips += now - t1;
}
void bench_kbd_idle(void) { kbd_idle(); }

static char app_args[64], transcript[8192];
static size_t transcript_len;
const char *bench_app_args(void) { return app_args; }
static struct snap {
    uint64_t t, nmi, xfer_nmi, xfer_fg;
    dir_t up, down;
    uint32_t pico_resends;
    uint16_t rx_frames, resent;
} marks[8];
static unsigned nmarks;
static void take_mark(void) {
    if (nmarks == 8) return;
    struct snap *m = &marks[nmarks++];
    m->t = now; m->nmi = st.nmi; m->xfer_nmi = st.xfer_nmi; m->xfer_fg = st.xfer_fg;
    m->up = to_pico; m->down = to_snes;
    m->pico_resends = link_stats().resends;
    m->rx_frames = net_stat_rx_frames; m->resent = net_stat_resent;
}
void bench_app_output(const char *s) {
    size_t n = strlen(s);
    assert(transcript_len + n < sizeof transcript);
    memcpy(transcript + transcript_len, s, n + 1);
    transcript_len += n;
    if (!strncmp(s, "frames ", 7)) take_mark();
}
void bench_charge_syscall(unsigned which, unsigned bytes) {
    uint64_t t0 = now;
    uint64_t c = (uint64_t)(C("sys_fixed") + C("app_loop"));
    if (which == 8) {
        c += (uint64_t)C("sys_write_byte") * bytes;
        tl("write");
    }
    if (which == 7) c += (uint64_t)C("sys_read_fixed");
    charge(c);
    st.t_app += now - t0;
}

/* Raw mode: no TCP, both ends keep the IP stream full. */
static uint64_t raw_rx_snes, raw_tx_snes;
static bool raw_console_feed(void) {
    static uint8_t buf[1500];
    int n;
    while ((n = net_ip_read(buf, sizeof buf)) > 0) raw_rx_snes += (uint64_t)n;
    while (net_ip_wspace() >= 1462) {
        uint8_t pre[2] = {0xb4, 0x05};
        if (net_ip_write(pre, 2) != 2) break;
        buf[0] = 0x45;
        if (net_ip_write(buf, 1460) != 1460) break;
        raw_tx_snes += 1462;
    }
    return true;
}

/* -------------------------------------------------------------- scenarios */

static void boot(void) {
    regs[0x4200] = 0x81;
    nat_ops_t ops = {emit, no_connect, no_write, no_slot, no_slot, no_udp, no_fwd, no_fdel,
                     no_recved, NULL};
    nat_core_init(core, &ops);
    nat_core_set_adapter(core, emit_try, link_busy, NULL, NULL);
    nat_core_set_local_address(core, pico_addr());
    link_init();
    ctrl_address(pico_ip);
    uint8_t tx[LINK_FRAME];
    link_next_out(tx);
    memcpy(latched, tx, LINK_FRAME);
    net_init();
    ip_init();
    /* The shell, idle, until HELLO arms IP mode. */
    while (!ip_up()) {
        kbd_idle();
        assert(snos_frames < 1200);
    }
}

static void report_phase(const char *label, const struct snap *a, const struct snap *b,
                         double seconds) {
    double ex = (double)(b->xfer_nmi + b->xfer_fg - a->xfer_nmi - a->xfer_fg);
    double frames = (double)(b->nmi - a->nmi);
    printf("  %-9s %5.2f exch/frame (%4.2f fg) | down fresh %5.1f/s rep %5.1f/s, %5.2f exch/fresh"
           " | up fresh %5.1f/s rep %5.1f/s, %5.2f exch/fresh | pico resends %4.1f/s\n",
           label, ex / frames, (double)(b->xfer_fg - a->xfer_fg) / frames,
           (b->down.fresh - a->down.fresh) / seconds, (b->down.repeat - a->down.repeat) / seconds,
           (b->down.fresh - a->down.fresh) ? ex / (double)(b->down.fresh - a->down.fresh) : 0.0,
           (b->up.fresh - a->up.fresh) / seconds, (b->up.repeat - a->up.repeat) / seconds,
           (b->up.fresh - a->up.fresh) ? ex / (double)(b->up.fresh - a->up.fresh) : 0.0,
           (b->pico_resends - a->pico_resends) / seconds);
}

static void tune(const char *spec) {
    /* window=N, exch=N: the kernel's own tunables, when it has them. */
    int v;
    if (sscanf(spec, "window=%d", &v) == 1) {
        if (!net_tune_set) { fprintf(stderr, "kernel has no tunables\n"); exit(2); }
        net_tune_set(0, v);
    } else if (sscanf(spec, "exch=%d", &v) == 1) {
        if (!net_tune_set) { fprintf(stderr, "kernel has no tunables\n"); exit(2); }
        net_tune_set(1, v);
    }
}

int main(int argc, char **argv) {
    unsigned seconds = 10;
    const char *mode = "tcp";
    char *tunes[8];
    unsigned ntunes = 0;
    for (int i = 1; i < argc; ++i) {
        char name[32];
        double v;
        if (sscanf(argv[i], "%31[^=]=%lf", name, &v) == 2) {
            bool found = false;
            for (size_t k = 0; k < NCOST; ++k)
                if (!strcmp(cost[k].name, name)) { cost[k].v = v; found = true; }
            if (!strcmp(name, "seconds")) { seconds = (unsigned)v; found = true; }
            if (!strcmp(name, "fifo")) { staging_fifo = v != 0; found = true; }
            if (!strcmp(name, "window") || !strcmp(name, "exch")) {
                assert(ntunes < 8);
                tunes[ntunes++] = argv[i];
                found = true;
            }
            if (!found) { fprintf(stderr, "unknown %s\n", name); return 2; }
        } else if (!strcmp(argv[i], "raw") || !strcmp(argv[i], "tcp")) {
            mode = argv[i];
        } else {
            fprintf(stderr, "usage: [tcp|raw] [seconds=N] [fifo=0|1] [window=N] [exch=N] "
                            "[cost=value...]\n");
            return 2;
        }
    }
    c_move_fixed = (uint64_t)C("move_fixed"); c_move_byte = (uint64_t)C("move_byte");
    c_checksum_byte = (uint64_t)C("checksum_byte");
    c_uip_in_fixed = (uint64_t)C("uip_in_fixed"); c_uip_in_byte = (uint64_t)C("uip_in_byte");
    c_uip_out_fixed = (uint64_t)C("uip_out_fixed"); c_uip_out_byte = (uint64_t)C("uip_out_byte");
    c_spin = (uint64_t)C("spin");
    c_nmi_entry = (uint64_t)C("nmi_entry"); c_pad_latch = (uint64_t)C("pad_latch");
    c_nmi_tail = (uint64_t)C("nmi_tail"); c_svc_fixed = (uint64_t)C("svc_fixed");
    c_recv_copy = (uint64_t)C("recv_copy"); c_frame_ok = (uint64_t)C("frame_ok");

    raw_mode = !strcmp(mode, "raw");
    boot();
    for (unsigned i = 0; i < ntunes; ++i) tune(tunes[i]);
    if (raw_mode) {
        take_mark();
        uint64_t rs = raw_rx_snes, rp = raw_rx_pico;
        uint16_t f0 = snos_frames;
        while ((uint16_t)(snos_frames - f0) < seconds * 60u) kbd_idle();
        take_mark();
        double secs = (double)(marks[1].t - marks[0].t) / (MC_PER_US * 1e6);
        printf("raw IP stream: down %.0f B/s, up %.0f B/s (both directions saturated)\n",
               (raw_rx_snes - rs) / secs, (raw_rx_pico - rp) / secs);
        report_phase("raw", &marks[0], &marks[1], secs);
    } else {
        snprintf(app_args, sizeof app_args, "%s %u", net_text, seconds);
        int rc = bench_run_speedtest();
        printf("%s", transcript);
        if (rc) { printf("speedtest failed (%d)\n", rc); return 1; }
        static const char *labels[] = {"download", "upload", "both"};
        for (unsigned i = 0; i + 1 < nmarks && i < 3; ++i)
            report_phase(labels[i], &marks[i], &marks[i + 1],
                         (double)(marks[i + 1].t - marks[i].t) / (MC_PER_US * 1e6));
    }
    double total = (double)now;
    printf("  cpu: nmi %.0f%% (exchanges %.0f%% of all time) ip_service %.0f%% app %.0f%% spin+idle %.0f%%\n",
           100 * st.t_nmi / total, 100 * st.t_xfer / total, 100 * st.t_ips / total,
           100 * st.t_app / total, 100 * st.t_idle / total);
    printf("  hazards: nmi late %llu (max %.0f us), masked across vblank %llu, nmi spans %llu, "
           "autoread collisions %llu, kicks %llu, refused %u, bad sums %u, ip drop %u desync %u\n"
           "  pico staging %s: %llu repeats of a frame already sent, %llu data frames replaced unsent\n",
           (unsigned long long)st.nmi_late, st.nmi_late_max / MC_PER_US,
           (unsigned long long)st.masked_vblank, (unsigned long long)st.nmi_span,
           (unsigned long long)st.collisions, (unsigned long long)st.kicks, net_stat_refused,
           net_stat_bad_sum, ip_stat_drop, ip_stat_desync, staging_fifo ? "fifo" : "latest",
           (unsigned long long)filled, (unsigned long long)skipped);
    printf("  fg exchanges per frame:");
    for (unsigned i = 0; i < 8; ++i)
        if (st.fg_frames_with[i]) printf(" %u:%llu", i, (unsigned long long)st.fg_frames_with[i]);
    printf("\n");
    return 0;
}
