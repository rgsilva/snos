/* Observe the real relay's transport boundary, without replacing its assembler. */
#include <assert.h>
#include "pico/stdlib.h"
#include "../relay.c"
size_t transition_relay_output(uint8_t *p, size_t capacity) {
    assert(out_len <= capacity);
    memcpy(p, output, out_len);
    return out_len;
}
void transition_relay_address(const uint8_t *address) {
    /* The real relay receives this record from its server. Defer the network
     * connect attempt, which is unrelated to processing its incoming records. */
    retry_at = now_ms() + 10000;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 7, PBUF_RAM);
    uint8_t message[7] = {5, 4, 0};
    assert(p); memcpy(message + 3, address, 4);
    assert(pbuf_take(p, message, sizeof(message)) == ERR_OK);
    /* recv_cb credits its live PCB. This record is already received here. */
    assert(in_len + sizeof(message) <= sizeof(input));
    in_len += pbuf_copy_partial(p, input + in_len, sizeof(message), 0);
    pbuf_free(p); relay_poll();
}
