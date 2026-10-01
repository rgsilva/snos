#!/usr/bin/env python3
"""Independent speed-peer validation. Regressions are strict failures, never xfail.

The console binary includes the actual apps/speedtest/speedtest.c unchanged;
only the syscall, stream and clock boundaries are simulated. This does not
claim to emulate the 65816, uIP or controller-port timing.
"""
import os
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
build = root / "pico/build/speed_validation_test"
build.mkdir(parents=True, exist_ok=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
common = ["cc", "-std=c11", "-g", "-O2", "-Wall", "-Wextra", "-Werror"]
core = root / "pico/nat_core.c"
failed = []
passed = 0


def run(exe, cases):
    global passed
    for case in cases:
        result = subprocess.run([str(exe), case], capture_output=True, text=True)
        print(result.stdout, end="")
        if result.returncode:
            failed.append(case)
            print(f"FAIL {case}: {result.stderr.strip()}")
        else:
            passed += 1


exe = build / "core"
subprocess.run([*common, str(core), str(root / "pico/tests/speed_validation_harness.c"),
                "-o", str(exe)], check=True)
run(exe, subprocess.check_output([str(exe), "--list"], text=True).splitlines())

exe = build / "console"
subprocess.run([*common, "-I", str(root / "snes/kernel"), "-I", str(root / "apps/lib"),
                str(core), str(root / "pico/tests/speed_console_harness.c"),
                "-o", str(exe)], check=True)
run(exe, ["1", "10", "120"])

# Reuse the established SDK test's tiny platform headers; do not edit them or
# any existing test. Run it as well so this new runner works on a clean tree.
subprocess.run([sys.executable, str(root / "pico/tests/nat_lwip_test.py")], check=True,
               stdout=subprocess.DEVNULL)
sdk = pathlib.Path(os.environ.get("PICO_SDK_PATH", "/home/ricardo/pico/pico-sdk"))
lwip = sdk / "lib/lwip/src"
sources = sorted(lwip.glob("core/*.c")) + sorted(lwip.glob("core/ipv4/*.c"))
sources += [lwip / "netif/ethernet.c", core, root / "pico/tests/speed_adapter_harness.c"]
exe = build / "adapter"
subprocess.run([*common, "-ftrivial-auto-var-init=pattern",
                "-I", str(root / "pico/build/nat_lwip_test"), "-I", str(root / "pico"),
                "-I", str(lwip / "include"), *map(str, sources), "-o", str(exe)], check=True)
run(exe, subprocess.check_output([str(exe), "--list"], text=True).splitlines())
print(f"speed validation: {passed} passed, {len(failed)} failed")
sys.exit(bool(failed))
