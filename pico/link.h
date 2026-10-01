#ifndef SNOS_PICO_LINK_H
#define SNOS_PICO_LINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LINK_FRAME 64
#define LINK_HDR 4
#define F_DATA 1
#define F_SYN 2
#define CH_CTRL 0
#define CH_IP 0x10
#define CH_KBD 0x11
#define CH_PAD 0xff
#define LINK_MTU 1500
typedef struct { uint8_t ch, len; const uint8_t *data; } link_record_t;
typedef struct {
    link_record_t records[(LINK_FRAME - LINK_HDR) / 2];
    size_t count;
    bool reset;
    bool valid; /* the checksum matched: a peer is there */
} link_rx_t;
typedef struct { uint8_t seq, ack; uint32_t resends, bad_sum; uint8_t window; } link_stats_t;
void link_init(void);
bool link_push(uint8_t ch, const uint8_t *data, size_t len);
bool link_push_hello(const uint8_t *data, size_t len);
link_rx_t link_on_rx(const uint8_t *frame);
void link_next_out(uint8_t *frame);
size_t link_queued(uint8_t ch);
/* Drop unstarted queued output for one channel before a mode transition. */
void link_purge(uint8_t ch);
/* Frames in flight toward the console, 1..8; the console asks (ctrl.c). */
void link_set_window(unsigned n);
link_stats_t link_stats(void);
#endif
