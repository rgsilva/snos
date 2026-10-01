#!/usr/bin/env python3
"""Link windows: exactly-once, in-order delivery under loss, at every window.

The real console net.c (console_host.py) against the real pico/link.c and
ctrl.c, through window_harness.c's faulty channel: frames lost, corrupted,
repeated, skipped and delayed both ways, and a console that stops reading.
Each pair of windows runs with several seeds, plus one clean run, which also
asserts that a window of 1 on both ends moves data at all. Failures are
strict.
"""
import subprocess
import sys

from console_host import ROOT, console_objects

build = ROOT / "pico/build/window_test"
objects = console_objects(build)
host = ["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "pico")]
for src in (ROOT / "pico/link.c", ROOT / "pico/ctrl.c", ROOT / "pico/tests/window_harness.c"):
    obj = build / (src.stem + ".pico.o")
    subprocess.run([*host, "-c", str(src), "-o", str(obj)], check=True)
    objects.append(obj)
exe = build / "window"
subprocess.run(["cc", *map(str, objects), "-o", str(exe)], check=True)

failed = 0
runs = 0
for wc in (1, 2, 4, 8):
    for wp in (1, 2, 4, 8):
        for seed in (["clean"] + [str(s) for s in (1, 2, 3)]):
            args = [str(exe), str(wc), str(wp), "7" if seed == "clean" else seed]
            if seed == "clean":
                args.append("clean")
            r = subprocess.run(args, capture_output=True, text=True, timeout=120)
            runs += 1
            if r.returncode:
                failed += 1
                print(f"FAIL windows {wc}/{wp} seed {seed}: {r.stderr.strip() or r.stdout.strip()}")
            elif seed in ("clean", "1"):
                print(r.stdout.strip() + (" (clean)" if seed == "clean" else ""))
print(f"window: {runs - failed}/{runs} runs delivered every byte exactly once, in order")
sys.exit(bool(failed))
