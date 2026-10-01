#include "link.h"
#include <string.h>
#ifdef PICO_ON_DEVICE
#include "pico/critical_section.h"
#endif
#define QUEUE_COUNT 32
#define CHUNK_SIZE (LINK_MTU + 2)
typedef struct { uint8_t ch; size_t pos, len; uint8_t data[CHUNK_SIZE]; } chunk_t;
static chunk_t pool[QUEUE_COUNT];
static uint8_t order[QUEUE_COUNT];
static size_t count;
static bool syn, peer_syn_seen, rx_resync, started;
/* The send window (docs/link-throughput.md). Frames base..tx_seq-1 are in
 * flight, kept by sequence; cursor is the next of them to put on the wire
 * again, or tx_seq when none is owed. A window of 1 is stop-and-wait, frame
 * for frame what this file sent before windows existed, and it is what a
 * console gets until it asks for more (C2S_LINKWIN, ctrl.c) -- an older
 * console never asks, and every console restart returns it to 1. */
#define WINDOW_MAX 8
static uint8_t base, tx_seq, cursor, window, rx_next, frames[WINDOW_MAX][LINK_FRAME];
static uint32_t resends, bad_sum;
/* The IP stream crosses mode endpoints, but it belongs to this peer lifetime.
 * Keep the hook optional so LINK's standalone tests do not need NAT. */
extern void ip_stream_reset(void) __attribute__((weak));
#ifdef PICO_ON_DEVICE
static critical_section_t queue_lock;
#define LOCK() critical_section_enter_blocking(&queue_lock)
#define UNLOCK() critical_section_exit(&queue_lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif
static uint8_t checksum(const uint8_t *f) {
    uint8_t sum = 0;
    for (size_t i = 0; i < LINK_FRAME; ++i) if (i != 3) sum += f[i];
    return sum;
}
void link_init(void) {
#ifdef PICO_ON_DEVICE
    critical_section_init(&queue_lock);
#endif
    LOCK();
    count = 0; peer_syn_seen = started = false;
    syn = rx_resync = true; base = tx_seq = cursor = rx_next = 0; window = 1;
    resends = bad_sum = 0;
    UNLOCK();
}
static bool enqueue(uint8_t ch, const uint8_t *data, size_t len, bool hello) {
    size_t prefix = ch == CH_IP ? 2 : 0;
    if (count == QUEUE_COUNT || !len || len + prefix > CHUNK_SIZE) return false;
    unsigned slot;
    for (slot = 0; slot < QUEUE_COUNT; ++slot) {
        size_t i;
        for (i = 0; i < count && order[i] != slot; ++i) {}
        if (i == count) break;
    }
    chunk_t *q = &pool[slot];
    q->ch = ch; q->pos = 0; q->len = len + prefix;
    if (prefix) { q->data[0] = len & 255; q->data[1] = len >> 8; }
    memcpy(q->data + prefix, data, len);
    size_t at = count;
    if (hello) {
        at = count && started && pool[order[0]].ch == CH_CTRL ? 1 : 0;
        if (!at) started = false;
    }
    memmove(order + at + 1, order + at, count - at);
    order[at] = slot; ++count;
    return true;
}
bool link_push(uint8_t ch, const uint8_t *data, size_t len) {
    LOCK(); bool ok = enqueue(ch, data, len, false); UNLOCK(); return ok;
}
bool link_push_hello(const uint8_t *data, size_t len) {
    LOCK(); bool ok = enqueue(CH_CTRL, data, len, true); UNLOCK(); return ok;
}
link_rx_t link_on_rx(const uint8_t *f) {
    link_rx_t result = {0};
    LOCK();
    if (f[3] != checksum(f)) { ++bad_sum; UNLOCK(); return result; }
    result.valid = true;
    if (rx_resync) { rx_resync = false; rx_next = f[0]; }
    /* Cumulative: an ack covers every frame up to it. */
    uint8_t acked = (uint8_t)(f[1] - base + 1), flying = (uint8_t)(tx_seq - base);
    if (acked && acked <= flying) {
        base = (uint8_t)(f[1] + 1); syn = false;
        if ((uint8_t)(cursor - base) > (uint8_t)(tx_seq - base)) cursor = base;
    }
    if (f[2] & F_SYN) {
        if (!peer_syn_seen) {
            /* What was in flight belongs to the old session, and so does
             * the window it asked for. */
            peer_syn_seen = true; rx_next = f[0];
            count = 0; started = false; tx_seq = cursor = base; window = 1;
            result.reset = true;
            if (ip_stream_reset)
                ip_stream_reset();
        }
    } else peer_syn_seen = false;
    if ((f[2] & F_DATA) && f[0] == rx_next) {
        for (size_t o = LINK_HDR; o + 2 <= LINK_FRAME;) {
            uint8_t ch = f[o], n = f[o + 1];
            if (ch == CH_PAD || o + 2 + n > LINK_FRAME) break;
            result.records[result.count++] = (link_record_t){ch, n, f + o + 2};
            o += 2 + n;
        }
        ++rx_next;
    }
    UNLOCK(); return result;
}
/* One frame per exchange. Go back to the oldest unacknowledged frame when
 * the window is full or there is nothing new to say: the console takes
 * frames strictly in order, so after a loss everything behind it must come
 * again, and a repeat it already has costs nothing but the exchange. While
 * our own SYN is unacknowledged the window is 1: the receiver adopts the
 * first SYN frame it sees, so a second in flight could be adopted instead. */
