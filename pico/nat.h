#ifndef SNOS_PICO_NAT_H
#define SNOS_PICO_NAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef void (*ip_stream_consumer_t)(const uint8_t *packet, size_t len);
#include "nat_core.h"
void nat_init(void);
void nat_reset(void);
void nat_configure(bool wifi_up);
void nat_ip_bytes(const uint8_t *data, size_t len);
void ip_stream_bytes(const uint8_t *data, size_t len, ip_stream_consumer_t consume);
void nat_poll(void);
bool nat_forward_add(uint16_t port, uint8_t channel);
void nat_forward_del(uint16_t port, uint8_t channel);
nat_stats_t nat_stats(void);

#endif
