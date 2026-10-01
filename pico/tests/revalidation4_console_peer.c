/* Extend the existing actual-kernel fixture; no receiver implementation copy. */
#include "revalidation3_console_peer.c"
void console_peer_enable_ip(void) { ip_mode = 1; }
int console_peer_ip_write(const uint8_t *p, int n) { return net_ip_write(p, n); }
void console_peer_open_channel(void) { ch_state[1] = NET_ST_OPEN; }
int console_peer_channel_failed(void) { return ch_state[1] == NET_ST_ERROR; }
const uint8_t *console_peer_key_values(void) { return key_values; }
