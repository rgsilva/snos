#!/usr/bin/env python3
"""Nonblocking adversarial probes, intentionally outside the *_test.py suite.

Exit 1 means a documented fairness limitation was reproduced. These schedules
force ACKs into every clear-link opportunity or omit normal busy polling for
an entire RTO. They are not evidence of a defect reachable in normal use.
"""
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
build = root / "pico/build/speed_validation_test"
build.mkdir(parents=True, exist_ok=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
exe = build / "followup"
subprocess.run(["cc", "-std=c11", "-g", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "pico/nat_core.c"),
                str(root / "pico/tests/speed_followup_harness.c"), "-o", str(exe)], check=True)
limitations = 0
for case in ["input-ack", "poll-ack", "sparse-busy"]:
    result = subprocess.run([str(exe), case], check=False)
    if result.returncode not in (0, 1):
        raise RuntimeError(f"probe {case} failed unexpectedly: {result.returncode}")
    limitations += result.returncode
print(f"follow-up probes: {limitations}/3 fairness limitations reproduced")
sys.exit(bool(limitations))
