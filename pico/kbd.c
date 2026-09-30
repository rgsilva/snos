#include "kbd.h"
#include "link.h"
#include "pico/time.h"
#include <string.h>
static uint32_t next;
static bool test_mode;
void kbd_init(void) { next = to_ms_since_boot(get_absolute_time()) + 5000; }
void kbd_test(bool on) { test_mode = on; }
bool kbd_test_on(void) { return test_mode; }
/* Text from the USB console, plus a newline: the keyboard until a real one
   is attached. Returns false if the link queue is full. */
bool kbd_type(const char *text) {
    size_t n = strlen(text);
    if (n && !link_push(CH_KBD, (const uint8_t *)text, n)) return false;
    return link_push(CH_KBD, (const uint8_t *)"\n", 1);
}
void kbd_poll(void) {
    if (!test_mode) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - next) >= 0 && link_push(CH_KBD, (const uint8_t *)"abcdef", 6)) next = now + 5000;
}
