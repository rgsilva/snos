#include "ctrl.h"
#include "relay.h"
#include "log.h"
#include <stdio.h>
static uint8_t state, type, length, used, payload[255];
static uint8_t address[4];
static bool have_address, hello_pending;
static bool (*forward_add)(uint16_t, uint8_t);
static void (*forward_del)(uint16_t, uint8_t);
void ctrl_set_forwarder(bool (*add)(uint16_t, uint8_t), void (*del)(uint16_t, uint8_t)) {
    forward_add = add; forward_del = del;
}
void ctrl_reset(void) { state = used = 0; hello_pending = have_address = false; }
void ctrl_clear_address(void) { have_address = false; }
void ctrl_announce(void) { hello_pending = true; }
void ctrl_address(const uint8_t *ip) {
    for (unsigned i = 0; i < 4; ++i) address[i] = ip[i];
    have_address = true;
}
void ctrl_poll(void) {
    if (!hello_pending || !have_address) return;
    uint8_t reply[19] = {0x84, 0, 2};
    int n = snprintf((char *)reply + 3, sizeof(reply) - 3, "%u.%u.%u.%u",
                     address[0], address[1], address[2], address[3]);
    reply[1] = n + 1;
    if (link_push_hello(reply, (size_t)n + 3)) hello_pending = false;
}
bool ctrl_error(const uint8_t *p) {
    uint8_t reply[] = {0x83, 2, p[2], 1};
    return link_push(CH_CTRL, reply, sizeof(reply));
}
static void dispatch(void) {
    if (type == 0x04) { hello_pending = true; ctrl_poll(); }
    else if (type == 0x0d && length >= 1) link_set_window(payload[0]); /* C2S_LINKWIN */
    else if ((type == 0x0b || type == 0x0c) && length >= 3) {
        uint8_t p[] = {payload[1], payload[2], payload[0]};
        bool ok;
        uint16_t port = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        if (forward_add) {
            if (type == 0x0b) ok = forward_add(port, p[2]);
            else { forward_del(port, p[2]); ok = true; }
        } else ok = relay_send(type == 0x0b ? 2 : 3, p, sizeof(p));
        if (!ok && !ctrl_error(p))
            log_event(P_CTRL "error queue full\n");
    }
}
void ctrl_bytes(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        if (state == 0) { type = b; state = 1; }
        else if (state == 1) {
            length = b; used = 0; state = b ? 2 : 0;
            if (!b) dispatch();
        } else {
            payload[used++] = b;
            if (used == length) { state = 0; dispatch(); }
        }
    }
}
