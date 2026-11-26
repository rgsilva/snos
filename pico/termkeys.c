#include "termkeys.h"

enum { IDLE, ESC, CSI, SS3 };
/* Console key codes, snes/kernel/snos_abi.h. */
enum { K_UP = 0x1C, K_DOWN, K_LEFT, K_RIGHT, K_BS = 0x08, K_ENTER = 0x0A, K_ESC = 0x1B,
       K_DEL = 0x7F, K_F1 = 0x81, K_INS = 0x8D, K_HOME, K_END, K_PGUP, K_PGDN };

/* UTF-8 C3 80..BF (Latin-1 letters) to the base letter; '.' has none. */
static const char latin1[] =
    "AAAAAA.CEEEEIIII.NOOOOO.OUUUUY.."
    "aaaaaa.ceeeeiiii.nooooo.ouuuuy.y";

static void put(termkeys_out_t *o, uint8_t k) { if (o->n < 2) o->keys[o->n++] = k; }

static void plain(termkeys_t *t, uint8_t c, termkeys_out_t *o) {
    bool was_c3 = t->c3;
    t->c3 = false;
    if (c == TERMKEYS_EXIT) { o->leave = true; return; }
    if (c == '\r') { put(o, K_ENTER); t->cr = true; return; }
    if (c == '\n') { if (!t->prev_cr) put(o, K_ENTER); return; }
    if (c == 0x7F) { put(o, K_BS); return; }
    if (c == 0) return;
    if (c < 0x80) { put(o, c); return; }
    if (c == 0xC3) { t->c3 = true; return; }
    if (was_c3 && c < 0xC0 && latin1[c - 0x80] != '.') put(o, (uint8_t)latin1[c - 0x80]);
}

static uint8_t letter_key(uint8_t c) {
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'P': case 'Q': case 'R': case 'S': return (uint8_t)(K_F1 + (c - 'P'));
    }
    return 0;
}

static uint8_t tilde_key(uint16_t n) {
    switch (n) {
    case 1: case 7: return K_HOME;
    case 2: return K_INS;
    case 3: return K_DEL;
    case 4: case 8: return K_END;
    case 5: return K_PGUP;
    case 6: return K_PGDN;
    case 11: case 12: case 13: case 14: return (uint8_t)(K_F1 + (n - 11));
    case 15: return K_F1 + 4;
    case 17: case 18: case 19: case 20: case 21: return (uint8_t)(K_F1 + 5 + (n - 17));
    case 23: case 24: return (uint8_t)(K_F1 + 10 + (n - 23));
    }
    return 0;
}

void termkeys_init(termkeys_t *t, bool after_cr) {
    *t = (termkeys_t){0};
    t->cr = after_cr;
}

void termkeys_feed(termkeys_t *t, uint32_t now_ms, uint8_t c, termkeys_out_t *o) {
    uint8_t k;
    o->n = 0; o->leave = false;
    t->at = now_ms;
    t->prev_cr = t->cr; t->cr = false;
    switch (t->state) {
    case IDLE:
        if (c == 0x1B) { t->state = ESC; t->c3 = false; return; }
        plain(t, c, o);
        return;
    case ESC:
        if (c == '[' || c == 'O') {
            t->state = c == '[' ? CSI : SS3; t->num = 0; t->semis = 0; t->any = 0;
            return;
        }
        put(o, K_ESC);
        if (c == 0x1B) return;
        t->state = IDLE;
        plain(t, c, o);
        return;
    case CSI:
        if (c == 0x1B) { t->state = ESC; return; }
        if (c >= 0x30 && c <= 0x3F) {
            t->any = 1;
            if (c == ';') ++t->semis;
            else if (c >= '0' && c <= '9' && !t->semis) {
                t->num = (uint16_t)(t->num * 10 + (c - '0'));
                if (t->num > 999) t->num = 999;
            }
            return;
        }
        if (c >= 0x20 && c <= 0x2F) { t->any = 1; return; }
        t->state = IDLE;
        if (c >= 0x40 && c <= 0x7E) {
            k = c == '~' ? tilde_key(t->num) : letter_key(c);
            if (k) put(o, k);
        } else plain(t, c, o);
        return;
    default:
        if (c == 0x1B) { t->state = ESC; return; }
        t->state = IDLE;
        if (c >= 0x20 && c <= 0x7E) {
            k = c == 'M' ? K_ENTER : letter_key(c);
            if (k) put(o, k);
        } else plain(t, c, o);
        return;
    }
}

void termkeys_tick(termkeys_t *t, uint32_t now_ms, termkeys_out_t *o) {
    o->n = 0; o->leave = false;
    if (t->state == IDLE || (int32_t)(now_ms - t->at) < (int32_t)TERMKEYS_ESC_MS) return;
    if (t->state == ESC) put(o, K_ESC);
    else if (t->state == SS3) { put(o, K_ESC); put(o, 'O'); }
    else if (!t->any) { put(o, K_ESC); put(o, '['); }
    t->state = IDLE;
}
