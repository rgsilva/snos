#include "hidkeys.h"
#include <stdbool.h>
#include <string.h>

#define MOD_CTRL  0x11u
#define MOD_SHIFT 0x22u
#define MOD_ALT   0x44u
#define ESC       0x1bu

/* snes/kernel/snos_abi.h owns these values; the console is the reader. */
#define SNOS_KEY_UP       0x1cu
#define SNOS_KEY_DOWN     0x1du
#define SNOS_KEY_LEFT     0x1eu
#define SNOS_KEY_RIGHT    0x1fu
#define SNOS_KEY_DELETE   0x7fu
#define SNOS_KEY_F1       0x81u
#define SNOS_KEY_INSERT   0x8du
#define SNOS_KEY_HOME     0x8eu
#define SNOS_KEY_END      0x8fu
#define SNOS_KEY_PAGEUP   0x90u
#define SNOS_KEY_PAGEDOWN 0x91u

/* HID usages, keyboard page */
#define USAGE_F1     0x3au
#define USAGE_F12    0x45u

static bool key_present(const uint8_t keys[6], uint8_t key) {
    for (size_t i = 0; i < 6; ++i) if (keys[i] == key) return true;
    return false;
}

static bool rollover_report(const uint8_t keys[6]) {
    for (size_t i = 0; i < 6; ++i) if (keys[i] == 1) return true;
    return false;
}

static uint8_t translate(uint8_t modifiers, uint8_t key) {
    static const char normal[] = {
        0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
        'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
        's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '1', '2',
        '3', '4', '5', '6', '7', '8', '9', '0', '\n', 0x1b,
        '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';',
        '\'', '`', ',', '.', '/'
    };
    static const char shifted[] = {
        0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R',
        'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@',
        '#', '$', '%', '^', '&', '*', '(', ')', '\n', 0x1b,
        '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':',
        '"', '~', '<', '>', '?'
    };

    if ((modifiers & MOD_CTRL) && key >= 4 && key <= 29) return (uint8_t)(key - 3);
    if (key >= USAGE_F1 && key <= USAGE_F12) return (uint8_t)(SNOS_KEY_F1 + (key - USAGE_F1));
    switch (key) {
        case 0x49: return SNOS_KEY_INSERT;
        case 0x4a: return SNOS_KEY_HOME;
        case 0x4b: return SNOS_KEY_PAGEUP;
        case 0x4c: return SNOS_KEY_DELETE;
        case 0x4d: return SNOS_KEY_END;
        case 0x4e: return SNOS_KEY_PAGEDOWN;
        case 0x4f: return SNOS_KEY_RIGHT;
        case 0x50: return SNOS_KEY_LEFT;
        case 0x51: return SNOS_KEY_DOWN;
        case 0x52: return SNOS_KEY_UP;
        default: break;
    }
    if (key >= sizeof(normal)) return 0;
    return (uint8_t)((modifiers & MOD_SHIFT) ? shifted[key] : normal[key]);
}

/* Escape first under Alt, and never the Escape alone: a pair that does not
 * fit is dropped whole, or the next key would arrive as Alt+key. */
static size_t emit(uint8_t modifiers, uint8_t byte, uint8_t *out, size_t n, size_t cap) {
    size_t need = (modifiers & MOD_ALT) ? 2 : 1;
    if (n + need > cap) return n;
    if (need == 2) out[n++] = ESC;
    out[n++] = byte;
    return n;
}

void hidkeys_init(hidkeys_t *keys) {
    memset(keys, 0, sizeof(*keys));
}

size_t hidkeys_report(hidkeys_t *keys, uint32_t now_ms, const uint8_t report[HIDKEYS_REPORT_LEN],
                      uint8_t *out, size_t out_cap) {
    const uint8_t modifiers = report[0];
    const uint8_t *down = &report[2];
    size_t n = 0;

    if (rollover_report(down)) return 0;

    for (size_t i = 0; i < 6; ++i) {
        uint8_t key = down[i];
        if (!key || key_present(keys->keys, key)) continue;
        uint8_t byte = translate(modifiers, key);
        if (!byte) continue;
        n = emit(modifiers, byte, out, n, out_cap);
        keys->repeat_key = key;
        keys->repeat_modifiers = modifiers;
        keys->repeat_byte = byte;
        keys->repeat_at = now_ms + HIDKEYS_REPEAT_DELAY_MS;
    }

    if (keys->repeat_key && !key_present(down, keys->repeat_key)) {
        keys->repeat_key = 0;
        keys->repeat_byte = 0;
        keys->repeat_at = 0;
    }
    keys->modifiers = modifiers;
    memcpy(keys->keys, down, sizeof(keys->keys));
    return n;
}

size_t hidkeys_repeat(hidkeys_t *keys, uint32_t now_ms, uint8_t *out, size_t out_cap) {
    if (!keys->repeat_key || !keys->repeat_byte || !out_cap) return 0;
    if ((int32_t)(now_ms - keys->repeat_at) < 0) return 0;
    size_t n = emit(keys->repeat_modifiers, keys->repeat_byte, out, 0, out_cap);
    if (n) keys->repeat_at = now_ms + HIDKEYS_REPEAT_MS;
    return n;
}
