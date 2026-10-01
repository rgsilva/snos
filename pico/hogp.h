#ifndef SNOS_PICO_HOGP_H
#define SNOS_PICO_HOGP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Data extracted from one advertising or scan-response payload. */
typedef struct {
    bool hid_service;
    bool keyboard_appearance;
    bool complete_name;
    char name[64];
} hogp_advertisement_t;

/* Enough of a keyboard input descriptor to make a boot-style report. */
typedef struct {
    bool valid;
    uint8_t report_id;
    uint16_t modifiers_offset;
    uint16_t keys_offset;
    uint8_t key_count;
} hogp_keyboard_map_t;

void hogp_advertisement_parse(const uint8_t *data, size_t len, hogp_advertisement_t *out);
bool hogp_keyboard_map_parse(const uint8_t *data, size_t len, hogp_keyboard_map_t *out);
bool hogp_keyboard_report_to_boot(const hogp_keyboard_map_t *map, uint8_t report_id,
                                  const uint8_t *report, size_t len, uint8_t boot[8]);

#endif