void link_next_out(uint8_t *f) {
    LOCK();
    uint8_t flying = (uint8_t)(tx_seq - base), w = syn ? 1 : window;
    if (cursor == tx_seq && flying && (flying >= w || !count)) cursor = base;
    if (cursor != tx_seq) {
        uint8_t *g = frames[cursor++ % WINDOW_MAX];
        ++resends; g[1] = rx_next - 1; g[3] = checksum(g);
        memcpy(f, g, LINK_FRAME); UNLOCK(); return;
    }
    memset(f, 0, LINK_HDR); f[0] = tx_seq; f[1] = rx_next - 1;
    size_t o = LINK_HDR;
    while (count && o + 2 < LINK_FRAME) {
        chunk_t *q = &pool[order[0]];
        size_t n = q->len - q->pos;
        if (n > LINK_FRAME - o - 2) n = LINK_FRAME - o - 2;
        if (n > 255) n = 255;
        f[o++] = q->ch; f[o++] = n;
        memcpy(f + o, q->data + q->pos, n); o += n; q->pos += n;
        if (q->pos < q->len) started = true;
        else { --count; memmove(order, order + 1, count); started = false; }
        f[2] |= F_DATA;
    }
    memset(f + o, CH_PAD, LINK_FRAME - o);
    if (syn) {
        f[2] |= F_SYN;
    }
    f[3] = checksum(f);
    if (f[2] & F_DATA) { memcpy(frames[tx_seq % WINDOW_MAX], f, LINK_FRAME); cursor = ++tx_seq; }
    UNLOCK();
}
void link_set_window(unsigned n) {
    LOCK(); window = n < 1 ? 1 : n > WINDOW_MAX ? WINDOW_MAX : (uint8_t)n; UNLOCK();
}
size_t link_queued(uint8_t ch) {
    LOCK();
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        chunk_t *q = &pool[order[i]];
        if (q->ch == ch) n += q->len - q->pos;
    }
    UNLOCK(); return n;
}
void link_purge(uint8_t ch) {
    LOCK();
    for (size_t i = 0; i < count;) {
        /* A chunk whose prefix has entered a port-owned frame must finish.
         * Mode changes discard only complete chunks that are still queued. */
        if (pool[order[i]].ch != ch || pool[order[i]].pos != 0) {
            ++i;
            continue;
        }
        --count;
        memmove(order + i, order + i + 1, count - i);
    }
    started = count && pool[order[0]].pos != 0;
    UNLOCK();
}

link_stats_t link_stats(void) {
    LOCK(); link_stats_t s = {base, rx_next - 1, resends, bad_sum, window}; UNLOCK(); return s;
}
