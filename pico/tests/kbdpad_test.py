"""Host checks for the keyboard-as-controller records (kbd.c) and `kbd map`'s
persistence (config.c), against stubbed SDK headers and a recording link."""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/kbdpad_test"
for directory in (BUILD / "hardware", BUILD / "pico"):
    directory.mkdir(parents=True, exist_ok=True)
FLAGS = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(BUILD), "-I", str(ROOT)]

(BUILD / "hardware/sync.h").write_text('''
#include <stdint.h>
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t state);
''')
(BUILD / "pico/time.h").write_text('''
#include <stdint.h>
extern uint32_t test_now;
static inline uint32_t get_absolute_time(void) { return test_now; }
static inline uint32_t to_ms_since_boot(uint32_t t) { return t; }
''')
(BUILD / "kbd_main.c").write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kbd.h"
#include "link.h"
uint32_t test_now;
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t state) { (void)state; }
/* The link queue: what kbd.c pushed, in order; `full` refuses everything. */
static uint8_t log_ch[64], log_data[64][16], log_len[64];
static unsigned logged;
static int full;
bool link_push(uint8_t ch, const uint8_t *data, size_t len) {
    if (full) return false;
    assert(logged < 64 && len <= 16);
    log_ch[logged] = ch; log_len[logged] = (uint8_t)len;
    memcpy(log_data[logged++], data, len);
    return true;
}
/* The next record on a channel, skipping the others; -1 when none. */
static unsigned seen;
static int next_pad(void) {
    for (; seen < logged; ++seen) {
        if (log_ch[seen] != CH_KBDPAD) continue;
        assert(log_len[seen] == 2);
        const uint8_t *d = log_data[seen++];
        return d[0] | (d[1] << 8);
    }
    return -1;
}
static void key(uint8_t modifiers, uint8_t a, uint8_t b) {
    uint8_t r[8] = {modifiers, 0, a, b};
    kbd_hid_boot_report(r);
}
int main(void) {
    kbd_init();
    kbd_poll();
    assert(next_pad() == 0 && next_pad() == -1);  /* once at start, never again */
    kbd_poll();
    assert(next_pad() == -1);

    key(0, 0x1d, 0);                              /* Z: B, and a z */
    kbd_poll();
    assert(logged >= 2 && log_ch[logged - 2] == CH_KBD && log_data[logged - 2][0] == 'z');
    assert(next_pad() == 0x8000 && next_pad() == -1);
    key(0, 0x1d, 0);                              /* still held: nothing */
    kbd_poll();
    assert(next_pad() == -1);
    key(0, 0x1d, 0x52);                           /* + Up */
    key(0, 0x52, 0);                              /* - Z */
    key(0, 0, 0);                                 /* - Up: three in one poll */
    kbd_poll();
    assert(next_pad() == 0x8800 && next_pad() == 0x0800 && next_pad() == 0);
    assert(next_pad() == -1);

    key(0x20, 0x28, 0);                           /* Right Shift + Enter */
    kbd_poll();
    assert(next_pad() == 0x3000);
    kbd_hid_release();                            /* the keyboard went away */
    kbd_poll();
    assert(next_pad() == 0 && next_pad() == -1);
    test_now = 2000;                              /* and no typematic repeat */
    unsigned before = logged;
    kbd_poll();
    assert(logged == before);

    key(0, 0x1b, 0);                              /* X: A */
    kbd_poll();
    assert(next_pad() == 0x0080);
    kbd_link_restart();                           /* the console restarted */
    kbd_poll();
    assert(next_pad() == 0x0080 && next_pad() == -1);

    full = 1;                                     /* the queue refuses ... */
    key(0, 0x1b, 0x1a);                           /* + W: R */
    kbd_poll();
    full = 0;                                     /* ... until it does not */
    kbd_poll();
    assert(next_pad() == 0x0090 && next_pad() == -1);
    key(0, 0, 0);
    kbd_poll();
    assert(next_pad() == 0);

    uint8_t map[12] = {0x05};                     /* B on the b key */
    kbd_set_map(map);
    char text[160];
    kbd_map_text(text, sizeof(text));
    assert(!strncmp(text, "B=b Y=a ", 8));
    key(0, 0x05, 0);
    kbd_poll();
    assert(next_pad() == 0x8000);
    return 0;
}
''')
subprocess.run([*FLAGS, str(ROOT / "kbd.c"), str(ROOT / "hidkeys.c"), str(BUILD / "kbd_main.c"),
                "-o", str(BUILD / "kbd")], check=True)
subprocess.run([str(BUILD / "kbd")], check=True)

(BUILD / "hardware/flash.h").write_text('''
#include <stdint.h>
#include <stddef.h>
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u
extern uint8_t flash_memory[2 * 1024 * 1024];
#define XIP_BASE ((uintptr_t)flash_memory)
void flash_range_erase(uint32_t offset, size_t size);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t size);
''')
(BUILD / "pico/stdlib.h").write_text('#define __no_inline_not_in_flash_func(name) name\n')
(BUILD / "config_main.c").write_text(r'''
#include <assert.h>
#include <string.h>
#include "config.h"
#include "hardware/flash.h"
uint8_t flash_memory[2 * 1024 * 1024];
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t state) { (void)state; }
void flash_range_erase(uint32_t offset, size_t size) { memset(flash_memory + offset, 255, size); }
void flash_range_program(uint32_t offset, const uint8_t *data, size_t size) {
    memcpy(flash_memory + offset, data, size);
}
int main(void) {
    config_t value = {0}, loaded;
    uint8_t *page = flash_memory + sizeof(flash_memory) - 4096;
    memset(flash_memory, 255, sizeof(flash_memory));
    assert(!config_load(&loaded));
    for (int i = 0; i < 12; ++i) assert(loaded.pad_map[i] == 0);  /* defaults */
    /* A version-1 image: the map's bytes were never written, so they are
     * erased flash, and must read as "every default". */
    page[0] = 0x53; page[1] = 0x4f; page[2] = 0x4e; page[3] = 0x53;  /* CONFIG_MAGIC, LE */
    page[4] = 1; page[5] = page[6] = page[7] = 0;
    memcpy(page + 8, "ssid", 5);
    assert(config_load(&loaded) && !strcmp(loaded.ssid, "ssid"));
    for (int i = 0; i < 12; ++i) assert(loaded.pad_map[i] == 0);
    strcpy(value.ssid, "net");
    value.pad_map[0] = 0x05; value.pad_map[11] = 0xe5;
    assert(config_save(&value) && page[4] == 2);
    assert(config_load(&loaded) && !strcmp(loaded.ssid, "net"));
    assert(!memcmp(loaded.pad_map, value.pad_map, 12));
    return 0;
}
''')
subprocess.run([*FLAGS, str(ROOT / "config.c"), str(BUILD / "config_main.c"),
                "-o", str(BUILD / "config")], check=True)
subprocess.run([str(BUILD / "config")], check=True)
print("kbdpad: pad records on start, change, tap, release, restart and full queue; "
      "map persistence passed")
