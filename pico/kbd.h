#ifndef SNOS_PICO_KBD_H
#define SNOS_PICO_KBD_H
#include <stdbool.h>
void kbd_init(void);
void kbd_poll(void);
void kbd_test(bool on);      /* "abcdef" every 5 s, the wire test; off by default */
bool kbd_test_on(void);
bool kbd_type(const char *text); /* send text + newline as keystrokes */
#endif
