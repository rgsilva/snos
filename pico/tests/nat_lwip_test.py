#!/usr/bin/env python3
"""Exercise unchanged nat.c against SDK lwIP, with a fake netif and Pico clock.

Each assertion runs in its own process. Named bug regressions intentionally fail.
-ftrivial-auto-var-init=pattern makes uninitialized receive bytes reproducible.
"""
import os
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
sdk = pathlib.Path(os.environ.get("PICO_SDK_PATH", "/home/ricardo/pico/pico-sdk"))
lwip = sdk / "lib/lwip/src"
build = root / "pico/build/nat_lwip_test"
for directory in (build / "arch", build / "pico"):
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
#include <stdint.h>
#include <stdlib.h>
uint32_t get_absolute_time(void);
static inline uint32_t to_ms_since_boot(uint32_t t) { return t; }
#define panic(x) abort()
''')
(build / "pico/cyw43_arch.h").write_text('''
void cyw43_arch_lwip_begin(void);
void cyw43_arch_lwip_end(void);
''')
sources = sorted(lwip.glob("core/*.c")) + sorted(lwip.glob("core/ipv4/*.c"))
sources += [lwip / "netif/ethernet.c", root / "pico/nat_core.c", root / "pico/tests/nat_lwip_harness.c"]
exe = build / "test"
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
subprocess.run(["cc", "-std=c11", "-g", "-O0", "-Wall", "-Wextra", "-Werror",
                "-ftrivial-auto-var-init=pattern", "-I", str(build), "-I", str(root / "pico"),
                "-I", str(lwip / "include"), *map(str, sources), "-o", str(exe)], check=True)
failed = []
for name in subprocess.check_output([str(exe), "--list"], text=True).splitlines():
    result = subprocess.run([str(exe), name], capture_output=True, text=True)
    print(result.stdout, end="")
    if result.returncode:
        failed.append(name)
        print(f"FAIL {name}: {result.stderr.strip()}")
print(f"nat lwIP: {len(failed)} failing scenarios")
sys.exit(bool(failed))
