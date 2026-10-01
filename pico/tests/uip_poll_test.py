#!/usr/bin/env python3
"""uIP's poll path must not send another connection's stale length.

See uip_poll_harness.c: uip.c as the console builds it (uip-conf.h), two
connections, a poll that sends followed by a poll that does not.
"""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
uip = root / "snes/kernel/uip"
build = root / "pico/build/uip_poll_test"
build.mkdir(parents=True, exist_ok=True)
exe = build / "uip_poll"
subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-w", "-I", str(uip),
                str(uip / "uip.c"), str(root / "pico/tests/uip_poll_harness.c"),
                "-o", str(exe)], check=True)
sys.exit(subprocess.run([str(exe)]).returncode)
