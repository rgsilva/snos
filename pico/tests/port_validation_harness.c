/* The production port queue/restart code, with only SDK operations mocked.
 * Burst input is completed RX bytes, not a GPIO/PIO timing simulation. */
#include <assert.h>
#include <stdio.h>
#include "../port.c"
static void enqueue_byte(uint8_t value) {
    uint8_t f[LINK_FRAME]; memset(f, value, sizeof f); port_set_tx(f);
}
static uint8_t staged(void) {
#ifdef DATA1_OPEN_DRAIN
    return (uint8_t)~tx[0];
#else
    return tx[0];
#endif
}
static void burst(unsigned bytes) {
    assert(bytes <= LINK_FRAME);
    memset(rx, 0x5a, sizeof rx);
    channels[rx_dma].transfer_count = LINK_FRAME - bytes;
    gap_pending = true;
    service();
    assert(channels[rx_dma].transfer_count == LINK_FRAME);
}
int main(void) {
    port_init(); port_set_idle(false);
    enqueue_byte(1); burst(2); assert(staged() == 1);
    enqueue_byte(2);
    /* Kicks and autoreads cannot replace a staged frame that has not gone
       out whole; zero-byte and truncated bursts have the same property. */
    for (unsigned n = 0; n < LINK_FRAME; ++n) {
        burst(n); assert(staged() == 1 && q_count == 1);
    }
    burst(LINK_FRAME); assert(staged() == 2 && !q_count);
    burst(1); assert(staged() == 2);
    burst(LINK_FRAME); assert(staged() == 2 && tx_sent);
    enqueue_byte(3); burst(1); assert(staged() == 3 && !tx_sent);
    /* A main-loop stall leaves IRQs running: repeat the staged bytes, keep
       four RX frames, count overflow, then allow a catch-up reply burst.
       Both 5 ms and 50 ms fit inside this conservative 10-exchange case. */
    for (unsigned i = 0; i < 10; ++i) burst(LINK_FRAME);
    assert(head - tail == 4 && stats.overruns == 8);
    enqueue_byte(4); enqueue_byte(5); enqueue_byte(6);
    assert(q_count == 2); /* real queue drops oldest, unlike bench's 64 */
    burst(1); assert(staged() == 5 && q_count == 1);
    burst(2); assert(staged() == 5);
    burst(LINK_FRAME); assert(staged() == 6 && q_count == 0);
    uint8_t f[LINK_FRAME]; unsigned read = 0;
    while (port_take_rx(f)) { assert(f[0] == 0x5a); ++read; }
    assert(read == 4);
    /* Idle suppresses output, but queued replies survive until active. */
    port_set_idle(true); enqueue_byte(7); burst(1);
    assert(tx[0] == IDLE_BYTE);
    port_set_idle(false); burst(1); assert(staged() == 7);
    puts("port: real queue/restart passed short bursts, full frames, stalled main, overflow, idle");
}
