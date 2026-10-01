#!/usr/bin/env python3
"""Differential: current ip.c checksum glue versus vendored uIP's C version.

Compile the real ip.c, discard unrelated functions at link time. Only ip_sum
is modelled: its little-endian ADC carry chain, final folds and XBA. This is
arithmetic validation, not a 65816 instruction/timing emulator.
"""
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
kernel = root / 'snes/kernel'
build = root / 'pico/build/checksum_validation'
build.mkdir(parents=True, exist_ok=True)
uip = (kernel / 'uip/uip.c').read_text()
start = uip.index('#if ! UIP_ARCH_CHKSUM')
end = uip.index('#endif /* UIP_ARCH_CHKSUM */', start)
reference = uip[start:end].replace('#if ! UIP_ARCH_CHKSUM', '#if 1', 1) + '\n#endif\n'
for name in ('uip_chksum', 'uip_ipchksum', 'uip_tcpchksum', 'uip_udpchksum'):
    reference = reference.replace(name, 'ref_' + name)
(build / 'reference.c').write_text('#include "uip.h"\n#define DEBUG_PRINTF(...) ((void)0)\n#define BUF ((struct uip_tcpip_hdr *)&uip_buf[UIP_LLH_LEN])\n' + reference)
(build / 'check.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "uip.h"
u8_t uip_buf[UIP_BUFSIZE + 2];
u16_t ref_uip_chksum(u16_t *, u16_t);
u16_t ref_uip_ipchksum(void), ref_uip_tcpchksum(void), ref_uip_udpchksum(void);
u16_t htons(u16_t n) { return HTONS(n); }
uint16_t ip_sum(const uint8_t *p, uint16_t n) {
    assert(!(n & 1));
    uint32_t a = 0, carry = 0;
    for (unsigned i = 0; i < n; i += 2) {
        a = (a & 65535) + p[i] + ((uint32_t)p[i + 1] << 8) + carry;
        carry = a >> 16;
    }
    a = (a & 65535) + carry;
    a = (a & 65535) + (a >> 16);
    return (uint16_t)((a << 8) | (a >> 8));
}
int main(void) {
    uint32_t seed = 17; unsigned cases = 0;
    /* Aligned and unaligned; all-zero, all-ones, alternating and random.
       Every supported datagram length includes both parities and >255. */
    for (unsigned style = 0; style < 8; ++style) {
        for (unsigned n = 0; n <= 1500; ++n) {
            for (unsigned i = 0; i < sizeof uip_buf; ++i) {
                seed = seed * 1664525u + 1013904223u;
                uip_buf[i] = style == 0 ? 0 : style == 1 ? 255 :
                             style == 2 ? (i & 1 ? 255 : 0) : seed >> 24;
            }
            for (unsigned off = 0; off < 2; ++off) {
                assert(uip_chksum((u16_t *)(uip_buf + off), n) ==
                       ref_uip_chksum((u16_t *)(uip_buf + off), n));
                ++cases;
            }
            if (n < UIP_IPH_LEN) continue;
            uip_buf[2] = n >> 8; uip_buf[3] = n;
            assert(uip_ipchksum() == ref_uip_ipchksum());
            assert(uip_tcpchksum() == ref_uip_tcpchksum());
            assert(uip_udpchksum() == ref_uip_udpchksum());
            cases += 3;
        }
    }
    printf("checksums: %u differential comparisons passed (ICMP/general, IPv4, TCP, UDP)\n", cases);
}
''')
flags = ['cc', '-std=gnu11', '-O1', '-D__far=', '-D__huge=', '-ffunction-sections',
         '-fdata-sections', '-Wno-pointer-to-int-cast', '-I', str(kernel),
         '-I', str(kernel / 'uip')]
exe = build / 'check'
subprocess.run([*flags, str(kernel / 'ip.c'), str(build / 'reference.c'),
                str(build / 'check.c'), '-Wl,--gc-sections', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
