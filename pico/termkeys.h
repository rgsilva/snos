#ifndef SNOS_PICO_TERMKEYS_H
#define SNOS_PICO_TERMKEYS_H
#include <stdbool.h>
#include <stdint.h>

/* Ctrl+] (telnet's escape): leaves passthrough, never sent. */
#define TERMKEYS_EXIT 0x1D
/* How long an Escape waits for the rest of a sequence. */
#define TERMKEYS_ESC_MS 50u

typedef struct {
    uint8_t state, prev_cr, cr, c3, any;
    uint16_t num;
    uint8_t semis;
    uint32_t at;
} termkeys_t;

typedef struct {
    uint8_t keys[2];
    uint8_t n;
    bool leave;
} termkeys_out_t;

/* after_cr: the byte that started this was a CR, so a LF right behind it is its pair. */
void termkeys_init(termkeys_t *t, bool after_cr);
/* One terminal byte in; 0..2 console key bytes out, or leave. */
void termkeys_feed(termkeys_t *t, uint32_t now_ms, uint8_t byte, termkeys_out_t *out);
/* Call often: an Escape that nothing followed becomes the key. */
void termkeys_tick(termkeys_t *t, uint32_t now_ms, termkeys_out_t *out);
#endif
