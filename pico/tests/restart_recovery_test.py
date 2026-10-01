#!/usr/bin/env python3
"""Peer restarts in both directions (restart_recovery_harness.c).

Built exactly as transition_test.py builds its properties -- SDK lwIP, the
real Pico link/NAT/ctrl, the real console receiver -- around a different
harness. The Pico-side properties run against the legacy console (main at
99b6002) and the checked-out one at windows 1, 4 and 8; the console-side
ones, which are the console's own receive path, against the checked-out one.
"""
import os
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "pico/tests"))
from console_main_fixture import prepare

sdk = pathlib.Path(os.environ.get("PICO_SDK_PATH", "/home/ricardo/pico/pico-sdk"))
lwip = sdk / "lib/lwip/src"
build = root / "pico/build/restart_recovery_test"
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
sources = sorted(lwip.glob("core/*.c")) + sorted(lwip.glob("core/ipv4/*.c"))
sources += [lwip / "netif/ethernet.c"]
sources += [root / "pico" / name for name in
            ("nat_core.c", "link.c", "ctrl.c", "kbd.c", "hidkeys.c",
             "tests/restart_recovery_harness.c", "tests/transition_relay_peer.c")]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def console_flags(window):
    if not window:
        return prepare(root, build)
    return ["-I", str(root / "snes/kernel"),
            '-DCONSOLE_NET_SOURCE="' + str(root / "snes/kernel/net.c") + '"',
            "-DTRANSITION_CURRENT=1", f"-DTEST_CONSOLE_WINDOW={window}"]


failed = 0
for window in (0, 1, 4, 8):
    label = "legacy console" if not window else f"current console, window {window}"
    console_obj = build / f"console_peer_{window}.o"
    subprocess.run([*common, *console_flags(window), "-Wno-pointer-to-int-cast",
                    "-Wno-unused-function", "-Wno-unknown-pragmas", "-ffunction-sections",
                    "-fdata-sections", "-c", str(root / "pico/tests/transition_console_peer.c"),
                    "-o", str(console_obj)], check=True)
    exe = build / f"recovery_{window}"
    subprocess.run([*common, "-Wl,--gc-sections", *map(str, sources), str(console_obj),
                    "-o", str(exe)], check=True)
    names = subprocess.check_output([str(exe), "--list" if window else "--list-pico"],
                                    text=True).splitlines()
    for name in names:
        r = subprocess.run([str(exe), name], capture_output=True, text=True, timeout=30)
        ok = r.returncode == 0
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'} {label}: {name}", flush=True)
        if not ok:
            print("  " + r.stderr.strip().replace("\n", "\n  "), flush=True)
print("restart recovery:", "FAIL" if failed else "PASS")
sys.exit(bool(failed))
