/* uIP's poll path, alone: snes/kernel/uip/uip.c, compiled for the host with
 * the console's uip-conf.h. Two established connections; a poll of the first
 * sends 100 bytes, then a poll of the second, whose application has nothing
 * to say, must send nothing. uIP 1.0 left uip_slen as the first connection's
 * 100 and sent that many stale bytes on the second -- found when ip.c began
 * polling a connection only to reopen its receive window
 * (docs/link-throughput.md). The checksums are uip.c's contract with the
 * kernel (UIP_ARCH_CHKSUM); plain C here, their result is not under test. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uip.h"

static int sender = -1;
static unsigned calls;
void snos_tcp_appcall(void) {
    static char data[100];
    ++calls;
    if (uip_poll() && uip_conn == &uip_conns[sender]) uip_send(data, sizeof data);
}
void snos_udp_appcall(void) {}
static uint16_t sum(const uint8_t *p, unsigned n) {
    uint32_t s = 0;
    for (unsigned i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)s;
}
u16_t uip_chksum(u16_t *data, u16_t len) { return sum((const uint8_t *)data, len); }
u16_t uip_ipchksum(void) { return sum(uip_buf, UIP_IPH_LEN); }
u16_t uip_tcpchksum(void) { return 0; }
u16_t uip_udpchksum(void) { return 0; }

static void establish(int i) {
    struct uip_conn *c = &uip_conns[i];
    memset(c, 0, sizeof *c);
    c->tcpstateflags = UIP_ESTABLISHED;
    c->lport = HTONS(1000 + i);
    c->rport = HTONS(7778);
    uip_ipaddr(c->ripaddr, 192, 168, 1, 50);
    c->mss = c->initialmss = 1000;
}

int main(void) {
    uip_init();
    establish(0);
    establish(1);
    sender = 0;
    uip_poll_conn(&uip_conns[0]);
    assert(calls == 1 && uip_len == UIP_TCPIP_HLEN + 100 && uip_conns[0].len == 100);
    uip_len = 0;
    uip_poll_conn(&uip_conns[1]);
    assert(calls == 2);
    if (uip_len != 0 || uip_conns[1].len != 0) {
        fprintf(stderr, "a poll with nothing to send sent %u bytes (outstanding %u)\n",
                uip_len ? uip_len - UIP_TCPIP_HLEN : 0, uip_conns[1].len);
        return 1;
    }
    printf("uip poll: a connection with nothing to say sends nothing after another sent\n");
    return 0;
}
