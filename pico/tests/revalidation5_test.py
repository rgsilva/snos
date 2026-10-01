#!/usr/bin/env python3
"""Fifth-round properties: progress, keyboard ordering, callback lifetime.

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
build = root / "pico/build/revalidation5_test"
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
                  "tests/revalidation5_lwip_harness.c")]
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
    print(f"revalidation5 {label}: {len(names) - failures} pass, {failures} fail")
# A separate process exposes actual console frames to the actual Gateway.
# Its wire/mailbox callbacks are stubbed as in the C peer tests, not Mesen.
driver = build / "console_driver"
subprocess.run([*common, *console_main_flags, "-Wno-pointer-to-int-cast", "-Wno-unused-function",
                "-Wno-unknown-pragmas", "-ffunction-sections", "-fdata-sections",
                str(root / "pico/tests/revalidation5_console_driver.c"),
                "-o", str(driver)], check=True)
sys.path.insert(0, str(root / "snes/tools"))
import gateway

def gateway_restart_case(inflight, retry=False):
    console = subprocess.Popen([str(driver)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    gw = gateway.Gateway()
    def exchange():
        console.stdin.write(b"F" + gw.next_out())
        console.stdin.flush()
        reply = bytearray(console.stdout.read(gateway.FRAME))
        assert len(reply) == gateway.FRAME
        gw.on_frame(reply)
        return reply
    try:
        # Complete initial handshakes in both directions.
        exchange()
        console.stdin.write(b"H")
        for _ in range(6): exchange()
        assert not gw.syn and not gw.peer_syn_seen
        # A legitimate HELLO is waiting when the external gateway restarts.
        console.stdin.write(b"H")
        if inflight:
            console.stdin.write(b"F" + gw.next_out())
            console.stdin.flush()
            pending = console.stdout.read(gateway.FRAME)
            assert pending[gateway.FLAGS] & gateway.F_DATA
            # The old daemon never reads this console frame.
        gw.teardown()
        gw = gateway.Gateway()
        seen = []
        dispatch = gw.dispatch
        def record_dispatch():
            seen.append(gw.ctype)
            dispatch()
        gw.dispatch = record_dispatch
        for _ in range(8): exchange()
        expected_hellos = 1
        if retry:
            expected_hellos = seen.count(gateway.C2S_HELLO) + 1
            # ip.c retries HELLO after 120 frames. Verify that recovery
            # works and that the mirrored SYN does not cause a reset loop.
            console.stdin.write(b"H")
            for _ in range(32): exchange()
        assert seen.count(gateway.C2S_HELLO) == expected_hellos, f"HELLO deliveries after gateway restart: {seen}"
        assert not gw.syn and not gw.peer_syn_seen
    finally:
        gw.teardown()
        console.stdin.close()
        assert console.wait(timeout=5) == 0

name = "t_gateway_restart_fresh_hello_recovers_without_reset_loop"
try:
    gateway_restart_case(False, retry=True)
    print("PASS", name)
except AssertionError as error:
    failed.append(name)
    print(f"FAIL {name}: {error}")
print(f"revalidation5 total: {len(names) + 1 - len(failed)} pass, {len(failed)} fail")
sys.exit(bool(failed))
