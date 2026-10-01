#!/usr/bin/env python3
"""Fourth-pass BLE regressions; all listed scenarios must pass.

The separate ble_revalidation4_followups.py runs synthetic expected-failures.
"""
import os
import pathlib
import resource
import subprocess
import sys


def build_harness():
    ROOT = pathlib.Path(__file__).resolve().parents[1]
    SDK = pathlib.Path(os.environ.get("PICO_SDK_PATH", "/home/ricardo/pico/pico-sdk"))
    BT = SDK / "lib/btstack"
    BUILD = ROOT / "build/ble_revalidation4_test"
    (BUILD / "pico").mkdir(parents=True, exist_ok=True)
    (BUILD / "pico/btstack_cyw43.h").write_text("""
    #include <stdbool.h>
    bool btstack_cyw43_init(void *context);
    """)
    (BUILD / "pico/async_context.h").write_text("""
    typedef struct { unsigned depth; } async_context_t;
    void async_context_acquire_lock_blocking(async_context_t *context);
    void async_context_release_lock(async_context_t *context);
    """)
    (BUILD / "pico/cyw43_arch.h").write_text("""
    #include <stdint.h>
    void *cyw43_arch_async_context(void);
    extern uint32_t validation_now;
    static inline uint32_t get_absolute_time(void) { return validation_now; }
    static inline uint32_t to_ms_since_boot(uint32_t t) { return t; }
    """)
    flags = ["cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
             "-fshort-enums", "-ffunction-sections", "-fdata-sections",
             "-DENABLE_BLE", "-DENABLE_CLASSIC", "-DPICO_ON_DEVICE=1",
             "-I", str(BUILD), "-I", str(ROOT), "-I", str(BT / "src"),
             "-I", str(BT / "platform/embedded"), "-I", str(BT / "3rd-party/micro-ecc"),
             "-I", str(BT / "3rd-party/bluedroid/encoder/include"),
             "-I", str(BT / "3rd-party/bluedroid/decoder/include"),
             "-I", str(BT / "3rd-party/yxml")]
    exe = BUILD / "scenarios"
    subprocess.run([
        *flags, str(ROOT / "tests/ble_revalidation4_harness.c"),
        str(ROOT / "tests/ble_validation_notification.c"),
        str(ROOT / "hogp.c"), str(ROOT / "hidkeys.c"),
        str(BT / "src/btstack_util.c"), str(BT / "src/btstack_linked_list.c"),
        str(BT / "src/ble/le_device_db_tlv.c"),
        str(BT / "src/classic/btstack_link_key_db_tlv.c"),
        str(BT / "platform/embedded/btstack_tlv_flash_bank.c"),
        "-Wl,--gc-sections", "-o", str(exe),
    ], check=True)
    return exe


def main():
    exe = build_harness()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    names = subprocess.check_output([str(exe), "--list"], text=True).splitlines()
    failed = []
    for name in names:
        result = subprocess.run([str(exe), name], capture_output=True, text=True)
        print(f"{'FAIL' if result.returncode else 'PASS'} {name}")
        print(result.stdout + result.stderr, end="")
        if result.returncode:
            failed.append(name)
    print(f"BLE fourth-pass revalidation: {len(names) - len(failed)} passed, {len(failed)} failed")
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
