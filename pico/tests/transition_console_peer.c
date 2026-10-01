/* The actual 99b6002 receiver; only hardware/scheduler boundaries are stubbed. */
#include "revalidation4_console_peer.c"
void proc_lock(void) {}
void proc_unlock(void) {}
int console_peer_control_write(const uint8_t *p, unsigned n) {
    if ((unsigned)txq_space() < n) return 0;
    for (unsigned i = 0; i < n; ++i) txq_put(p[i]);
    return 1;
}
unsigned console_peer_hellos(void) { return net_stat_hello; }
const char *console_peer_address(void) { return net_text; }
unsigned console_peer_sequence(void) { return tx_seq; }
unsigned console_peer_syn(void) { return syn_out; }
