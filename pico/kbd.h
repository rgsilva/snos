#ifndef SNOS_PICO_KBD_H
#define SNOS_PICO_KBD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void kbd_init(void);
void kbd_poll(void);
void kbd_test(bool on);      /* "abcdef" every 5 s, the wire test; off by default */
bool kbd_test_on(void);
bool kbd_type(const char *text); /* send text + newline as keystrokes */
bool kbd_key(const uint8_t *keys, size_t n); /* raw key bytes, in order with the rest */
void kbd_hid_boot_report(const uint8_t report[8]);
/* The keyboard went away: every key it held is up, and the console is told. */
void kbd_hid_release(void);
/* The link (re)started, so the console's pad word is stale: send it again. */
void kbd_link_restart(void);
/* `kbd map`: 12 stored usages (config.h), 0 for a button's default key. */
void kbd_set_map(const uint8_t map[12]);
size_t kbd_map_text(char *out, size_t cap);
#endif
