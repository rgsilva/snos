#ifndef SNOS_PICO_HIDKEYS_H
#define SNOS_PICO_HIDKEYS_H
#include <stdbool.h>
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

/* A key's name for `kbd map` ("z", "enter", "rshift", "f5"; "0x64" for the
 * rest), and back: 0 when the name is no key. Modifiers are usages
 * 0xe0-0xe7, the HID page's own numbering of the modifier byte's bits. */
#define HIDKEYS_NAME_MAX 10
void hidkeys_usage_name(uint8_t usage, char name[HIDKEYS_NAME_MAX]);
uint8_t hidkeys_usage_parse(const char *name);

/* The keyboard as a controller: LINK_CH_KBDPAD carries the buttons whose key
 * is held, in the SYS_PAD layout. map[i] is the usage for the button at bit
 * 15 - i: B Y SELECT START UP DOWN LEFT RIGHT A X L R. */
#define HIDPAD_BUTTONS 12
typedef struct {
    uint8_t map[HIDPAD_BUTTONS];
    uint16_t sent;  /* the mask the console last got */
    bool resend;    /* the link (re)started: send even if unchanged */
} hidpad_t;

void hidpad_init(hidpad_t *pad);
/* Stored entries: 0 keeps that button's default key. */
void hidpad_set_map(hidpad_t *pad, const uint8_t map[HIDPAD_BUTTONS]);
uint8_t hidpad_default(unsigned button);
uint16_t hidpad_mask(const hidpad_t *pad, const hidkeys_t *keys);
/* The record due now, little-endian into out: 2, or 0 when the console
 * already has this mask. Not sent until hidpad_sent says it was queued. */
size_t hidpad_due(const hidpad_t *pad, const hidkeys_t *keys, uint8_t out[2]);
void hidpad_sent(hidpad_t *pad, const uint8_t out[2]);
void hidpad_restart(hidpad_t *pad);
const char *hidpad_button_name(unsigned button);
int hidpad_button_parse(const char *name); /* -1 when no button */
/* "B=z A=x ..." for `kbd map`; returns the length written. */
size_t hidpad_map_text(const hidpad_t *pad, char *out, size_t cap);

#endif
