/* Run the actual console frame receiver and IP ring on the host. Only bank
 * qualifiers, mailbox indices, the wire and assembly memcpy are substituted. */
#include <string.h>
#define __far
#define __huge
#ifndef TEST_EMULATED_LINK
#define LINK_HW 1
#endif
#define SNES_MAILBOX_H
#define MB_IN_RING_SIZE 512u
#define MB_IN_WINDEX 0
#define MB_IN_RINDEX 0
#define link_init console_link_init
#define link_reset console_link_reset
#define link_present console_link_present
#define link_recv console_link_recv
#define link_send console_link_send
#ifndef CONSOLE_NET_SOURCE
#error "Compile with console_main_fixture.prepare: console must be main 99b6002"
#endif
#include CONSOLE_NET_SOURCE

volatile uint16_t snos_frames;
static uint8_t incoming[LINK_FRAME], outgoing[LINK_FRAME];
static unsigned keys_received;
static uint8_t key_values[4096];
int console_link_present(void) { return 1; }
int console_link_recv(uint8_t *rx) { memcpy(rx, incoming, LINK_FRAME); return 1; }
void console_link_send(const uint8_t *tx) { memcpy(outgoing, tx, LINK_FRAME); }
void ring_move(uint8_t *dst, const uint8_t *src, uint16_t n) { memcpy(dst, src, n); }
int kbd_push(int c) {
    if (keys_received < sizeof(key_values)) key_values[keys_received] = (uint8_t)c;
    ++keys_received;
    return 0;
}
unsigned console_peer_keys(void) { return keys_received; }
void console_peer_init(void) { session_init(); net_ready = 1; keys_received = 0; }
void console_peer_receive(const uint8_t *f, uint8_t *reply) {
    memcpy(incoming, f, LINK_FRAME); net_service(); memcpy(reply, outgoing, LINK_FRAME);
}
int console_peer_ip_read(uint8_t *p, int n) { return net_ip_read(p, n); }
uint8_t net_checksum(const uint8_t *p, uint16_t n) {
    uint8_t sum = 0;
    for (uint16_t i = 0; i < n; ++i) if (i != LINK_SUM) sum += p[i];
    return sum;
}
