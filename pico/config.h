#ifndef SNOS_PICO_CONFIG_H
#define SNOS_PICO_CONFIG_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t magic, version;
    char ssid[33], pass[65], host[64];
    uint8_t pad_map[12]; /* `kbd map`: a HID usage per button, 0 = default */
} config_t;
bool config_load(config_t *config);
bool config_save(const config_t *config);
bool config_clear(void);
#endif
