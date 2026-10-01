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
static bool awaiting, syn, peer_syn_seen, rx_resync, started;
static uint8_t tx_seq, rx_next, syn_seq, inflight[LINK_FRAME];
static uint32_t resends, bad_sum;
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
    count = 0; awaiting = peer_syn_seen = started = false;
    syn = rx_resync = true; tx_seq = syn_seq = rx_next = 0; resends = bad_sum = 0;
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
    if (awaiting && f[1] == tx_seq) { awaiting = false; ++tx_seq; syn = false; }
    if (f[2] & F_SYN) {
        if (!peer_syn_seen) {
            peer_syn_seen = true; rx_next = f[0];
            count = 0; started = awaiting = false; result.reset = true;
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
void link_next_out(uint8_t *f) {
    LOCK();
    if (awaiting) {
        ++resends; inflight[1] = rx_next - 1; inflight[3] = checksum(inflight);
        memcpy(f, inflight, LINK_FRAME); UNLOCK(); return;
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
        syn_seq = tx_seq;
    }
    f[3] = checksum(f); awaiting = (f[2] & F_DATA) != 0;
    memcpy(inflight, f, LINK_FRAME);
    UNLOCK();
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
        if (pool[order[i]].ch != ch) {
            ++i;
            continue;
        }
        --count;
        memmove(order + i, order + i + 1, count - i);
    }
    started = count && pool[order[0]].pos != 0;
    UNLOCK();
}

/* Keep keyboard records that shared the abandoned frame.  NAT and control
 * bytes belong to the old adapter lifetime, but keystrokes must survive it. */
static void replay_keyboard(void) {
    uint8_t data[LINK_FRAME - LINK_HDR];
    size_t lengths[(LINK_FRAME - LINK_HDR) / 2], records = 0, used = 0;
    for (size_t o = LINK_HDR; o + 2 <= LINK_FRAME;) {
        uint8_t ch = inflight[o++], len = inflight[o++];
        if (ch == CH_PAD || o + len > LINK_FRAME)
            break;
        if (ch == CH_KBD && records < sizeof(lengths) / sizeof(*lengths)) {
            memcpy(data + used, inflight + o, len);
            lengths[records++] = len;
            used += len;
        }
        o += len;
    }
    for (size_t record = records; record--;) {
        unsigned slot;
        for (slot = 0; slot < QUEUE_COUNT; ++slot) {
            size_t i;
            for (i = 0; i < count && order[i] != slot; ++i) {}
            if (i == count)
                break;
        }
        if (slot == QUEUE_COUNT) {
            /* A full pool means this frame only advanced the first chunk:
             * consuming a whole chunk would already have freed its slot. */
            if (record == 0 && count && pool[order[0]].ch == CH_KBD &&
                pool[order[0]].pos >= lengths[record])
                pool[order[0]].pos -= lengths[record];
            break;
        }
        size_t offset = 0;
        for (size_t i = 0; i < record; ++i)
            offset += lengths[i];
        chunk_t *q = &pool[slot];
        q->ch = CH_KBD;
        q->pos = 0;
        q->len = lengths[record];
        memcpy(q->data, data + offset, q->len);
        memmove(order + 1, order, count);
        order[0] = slot;
        ++count;
    }
}

void link_restart(void) {
    LOCK();
    if (awaiting)
        replay_keyboard();
    /* A delayed ACK for the abandoned frame must not acknowledge fresh data.
     * Do not reuse the prior SYN base either: the console accepts a changed
     * SYN sequence as a distinct restart even if the old SYN was its last
     * received frame. */
    awaiting = false;
    ++tx_seq;
    if (tx_seq == syn_seq)
        ++tx_seq;
    syn = true;
    started = count && pool[order[0]].pos != 0;
    UNLOCK();
}
link_stats_t link_stats(void) {
    LOCK(); link_stats_t s = {tx_seq, rx_next - 1, resends, bad_sum}; UNLOCK(); return s;
}
