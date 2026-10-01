/* Binary transport for the unmodified legacy/current console fixture.
 * No parser, frame builder or restart logic is duplicated here. */
#include <assert.h>
#include <stdio.h>
#include "transition_console_peer.c"

static unsigned word(void) {
    int lo = getchar(), hi = getchar();
    assert(lo >= 0 && hi >= 0);
    return (unsigned)lo | ((unsigned)hi << 8);
}
static void putword(unsigned n) { putchar(n & 255); putchar((n >> 8) & 255); }
int main(void) {
    uint8_t data[2048], out[LINK_FRAME];
    int op;
    console_peer_init();
    while ((op = getchar()) != EOF) {
        if (op == 'x') {
            assert(fread(data, 1, LINK_FRAME, stdin) == LINK_FRAME);
            console_peer_receive(data, out);
            assert(fwrite(out, 1, LINK_FRAME, stdout) == LINK_FRAME);
        } else if (op == 'h') putword((unsigned)net_hello());
        else if (op == 's') { putword(net_gw_version); putword(net_stat_hello); }
        else if (op == 'i') { console_peer_init(); putchar(0); }
        else if (op == 'w') {
            unsigned n = word();
            assert(n <= sizeof data && fread(data, 1, n, stdin) == n);
            putword((unsigned)console_peer_ip_write(data, (int)n));
        } else if (op == 'r') {
            int n = console_peer_ip_read(data, sizeof data);
            putword((unsigned)n);
            if (n > 0) assert(fwrite(data, 1, n, stdout) == (unsigned)n);
        } else assert(0);
        fflush(stdout);
    }
    return 0;
}
