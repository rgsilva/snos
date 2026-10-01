#include "relay.h"
#include "ctrl.h"
#include "nat.h"
#include "log.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include <stdio.h>
#include <string.h>
#define BUFFER_SIZE 16384
static struct tcp_pcb *pcb;
static uint8_t input[BUFFER_SIZE], output[BUFFER_SIZE];
static size_t in_len, out_len;
static bool connected, failed, resolving, resolved;
static ip_addr_t host;
static char hostname[64];
static bool network_up, ready;
static uintptr_t dns_generation;
static uint32_t retry_at, connect_at, backoff = 1000;
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static void error_cb(void *arg, err_t error) {
    (void)arg; (void)error; pcb = NULL; connected = false; failed = true;
}
static err_t recv_cb(void *arg, struct tcp_pcb *t, struct pbuf *p, err_t error) {
    (void)arg; (void)error;
    if (!p) { failed = true; return ERR_OK; }
    if (p->tot_len > sizeof(input) - in_len) return ERR_MEM;
    in_len += pbuf_copy_partial(p, input + in_len, p->tot_len, 0);
    tcp_recved(t, p->tot_len); pbuf_free(p); return ERR_OK;
}
static err_t connected_cb(void *arg, struct tcp_pcb *t, err_t error) {
    (void)arg; (void)t;
    if (error != ERR_OK) { failed = true; return error; }
    connected = true; backoff = 1000; return ERR_OK;
}
static void dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    if ((uintptr_t)arg != dns_generation) return;
    resolving = false;
    if (addr) { host = *addr; resolved = true; } else failed = true;
}
/* Called with the lwIP lock held. */
static void disconnect(void) {
    if (pcb) { tcp_arg(pcb, NULL); tcp_err(pcb, NULL); tcp_recv(pcb, NULL); tcp_abort(pcb); }
    pcb = NULL; connected = false; failed = false;
    in_len = out_len = 0;
    link_purge(CH_IP);
    link_purge(CH_CTRL);
    retry_at = now_ms() + backoff;
    if (backoff < 30000) backoff = backoff > 15000 ? 30000 : backoff * 2;
}
void relay_init(void) { ready = true; retry_at = now_ms(); }
void relay_configure(const char *name, bool wifi_up) {
    cyw43_arch_lwip_begin();
    if (strcmp(hostname, name) || network_up != wifi_up) {
        disconnect();
        ctrl_clear_address();
        ++dns_generation;
        resolving = resolved = false;
        snprintf(hostname, sizeof(hostname), "%s", name);
        network_up = wifi_up;
        backoff = 1000; retry_at = now_ms();
    }
    cyw43_arch_lwip_end();
}
void relay_reset(void) {
    if (!ready) return;
    cyw43_arch_lwip_begin(); disconnect(); cyw43_arch_lwip_end();
}
bool relay_send(uint8_t type, const uint8_t *data, size_t len) {
    if (!ready) return false;
    cyw43_arch_lwip_begin();
    bool ok = network_up && hostname[0] && len <= LINK_MTU && len + 3 <= sizeof(output) - out_len;
    if (ok) {
        output[out_len++] = type; output[out_len++] = len & 255; output[out_len++] = len >> 8;
        memcpy(output + out_len, data, len); out_len += len;
    }
    cyw43_arch_lwip_end(); return ok;
}
static void relay_input(const uint8_t *packet, size_t len) {
    if (!relay_send(1, packet, len))
        log_puts("relay: IP output queue full");
}
void relay_ip_bytes(const uint8_t *data, size_t len) {
    ip_stream_bytes(data, len, relay_input);
}
void relay_poll(void) {
    if (!ready) { ctrl_poll(); return; }
    cyw43_arch_lwip_begin();
    uint32_t now = now_ms();
    if (failed || (pcb && !connected && (uint32_t)(now - connect_at) > 15000)) disconnect();
    if (network_up && hostname[0] && !pcb && !resolving && (int32_t)(now - retry_at) >= 0) {
        if (!resolved) {
            err_t e = dns_gethostbyname(hostname, &host, dns_cb, (void *)dns_generation);
            if (e == ERR_OK) resolved = true;
            else if (e == ERR_INPROGRESS) resolving = true;
            else disconnect();
        }
        if (resolved) {
            pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
            if (!pcb) disconnect();
            else {
                tcp_err(pcb, error_cb); tcp_recv(pcb, recv_cb); tcp_nagle_disable(pcb);
                connect_at = now;
                if (tcp_connect(pcb, &host, 3210, connected_cb) != ERR_OK) disconnect();
            }
        }
    }
    /* Callbacks only append; dispatch is owned by the foreground. */
    while (in_len >= 3) {
        uint8_t type = input[0];
        size_t n = input[1] | ((size_t)input[2] << 8);
        if (n > LINK_MTU) { disconnect(); break; }
        if (in_len < n + 3) break;
        const uint8_t *p = input + 3;
        if (type == 1 && n && !link_push(CH_IP, p, n)) break;
        if (type == 4 && n == 3 && !ctrl_error(p)) break;
        if (type == 5 && n == 4) ctrl_address(p);
        in_len -= n + 3; memmove(input, input + n + 3, in_len);
    }
    if (connected && out_len) {
        size_t n = out_len;
        if (n > tcp_sndbuf(pcb)) n = tcp_sndbuf(pcb);
        if (n) {
            err_t e = tcp_write(pcb, output, (u16_t)n, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) { out_len -= n; memmove(output, output + n, out_len); tcp_output(pcb); }
            else if (e != ERR_MEM) failed = true;
        }
    }
    cyw43_arch_lwip_end();
    ctrl_poll();
}
const char *relay_state(void) {
    if (!ready) return "offline";
    cyw43_arch_lwip_begin();
    const char *s = !network_up || !hostname[0] ? "offline" : connected ? "connected" : pcb ? "connecting" : resolving ? "dns" : "backoff";
    cyw43_arch_lwip_end(); return s;
}
