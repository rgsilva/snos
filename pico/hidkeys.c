#include "hidkeys.h"
#include <stdbool.h>
#include <stdio.h>
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
#define USAGE_LCTRL  0xe0u
#define USAGE_RGUI   0xe7u

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

/* ------------------------------------------------------------- key names */

static const struct { uint8_t usage; const char *name; } names[] = {
    {0x28, "enter"}, {0x29, "esc"}, {0x2a, "backspace"}, {0x2b, "tab"},
    {0x2c, "space"}, {0x2d, "-"}, {0x2e, "="}, {0x2f, "["}, {0x30, "]"},
    {0x31, "\\"}, {0x33, ";"}, {0x34, "'"}, {0x35, "`"}, {0x36, ","},
    {0x37, "."}, {0x38, "/"}, {0x39, "capslock"},
    {0x49, "insert"}, {0x4a, "home"}, {0x4b, "pageup"}, {0x4c, "delete"},
    {0x4d, "end"}, {0x4e, "pagedown"},
    {0x4f, "right"}, {0x50, "left"}, {0x51, "down"}, {0x52, "up"},
    {0xe0, "lctrl"}, {0xe1, "lshift"}, {0xe2, "lalt"}, {0xe3, "lgui"},
    {0xe4, "rctrl"}, {0xe5, "rshift"}, {0xe6, "ralt"}, {0xe7, "rgui"},
};

static char lower(char c) { return (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); }

static bool same_name(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if (lower(*a) != lower(*b)) return false;
    return *a == *b;
}

void hidkeys_usage_name(uint8_t usage, char name[HIDKEYS_NAME_MAX]) {
    if (usage >= 4 && usage <= 29) { name[0] = (char)('a' + usage - 4); name[1] = '\0'; return; }
    if (usage >= 30 && usage <= 39) { name[0] = "1234567890"[usage - 30]; name[1] = '\0'; return; }
    if (usage >= USAGE_F1 && usage <= USAGE_F12) {
        snprintf(name, HIDKEYS_NAME_MAX, "f%u", (unsigned)(usage - USAGE_F1 + 1));
        return;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (names[i].usage == usage) { snprintf(name, HIDKEYS_NAME_MAX, "%s", names[i].name); return; }
    snprintf(name, HIDKEYS_NAME_MAX, "0x%02x", (unsigned)usage);
}

uint8_t hidkeys_usage_parse(const char *name) {
    char c = lower(name[0]);
    if (c && !name[1]) {
        if (c >= 'a' && c <= 'z') return (uint8_t)(4 + c - 'a');
        if (c >= '1' && c <= '9') return (uint8_t)(30 + c - '1');
        if (c == '0') return 39;
    }
    if (c == 'f' && name[1] >= '1' && name[1] <= '9') {
        unsigned n = (unsigned)(name[1] - '0');
        if (name[2] >= '0' && name[2] <= '9' && !name[3]) n = n * 10 + (unsigned)(name[2] - '0');
        else if (name[2]) n = 0;
        if (n >= 1 && n <= 12) return (uint8_t)(USAGE_F1 + n - 1);
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (same_name(name, names[i].name)) return names[i].usage;
    if (c == '0' && lower(name[1]) == 'x' && name[2]) {
        unsigned v = 0;
        for (const char *p = name + 2; *p; ++p) {
            char d = lower(*p);
            if (d >= '0' && d <= '9') v = v * 16 + (unsigned)(d - '0');
            else if (d >= 'a' && d <= 'f') v = v * 16 + (unsigned)(d - 'a' + 10);
            else return 0;
            if (v > 0xff) return 0;
        }
        /* 1-3 are the report's error codes, never a key. */
        return v >= 4 ? (uint8_t)v : 0;
    }
    return 0;
}

/* ------------------------------------------------- the keyboard as a pad */

static const char *const buttons[HIDPAD_BUTTONS] = {
    "B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R"
};
/* Arrows the d-pad; Z X for B A, the bottom row, as an emulator would have
 * them; A S above them for Y X; Q W for the shoulders; Enter START, and
 * Right Shift SELECT, the one modifier no typist holds for long. */
static const uint8_t defaults[HIDPAD_BUTTONS] = {
    0x1d, 0x04, 0xe5, 0x28, 0x52, 0x51, 0x50, 0x4f, 0x1b, 0x16, 0x14, 0x1a
};

uint8_t hidpad_default(unsigned button) {
    return button < HIDPAD_BUTTONS ? defaults[button] : 0;
}

void hidpad_init(hidpad_t *pad) {
    memcpy(pad->map, defaults, sizeof(pad->map));
    pad->sent = 0;
    pad->resend = true;
}

void hidpad_set_map(hidpad_t *pad, const uint8_t map[HIDPAD_BUTTONS]) {
    for (unsigned i = 0; i < HIDPAD_BUTTONS; ++i) pad->map[i] = map[i] ? map[i] : defaults[i];
}

uint16_t hidpad_mask(const hidpad_t *pad, const hidkeys_t *keys) {
    uint16_t mask = 0;
    for (unsigned i = 0; i < HIDPAD_BUTTONS; ++i) {
        uint8_t usage = pad->map[i];
        bool held = usage >= USAGE_LCTRL && usage <= USAGE_RGUI
                        ? (keys->modifiers >> (usage - USAGE_LCTRL)) & 1u
                        : usage && key_present(keys->keys, usage);
        if (held) mask |= (uint16_t)(0x8000u >> i);
    }
    return mask;
}

size_t hidpad_due(const hidpad_t *pad, const hidkeys_t *keys, uint8_t out[2]) {
    uint16_t mask = hidpad_mask(pad, keys);
    if (mask == pad->sent && !pad->resend) return 0;
    out[0] = (uint8_t)mask;
    out[1] = (uint8_t)(mask >> 8);
    return 2;
}

void hidpad_sent(hidpad_t *pad, const uint8_t out[2]) {
    pad->sent = (uint16_t)(out[0] | (out[1] << 8));
    pad->resend = false;
}

void hidpad_restart(hidpad_t *pad) { pad->resend = true; }

const char *hidpad_button_name(unsigned button) {
    return button < HIDPAD_BUTTONS ? buttons[button] : NULL;
}

int hidpad_button_parse(const char *name) {
    for (unsigned i = 0; i < HIDPAD_BUTTONS; ++i) if (same_name(name, buttons[i])) return (int)i;
    return -1;
}

size_t hidpad_map_text(const hidpad_t *pad, char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return 0;
    out[0] = '\0';
    for (unsigned i = 0; i < HIDPAD_BUTTONS; ++i) {
        char key[HIDKEYS_NAME_MAX];
        hidkeys_usage_name(pad->map[i], key);
        int w = snprintf(out + n, cap - n, "%s%s=%s", i ? " " : "", buttons[i], key);
        if (w < 0 || (size_t)w >= cap - n) { out[n] = '\0'; break; }
        n += (size_t)w;
    }
    return n;
}
