#!/usr/bin/env python3
"""Final validation gaps. Each case runs in a fresh process; failures are strict.

No emulator or production source substitutions. Clock/IRQ/netif boundaries
are host fixtures; deterministic scheduling is not physical IRQ timing.
"""
import os
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
sdk = pathlib.Path(os.environ.get("PICO_SDK_PATH", "/home/ricardo/pico/pico-sdk"))
lwip = sdk / "lib/lwip/src"
build = root / "pico/build/final_validation_test"
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
                  "tests/final_validation_harness.c")]
from console_main_fixture import prepare
console_main_flags = prepare(root, build)
# The console uses 24-bit far pointers; host-only warnings in its SDK headers
# are disabled for this translation unit, not for Pico or lwIP sources.
console_obj = build / "console_peer.o"
subprocess.run([*common, *console_main_flags, "-Wno-pointer-to-int-cast", "-Wno-unused-function",
                "-Wno-unknown-pragmas", "-ffunction-sections", "-fdata-sections",
                "-c", str(root / "pico/tests/transition_console_peer.c"),
                "-o", str(console_obj)], check=True)
lwip_sources.append(console_obj)
lwip_sources.append(root / "pico/tests/final_validation_relay_peer.c")
common += ["-Wl,--gc-sections"]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
import json
exe = build / "properties"
subprocess.run([*common, *map(str, lwip_sources), "-o", str(exe)], check=True)
names = subprocess.check_output([str(exe), "--list"], text=True).splitlines()
results = {}
for name in names:
    result = subprocess.run([str(exe), name], capture_output=True, text=True, timeout=10)
    results[name] = {"pass": int(result.returncode == 0), "fail": int(result.returncode != 0),
                     "returncode": result.returncode, "diagnostic": result.stderr.strip()}
    print(f"{'FAIL' if result.returncode else 'PASS'} {name}", flush=True)
    if result.stderr:
        print(result.stderr.strip(), flush=True)
(build / "results.json").write_text(json.dumps(results, indent=2) + "\n")
failures = sum(r["fail"] for r in results.values())
print(f"final validation: {len(names) - failures} pass, {failures} fail")
sys.exit(bool(failures))
