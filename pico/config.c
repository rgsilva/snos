#include "config.h"
#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASS ""
#define RELAY_HOST ""
#endif
#define CONFIG_MAGIC 0x534e4f53u
#define CONFIG_VERSION 1u
#define CONFIG_OFFSET (2u * 1024u * 1024u - FLASH_SECTOR_SIZE)
static const config_t defaults = {0, 0, WIFI_SSID, WIFI_PASS, RELAY_HOST};
_Static_assert(sizeof(config_t) <= FLASH_PAGE_SIZE, "config exceeds flash page");
static const config_t *stored(void) { return (const config_t *)(XIP_BASE + CONFIG_OFFSET); }
bool config_load(config_t *config) {
    const config_t *p = stored();
    bool valid = p->magic == CONFIG_MAGIC && p->version == CONFIG_VERSION;
    *config = valid ? *p : defaults;
    config->ssid[32] = config->pass[64] = config->host[63] = '\0';
    return valid;
}
/* SDK flash routines execute from RAM; no second core is running. */
static void __no_inline_not_in_flash_func(write_flash)(const uint8_t *page) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(CONFIG_OFFSET, FLASH_SECTOR_SIZE);
    if (page) flash_range_program(CONFIG_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq);
}
bool config_save(const config_t *config) {
    uint8_t page[FLASH_PAGE_SIZE];
    config_t value = *config;
    value.magic = CONFIG_MAGIC; value.version = CONFIG_VERSION;
    value.ssid[32] = value.pass[64] = value.host[63] = '\0';
    memset(page, 0xff, sizeof(page));
    memcpy(page, &value, sizeof(value));
    write_flash(page);
    return memcmp(stored(), page, sizeof(page)) == 0;
}
bool config_clear(void) {
    write_flash(NULL);
    const uint8_t *p = (const uint8_t *)stored();
    for (size_t i = 0; i < FLASH_SECTOR_SIZE; ++i) if (p[i] != 0xff) return false;
    return true;
}
