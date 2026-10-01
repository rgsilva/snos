/* Actual relay callbacks and SDK PCBs; no substitute stream assembler. */
#include <assert.h>
#include "pico/stdlib.h"
#include "../relay.c"

void final_relay_connect(void) {
    /* The transport handshake is outside these stream-lifetime tests. */
    assert(!pcb);
    pcb = tcp_new();
    assert(pcb);
    pcb->state = ESTABLISHED;
    tcp_recv(pcb, recv_cb);
    tcp_err(pcb, error_cb);
    assert(connected_cb(NULL, pcb, ERR_OK) == ERR_OK);
}
void final_relay_receive(uint8_t type, const uint8_t *data, size_t len) {
    uint8_t message[LINK_MTU + 3];
    assert(pcb && len <= LINK_MTU);
    message[0] = type; message[1] = len; message[2] = len >> 8;
    memcpy(message + 3, data, len);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len + 3, PBUF_RAM);
    assert(p && pbuf_take(p, message, len + 3) == ERR_OK);
    assert(recv_cb(NULL, pcb, p, ERR_OK) == ERR_OK);
    relay_poll();
}
void final_relay_eof(void) {
    assert(pcb && recv_cb(NULL, pcb, NULL, ERR_OK) == ERR_OK);
    relay_poll();
    assert(!pcb && !connected);
}
size_t final_relay_output(uint8_t *p, size_t capacity) {
    assert(out_len <= capacity);
    memcpy(p, output, out_len);
    return out_len;
}
