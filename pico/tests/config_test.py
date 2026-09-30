"""Host checks for flash configuration using SDK stubs."""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/config_test"
for directory in (BUILD / "hardware", BUILD / "pico"):
    directory.mkdir(parents=True, exist_ok=True)
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
(BUILD / "hardware/sync.h").write_text('''
#include <stdint.h>
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t state);
''')
(BUILD / "pico/stdlib.h").write_text('#define __no_inline_not_in_flash_func(name) name\n')
(BUILD / "test.c").write_text('''
#include <assert.h>
#include <string.h>
#include "config.h"
#include "hardware/flash.h"
uint8_t flash_memory[2 * 1024 * 1024];
static unsigned disabled, erases, programs;
uint32_t save_and_disable_interrupts(void) { assert(!disabled); disabled = 1; return 7; }
void restore_interrupts(uint32_t state) { assert(disabled && state == 7); disabled = 0; }
void flash_range_erase(uint32_t offset, size_t size) {
    assert(disabled && offset == sizeof(flash_memory) - 4096 && size == 4096);
    memset(flash_memory + offset, 255, size); ++erases;
}
void flash_range_program(uint32_t offset, const uint8_t *data, size_t size) {
    assert(disabled && offset == sizeof(flash_memory) - 4096 && size == 256);
    memcpy(flash_memory + offset, data, size); ++programs;
}
int main(void) {
    config_t value = {0}, loaded, defaults;
    memset(flash_memory, 255, sizeof(flash_memory));
    assert(!config_load(&defaults));
    strcpy(value.ssid, "SSID with spaces"); strcpy(value.pass, "secret");
    strcpy(value.host, "relay.local");
    assert(config_save(&value) && !disabled && erases == 1 && programs == 1);
    assert(config_load(&loaded));
    assert(!strcmp(loaded.ssid, value.ssid) && !strcmp(loaded.pass, value.pass));
    assert(!strcmp(loaded.host, value.host));
    flash_memory[sizeof(flash_memory) - 4096 + 4] ^= 1;
    assert(!config_load(&loaded) && !strcmp(loaded.ssid, defaults.ssid) && !strcmp(loaded.pass, defaults.pass) && !strcmp(loaded.host, defaults.host));
    assert(config_save(&value));
    flash_memory[sizeof(flash_memory) - 4096] ^= 1;
    assert(!config_load(&loaded));
    memset(value.ssid, 's', sizeof(value.ssid));
    memset(value.pass, 'p', sizeof(value.pass));
    memset(value.host, 'h', sizeof(value.host));
    assert(config_save(&value) && config_load(&loaded));
    assert(strlen(loaded.ssid) == 32 && strlen(loaded.pass) == 64 && strlen(loaded.host) == 63);
    assert(config_clear() && !disabled && !config_load(&loaded));
    assert(!strcmp(loaded.ssid, defaults.ssid) && !strcmp(loaded.pass, defaults.pass) && !strcmp(loaded.host, defaults.host));
    for (size_t i = 0; i < sizeof(flash_memory) - 4096; ++i) assert(flash_memory[i] == 255);
    return 0;
}
''')
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(BUILD), "-I", str(ROOT), str(ROOT / "config.c"),
                str(BUILD / "test.c"), "-o", str(BUILD / "test")], check=True)
subprocess.run([str(BUILD / "test")], check=True)
print("config: persistence, magic/version, bounds, erase, interrupt discipline passed")
