#ifndef SNOS_PICO_RELAY_H
#define SNOS_PICO_RELAY_H
#include "link.h"
void relay_init(void);
void relay_poll(void);
void relay_configure(const char *hostname, bool wifi_up);
void relay_reset(void);
bool relay_send(uint8_t type, const uint8_t *data, size_t len);
void relay_ip_bytes(const uint8_t *data, size_t len);
const char *relay_state(void);
#endif
