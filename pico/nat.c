#include "nat.h"
#include "nat_core.h"
#include "ctrl.h"
#include "link.h"
#include "log.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include <string.h>

/* nat_t is deliberately private to the core; this is comfortably above its
 * fixed 8-flow footprint and keeps the host-facing API opaque. */
#define CORE_BYTES 26000
typedef union { max_align_t align; uint8_t bytes[CORE_BYTES]; } core_store_t;
static core_store_t store;
static nat_t *core = (nat_t *)&store;
static struct tcp_pcb *tcp_pcbs[NAT_TCP_MAX], *listeners[NAT_TCP_MAX];
static struct udp_pcb *udp_slots[NAT_UDP_MAX];
static uint16_t listener_ports[NAT_TCP_MAX];
static int8_t connected_pending[NAT_TCP_MAX];
typedef struct { struct tcp_pcb *pcb; uint16_t port; } accepted_t;
static accepted_t accepted_pending[4];
static uint8_t ipbuf[LINK_MTU + 2];
static size_t ipused;
static bool ready, up;
static uint32_t now(void) { return to_ms_since_boot(get_absolute_time()); }
static void emit(void *arg, const uint8_t *p, size_t len) {
    (void)arg;
    if (!link_push(CH_IP, p, len)) log_puts("nat: link queue full");
}
static void nat_tcp_err(void *arg, err_t e) {
    unsigned i = (unsigned)(uintptr_t)arg;
    (void)e; tcp_pcbs[i] = NULL; nat_core_tcp_error(core, i);
}
static err_t nat_tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t e) {
    unsigned i = (unsigned)(uintptr_t)arg;
    (void)pcb;
    if (e != ERR_OK) { if (p) pbuf_free(p); nat_core_tcp_error(core, i); return ERR_OK; }
    if (!p) { nat_core_tcp_eof(core, i, now()); return ERR_OK; }
    uint8_t b[NAT_MTU];
    if (p->tot_len > sizeof(b) || !nat_core_tcp_received(core, i, b, p->tot_len, now())) return ERR_MEM;
    pbuf_copy_partial(p, b, p->tot_len, 0); pbuf_free(p); return ERR_OK;
}
static err_t tcp_connected(void *arg, struct tcp_pcb *pcb, err_t e) {
    (void)pcb; connected_pending[(unsigned)(uintptr_t)arg] = e == ERR_OK ? 1 : -1; return ERR_OK;
}
static bool op_tcp_connect(void *arg, unsigned i, uint32_t addr, uint16_t port) {
    (void)arg; ip_addr_t ip; IP_ADDR4(&ip, addr >> 24, addr >> 16, addr >> 8, addr);
    struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_V4); if (!p) return false;
    tcp_pcbs[i] = p; tcp_arg(p, (void *)(uintptr_t)i); tcp_err(p, nat_tcp_err); tcp_recv(p, nat_tcp_recv); tcp_nagle_disable(p);
    if (tcp_connect(p, &ip, port, tcp_connected) != ERR_OK) { tcp_pcbs[i] = NULL; tcp_abort(p); return false; }
    return true;
}
static bool op_tcp_write(void *arg, unsigned i, const uint8_t *p, size_t len) {
    (void)arg; struct tcp_pcb *t = tcp_pcbs[i];
    return t && len <= tcp_sndbuf(t) && tcp_write(t, p, (u16_t)len, TCP_WRITE_FLAG_COPY) == ERR_OK && tcp_output(t) == ERR_OK;
}
static void op_tcp_shutdown(void *arg, unsigned i) { (void)arg; if (tcp_pcbs[i]) tcp_shutdown(tcp_pcbs[i], 0, 1); }
static void op_tcp_abort(void *arg, unsigned i) { (void)arg; if (tcp_pcbs[i]) { struct tcp_pcb *p=tcp_pcbs[i]; tcp_pcbs[i]=NULL; tcp_arg(p,NULL);tcp_err(p,NULL);tcp_abort(p); } }
static void op_tcp_recved(void *arg, unsigned i, size_t len) { (void)arg; if (tcp_pcbs[i]) tcp_recved(tcp_pcbs[i], (u16_t)len); }
static void nat_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)pcb; (void)addr; (void)port; unsigned i=(unsigned)(uintptr_t)arg; uint8_t b[NAT_MTU];
    if (p->tot_len <= sizeof(b)) { pbuf_copy_partial(p,b,p->tot_len,0); nat_core_udp_received(core,i,b,p->tot_len,now()); }
    pbuf_free(p);
}
static bool op_udp_send(void *arg, unsigned i, uint32_t addr, uint16_t port, const uint8_t *p, size_t len) {
    (void)arg; if (!udp_slots[i]) { udp_slots[i]=udp_new_ip_type(IPADDR_TYPE_V4); if(!udp_slots[i])return false; udp_recv(udp_slots[i],nat_udp_recv,(void *)(uintptr_t)i); }
    ip_addr_t ip; IP_ADDR4(&ip,addr>>24,addr>>16,addr>>8,addr); struct pbuf *b=pbuf_alloc(PBUF_TRANSPORT,(u16_t)len,PBUF_RAM); if(!b)return false;
    pbuf_take(b,p,(u16_t)len); err_t e=udp_sendto(udp_slots[i],b,&ip,port);pbuf_free(b);return e==ERR_OK;
}
static err_t accepted(void *arg, struct tcp_pcb *pcb, err_t e) {
    if(e!=ERR_OK)return e;
    for (unsigned i = 0; i < 4; ++i) if (!accepted_pending[i].pcb) {
        accepted_pending[i] = (accepted_t){pcb, (uint16_t)(uintptr_t)arg}; return ERR_OK;
    }
    tcp_abort(pcb); return ERR_ABRT;
}
static bool op_forward_add(void *arg, uint16_t port) {
    (void)arg; for(unsigned i=0;i<NAT_TCP_MAX;++i)if(!listeners[i]){struct tcp_pcb *p=tcp_new_ip_type(IPADDR_TYPE_V4);if(!p)return false;if(tcp_bind(p,IP_ANY_TYPE,port)!=ERR_OK){tcp_abort(p);return false;}p=tcp_listen_with_backlog(p,4);if(!p)return false;listeners[i]=p;listener_ports[i]=port;tcp_arg(p,(void *)(uintptr_t)port);tcp_accept(p,accepted);return true;}return false;
}
static void op_forward_del(void *arg, uint16_t port) { (void)arg; for(unsigned i=0;i<NAT_TCP_MAX;++i)if(listeners[i]&&listener_ports[i]==port){tcp_close(listeners[i]);listeners[i]=NULL;listener_ports[i]=0;return;} }
static void ops_init(void) { nat_ops_t o={emit,op_tcp_connect,op_tcp_write,op_tcp_shutdown,op_tcp_abort,op_udp_send,op_forward_add,op_forward_del,op_tcp_recved,NULL};nat_core_init(core,&o); }
void nat_init(void) { memset(&store,0,sizeof(store)); if (nat_core_size() > sizeof(store)) panic("nat core"); ops_init();ready=true; }
void nat_reset(void) { if(!ready)return;cyw43_arch_lwip_begin();nat_core_reset(core);for(unsigned i=0;i<NAT_UDP_MAX;++i)if(udp_slots[i]){udp_remove(udp_slots[i]);udp_slots[i]=NULL;}for(unsigned i=0;i<4;++i)if(accepted_pending[i].pcb){tcp_abort(accepted_pending[i].pcb);accepted_pending[i].pcb=NULL;}memset(connected_pending,0,sizeof(connected_pending));cyw43_arch_lwip_end();ipused=0; }
void nat_configure(bool wifi_up) { up=wifi_up;if(!ready||!up)return;cyw43_arch_lwip_begin();const ip4_addr_t *a=netif_ip4_addr(netif_default);uint32_t v=ip4_addr_get_u32(a);uint8_t b[]={v>>24,v>>16,v>>8,v};ctrl_address(b);const ip_addr_t *dns=dns_getserver(0);if(IP_IS_V4(dns))nat_core_set_resolver(core,ip4_addr_get_u32(ip_2_ip4(dns)));cyw43_arch_lwip_end(); }
void nat_ip_bytes(const uint8_t *data,size_t len) { if(!ready||!up)return;cyw43_arch_lwip_begin();for(size_t i=0;i<len;++i){ipbuf[ipused++]=data[i];if(ipused<2)continue;size_t n=ipbuf[0]|((size_t)ipbuf[1]<<8);if(!n||n>LINK_MTU){log_puts("nat: IP stream desync");ipused=0;break;}if(ipused==n+2){nat_core_input(core,ipbuf+2,n,now());ipused=0;}}cyw43_arch_lwip_end(); }
void nat_poll(void) { if(!ready||!up){ctrl_poll();return;}cyw43_arch_lwip_begin();
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) if (connected_pending[i]) {
        int e = connected_pending[i]; connected_pending[i] = 0; nat_core_tcp_connected(core, i, e < 0, now());
    }
    for (unsigned j = 0; j < 4; ++j) if (accepted_pending[j].pcb) {
        struct tcp_pcb *p = accepted_pending[j].pcb; int i = nat_core_inbound_accept(core, accepted_pending[j].port, now());
        accepted_pending[j].pcb = NULL;
        if (i < 0) tcp_abort(p); else { tcp_pcbs[i] = p; tcp_arg(p, (void *)(uintptr_t)i); tcp_err(p, nat_tcp_err); tcp_recv(p, nat_tcp_recv); tcp_nagle_disable(p); }
    }
    nat_core_poll(core,now());cyw43_arch_lwip_end();ctrl_poll(); }
bool nat_forward_add(uint16_t port,uint8_t ch){if(!ready||!up)return false;cyw43_arch_lwip_begin();bool ok=nat_core_forward_add(core,port,ch);cyw43_arch_lwip_end();return ok;}
void nat_forward_del(uint16_t port,uint8_t ch){if(!ready)return;cyw43_arch_lwip_begin();nat_core_forward_del(core,port,ch);cyw43_arch_lwip_end();}
nat_stats_t nat_stats(void){nat_stats_t r={0};if(!ready)return r;cyw43_arch_lwip_begin();nat_stats_t s=nat_core_stats(core);cyw43_arch_lwip_end();r=(nat_stats_t){s.in,s.out,s.drop,s.tcp,s.udp,s.forwards};return r;}
