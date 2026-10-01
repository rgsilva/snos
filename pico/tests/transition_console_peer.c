/* Actual legacy or current receiver; only hardware/scheduler boundaries are stubbed. */
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
unsigned console_peer_sequence(void) {
#ifdef TRANSITION_CURRENT
    return tx_next;
#else
    return tx_seq;
#endif
}
#ifdef TRANSITION_CURRENT
int link_is_serial(void) { return 1; }
unsigned console_peer_window_expected(void) { return TEST_CONSOLE_WINDOW; }
void console_peer_window(void) {
    net_tune_set(NET_TUNE_WINDOW, TEST_CONSOLE_WINDOW);
    net_tune_announce();
}
int link_serial_exch(int value) { (void)value; return 1; }
#else
unsigned console_peer_window_expected(void) { return 1; }
void console_peer_window(void) {}
#endif
unsigned console_peer_syn(void) { return syn_out; }
