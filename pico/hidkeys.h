#ifndef SNOS_PICO_HIDKEYS_H
#define SNOS_PICO_HIDKEYS_H
#include <stddef.h>
#include <stdint.h>

#define HIDKEYS_REPORT_LEN 8
#define HIDKEYS_REPEAT_DELAY_MS 500u
#define HIDKEYS_REPEAT_MS 33u

typedef struct {
    uint8_t modifiers;
    uint8_t keys[6];
    uint8_t repeat_key;
    uint8_t repeat_modifiers;
    uint8_t repeat_byte;
    uint32_t repeat_at;
} hidkeys_t;

void hidkeys_init(hidkeys_t *keys);
/* Up to two bytes per newly pressed key: Alt sends Escape first. */
size_t hidkeys_report(hidkeys_t *keys, uint32_t now_ms, const uint8_t report[HIDKEYS_REPORT_LEN],
                      uint8_t *out, size_t out_cap);
size_t hidkeys_repeat(hidkeys_t *keys, uint32_t now_ms, uint8_t *out, size_t out_cap);

#endif
