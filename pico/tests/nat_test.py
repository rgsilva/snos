#!/usr/bin/env python3
"""Core scenarios, including named regressions that currently fail. Run all of them."""
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
out = root / "pico/build/nat_test"
out.parent.mkdir(parents=True, exist_ok=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(root / "pico/nat_core.c"), str(root / "pico/tests/nat_harness.c"),
                "-o", str(out)], check=True)
failed = []
for name in subprocess.check_output([str(out), "--list"], text=True).splitlines():
    result = subprocess.run([str(out), name], capture_output=True, text=True)
    print(result.stdout, end="")
    if result.returncode:
        failed.append(name)
        print(f"FAIL {name}: {result.stderr.strip()}")
print(f"nat core: {len(failed)} failing scenarios")
sys.exit(bool(failed))
