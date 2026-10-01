#include "final_validation_relay_peer.c"
void last_relay_fail(unsigned kind) {
    assert(pcb);
    if (kind == 0) {
        /* tcp_abort invokes the installed error callback after freeing the PCB. */
        tcp_abort(pcb);
        assert(failed && !pcb);
    } else if (kind == 1) {
        connected = false;
        connect_at = now_ms() - 15001;
    } else {
        input[0] = 1; input[1] = 0xff; input[2] = 0xff; in_len = 3;
    }
    relay_poll(); assert(!pcb && !connected);
}
