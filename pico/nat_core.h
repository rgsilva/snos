#ifndef SNOS_PICO_NAT_CORE_H
#define SNOS_PICO_NAT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAT_TCP_MAX 8
#define NAT_UDP_MAX 8
#define NAT_MTU 1500

typedef struct nat nat_t;
typedef struct {
    void (*emit)(void *arg, const uint8_t *packet, size_t len);
    bool (*tcp_connect)(void *arg, unsigned slot, uint32_t ip, uint16_t port);
    bool (*tcp_write)(void *arg, unsigned slot, const uint8_t *data, size_t len);
    void (*tcp_shutdown)(void *arg, unsigned slot);
    void (*tcp_abort)(void *arg, unsigned slot);
    bool (*udp_send)(void *arg, unsigned slot, uint32_t ip, uint16_t port,
                     const uint8_t *data, size_t len);
    bool (*forward_add)(void *arg, uint16_t port);
    void (*forward_del)(void *arg, uint16_t port);
    void (*tcp_recved)(void *arg, unsigned slot, size_t len);
    void *arg;
} nat_ops_t;

typedef struct { uint32_t in, out, drop; uint8_t tcp, udp, forwards; } nat_stats_t;
size_t nat_core_size(void);

/* All state is fixed-size: 8 TCP flows, 8 UDP flows, no heap. */
struct nat;
void nat_core_init(nat_t *nat, const nat_ops_t *ops);
void nat_core_set_resolver(nat_t *nat, uint32_t address);
void nat_core_set_resolvers(nat_t *nat, const uint32_t *addresses, size_t count);
void nat_core_set_adapter(nat_t *nat, bool (*emit_try)(void *, const uint8_t *, size_t),
                          bool (*link_busy)(void *), void (*udp_close)(void *, unsigned),
                          bool (*tcp_queued)(void *, unsigned));
void nat_core_reset(nat_t *nat);
void nat_core_input(nat_t *nat, const uint8_t *packet, size_t len, uint32_t now);
void nat_core_poll(nat_t *nat, uint32_t now);
void nat_core_tcp_connected(nat_t *nat, unsigned slot, int error, uint32_t now);
bool nat_core_tcp_received(nat_t *nat, unsigned slot, const uint8_t *data, size_t len,
                           uint32_t now);
void nat_core_tcp_eof(nat_t *nat, unsigned slot, uint32_t now);
void nat_core_tcp_error(nat_t *nat, unsigned slot);
void nat_core_udp_received(nat_t *nat, unsigned slot, const uint8_t *data, size_t len,
                           uint32_t now);
int nat_core_inbound_accept(nat_t *nat, uint16_t port, uint32_t now);
int nat_core_inbound_reserve(nat_t *nat, uint16_t port, uint32_t now);
void nat_core_inbound_start(nat_t *nat, unsigned slot, uint32_t now);
bool nat_core_forward_add(nat_t *nat, uint16_t port, uint8_t channel);
void nat_core_forward_del(nat_t *nat, uint16_t port, uint8_t channel);
nat_stats_t nat_core_stats(const nat_t *nat);
bool nat_core_udp_used(const nat_t *nat, unsigned slot);

#endif
