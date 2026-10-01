/* Binary pipe driver for the actual net.c, used by the real Python gateway.
 * The included fixture stubs the wire only; no frame protocol is duplicated. */
#include <stdio.h>
#define TEST_EMULATED_LINK
#include "revalidation4_console_peer.c"
int main(void) {
    console_peer_init();
    int command;
    uint8_t frame[LINK_FRAME], reply[LINK_FRAME];
    while ((command = getchar()) != EOF) {
        if (command == 'H') {
            /* The exact two-byte enqueue performed by net_hello(), without
             * the host's unavailable scheduler lock. */
            if (txq_space() < 2) return 3;
            txq_put(C2S_HELLO); txq_put(0);
        } else if (command == 'F') {
            if (fread(frame, 1, LINK_FRAME, stdin) != LINK_FRAME) return 2;
            console_peer_receive(frame, reply);
            if (fwrite(reply, 1, LINK_FRAME, stdout) != LINK_FRAME) return 2;
            fflush(stdout);
        } else return 2;
    }
    return 0;
}
