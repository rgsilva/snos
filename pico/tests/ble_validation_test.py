#!/usr/bin/env python3
"""Independent BLE regressions. Failures are real failures, never xfails.

Compile the production parsers and btkbd.c against the installed BTstack headers.
Radio/flash calls are spies; notification formatting runs BTstack's actual code.
Every C scenario runs in a fresh process, including guard-page bounds checks.
"""
import os
import pathlib
import resource
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK = pathlib.Path(os.environ.get("PICO_SDK_PATH", "/home/ricardo/pico/pico-sdk"))
BT = SDK / "lib/btstack"
BUILD = ROOT / "build/ble_validation_test"
(BUILD / "pico").mkdir(parents=True, exist_ok=True)
(BUILD / "pico/btstack_cyw43.h").write_text("""
#include <stdbool.h>
bool btstack_cyw43_init(void *context);
""")
(BUILD / "pico/cyw43_arch.h").write_text("""
#include <stdint.h>
void *cyw43_arch_async_context(void);
static inline uint32_t get_absolute_time(void) { return 100; }
static inline uint32_t to_ms_since_boot(uint32_t t) { return t; }
""")
flags = ["cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
         "-fshort-enums", "-ffunction-sections", "-fdata-sections",
         "-DENABLE_BLE", "-DENABLE_CLASSIC",
         "-I", str(BUILD), "-I", str(ROOT), "-I", str(BT / "src"),
         "-I", str(BT / "platform/embedded"), "-I", str(BT / "3rd-party/micro-ecc"),
         "-I", str(BT / "3rd-party/bluedroid/encoder/include"),
         "-I", str(BT / "3rd-party/bluedroid/decoder/include"),
         "-I", str(BT / "3rd-party/yxml")]
exe = BUILD / "scenarios"
subprocess.run([
    *flags, str(ROOT / "tests/ble_validation_harness.c"),
    str(ROOT / "tests/ble_validation_notification.c"),
    str(ROOT / "hogp.c"), str(ROOT / "hidkeys.c"),
    str(BT / "src/btstack_util.c"), str(BT / "src/btstack_linked_list.c"),
    str(BT / "src/ble/le_device_db_tlv.c"),
    str(BT / "src/classic/btstack_link_key_db_tlv.c"),
    str(BT / "platform/embedded/btstack_tlv_flash_bank.c"),
    "-Wl,--gc-sections", "-o", str(exe),
], check=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
names = subprocess.check_output([str(exe), "--list"], text=True).splitlines()
failed = []
for name in names:
    result = subprocess.run([str(exe), name], capture_output=True, text=True)
    print(f"{'FAIL' if result.returncode else 'PASS'} {name}")
    print(result.stdout + result.stderr, end="")
    if result.returncode:
        failed.append(name)
print(f"BLE validation: {len(names) - len(failed)} passed, {len(failed)} failed")
sys.exit(bool(failed))
