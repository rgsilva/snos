#include "kbd.h"
#include "hidkeys.h"
#include "link.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include <string.h>
static uint32_t next;
static bool test_mode;
static hidkeys_t hidkeys;
static hidpad_t hidpad;
static volatile bool released;
static uint8_t reports[8][HIDKEYS_REPORT_LEN];
static uint8_t report_head, report_tail;
void kbd_init(void) {
    next = to_ms_since_boot(get_absolute_time()) + 5000;
    hidkeys_init(&hidkeys);
    hidpad_init(&hidpad);
}
void kbd_set_map(const uint8_t map[HIDPAD_BUTTONS]) { hidpad_set_map(&hidpad, map); }
size_t kbd_map_text(char *out, size_t cap) { return hidpad_map_text(&hidpad, out, cap); }
void kbd_link_restart(void) { hidpad_restart(&hidpad); }
/* A flag, not an all-up report in the ring: the ring may be full, and the
 * keyboard is gone, so nothing newer than what it already holds can come. */
void kbd_hid_release(void) { released = true; }
void kbd_test(bool on) { test_mode = on; }
bool kbd_test_on(void) { return test_mode; }
/* Text from the USB console, plus a newline: the keyboard until a real one
 * is attached. Returns false if the link queue is full. */
bool kbd_type(const char *text) {
    size_t n = strlen(text);
    if (n && !link_push(CH_KBD, (const uint8_t *)text, n)) return false;
    return link_push(CH_KBD, (const uint8_t *)"\n", 1);
}
void kbd_hid_boot_report(const uint8_t report[HIDKEYS_REPORT_LEN]) {
    uint32_t irq = save_and_disable_interrupts();
    uint8_t next_head = (uint8_t)((report_head + 1u) & 7u);
    if (next_head != report_tail) {
        memcpy(reports[report_head], report, HIDKEYS_REPORT_LEN);
        report_head = next_head;
    }
    restore_interrupts(irq);
}
static bool take_report(uint8_t report[HIDKEYS_REPORT_LEN]) {
    uint32_t irq = save_and_disable_interrupts();
    bool have = report_tail != report_head;
    if (have) {
        memcpy(report, reports[report_tail], HIDKEYS_REPORT_LEN);
        report_tail = (uint8_t)((report_tail + 1u) & 7u);
    }
    restore_interrupts(irq);
    return have;
}
/* Per report, not once a poll: a tap whose press and release queue up
 * together would otherwise never reach the console as a press. A full queue
 * leaves it due, and the next poll sends whatever is held by then. */
static void send_pad(void) {
    uint8_t record[2];
    if (hidpad_due(&hidpad, &hidkeys, record) && link_push(CH_KBDPAD, record, sizeof(record)))
        hidpad_sent(&hidpad, record);
}
void kbd_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint8_t report[HIDKEYS_REPORT_LEN], out[12];
    while (take_report(report)) {
        size_t n = hidkeys_report(&hidkeys, now, report, out, sizeof(out));
        if (n && !link_push(CH_KBD, out, n)) break;
        send_pad();
    }
    if (released) {
        static const uint8_t none[HIDKEYS_REPORT_LEN];
        released = false;
        (void)hidkeys_report(&hidkeys, now, none, out, sizeof(out));
    }
    send_pad();
    size_t n = hidkeys_repeat(&hidkeys, now, out, sizeof(out));
    if (n) (void)link_push(CH_KBD, out, n);
    if (test_mode && (int32_t)(now - next) >= 0 && link_push(CH_KBD, (const uint8_t *)"abcdef", 6)) next = now + 5000;
}
