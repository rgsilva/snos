#!/usr/bin/env python3
"""Run the C NAT core with a deterministic fake raw-API transport."""
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
out = root / "pico/build/nat_test"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(root / "pico/nat_core.c"), str(root / "pico/tests/nat_harness.c"),
                "-o", str(out)], check=True)
subprocess.run([str(out)], check=True)
