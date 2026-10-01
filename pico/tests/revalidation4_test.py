#!/usr/bin/env python3
"""Fourth-round regressions: delivery and callback generation contracts.

Each named scenario runs in a fresh process. Failures are never suppressed.
The hardware clock/IRQ primitives are host stubs; this is not a timing simulator.
"""
import os
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
sdk = pathlib.Path(os.environ.get("PICO_SDK_PATH", "/home/ricardo/pico/pico-sdk"))
lwip = sdk / "lib/lwip/src"
build = root / "pico/build/revalidation4_test"
for directory in (build / "arch", build / "pico", build / "hardware"):
    directory.mkdir(parents=True, exist_ok=True)
(build / "arch/cc.h").write_text('''
#ifndef TEST_ARCH_CC_H
#define TEST_ARCH_CC_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
typedef int sys_prot_t;
#define LWIP_RAND() 12345u
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "%s\\n", (x)); abort(); } while (0)
#endif
''')
(build / "pico/stdlib.h").write_text('''
#ifndef TEST_STDLIB_H
#define TEST_STDLIB_H
#include <stdint.h>
#include <stdlib.h>
uint32_t get_absolute_time(void);
static inline uint32_t to_ms_since_boot(uint32_t t) { return t; }
#define panic(x) abort()
#endif
''')
(build / "pico/time.h").write_text('#include "pico/stdlib.h"\n')
(build / "pico/cyw43_arch.h").write_text('''
void cyw43_arch_lwip_begin(void);
void cyw43_arch_lwip_end(void);
''')
(build / "hardware/sync.h").write_text('''
#include <stdint.h>
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t saved) { (void)saved; }
''')
common = ["cc", "-std=c11", "-g", "-O0", "-Wall", "-Wextra", "-Werror",
          "-ftrivial-auto-var-init=pattern", "-I", str(build), "-I", str(root / "pico"),
          "-I", str(lwip / "include")]
lwip_sources = sorted(lwip.glob("core/*.c")) + sorted(lwip.glob("core/ipv4/*.c"))
lwip_sources += [lwip / "netif/ethernet.c"]
lwip_sources += [root / "pico" / name for name in
                 ("nat_core.c", "link.c", "ctrl.c", "kbd.c", "hidkeys.c",
                  "tests/revalidation4_lwip_harness.c")]
from console_main_fixture import prepare
console_main_flags = prepare(root, build)
# The console uses 24-bit far pointers; host-only warnings in its SDK headers
# are disabled for this translation unit, not for Pico or lwIP sources.
console_obj = build / "console_peer.o"
subprocess.run([*common, *console_main_flags, "-Wno-pointer-to-int-cast", "-Wno-unused-function",
                "-Wno-unknown-pragmas", "-ffunction-sections", "-fdata-sections",
                "-c", str(root / "pico/tests/revalidation4_console_peer.c"),
                "-o", str(console_obj)], check=True)
lwip_sources.append(console_obj)
common += ["-Wl,--gc-sections"]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
failed = []
# Exercise the actual gateway sender over retransmissions, ACKs and sequence
# wrap. It must never change its SYN base within one startup episode.
sys.path.insert(0, str(root / "snes/tools"))
import gateway

gw = gateway.Gateway()
try:
    first = gw.next_out()
    assert first[gateway.FLAGS] & gateway.F_SYN
    gw.push(1, b"a")
    frame = gw.next_out()
    assert frame[gateway.SEQ] == first[gateway.SEQ]
    assert frame[gateway.FLAGS] & gateway.F_SYN
    assert gw.next_out() == frame
    for _ in range(300):
        ack = bytearray(gateway.FRAME)
        ack[gateway.ACK] = frame[gateway.SEQ]
        ack[gateway.SUM] = gateway.checksum(ack)
        gw.on_frame(ack)
        gw.push(1, b"b")
        next_frame = gw.next_out()
        assert next_frame[gateway.SEQ] == (frame[gateway.SEQ] + 1) & 255
        assert not next_frame[gateway.FLAGS] & gateway.F_SYN
        assert gw.next_out() == next_frame
        frame = next_frame
    print("PASS t_gateway_syn_base_stable_until_ack_and_across_wrap")
except AssertionError:
    failed.append("t_gateway_syn_base_stable_until_ack_and_across_wrap")
    print("FAIL t_gateway_syn_base_stable_until_ack_and_across_wrap")
finally:
    gw.teardown()
for label, sources in (("lwip", lwip_sources),):
    exe = build / label
    subprocess.run([*common, *map(str, sources), "-o", str(exe)], check=True)
    names = subprocess.check_output([str(exe), "--list"], text=True).splitlines()
    failures = 0
    for name in names:
        result = subprocess.run([str(exe), name], capture_output=True, text=True)
        print(result.stdout, end="")
        if result.returncode:
            failures += 1
            failed.append(name)
            print(f"FAIL {name}: {result.stderr.strip()}")
    print(f"revalidation4 {label}: {len(names) - failures} pass, {failures} fail")
sys.exit(bool(failed))
