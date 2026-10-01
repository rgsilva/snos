#!/usr/bin/env python3
"""Link throughput bench: real console and Pico link code, modelled cable.

    python3 pico/tests/linkbench.py                 # the speedtest, defaults
    python3 pico/tests/linkbench.py raw             # IP stream only, no TCP
    python3 pico/tests/linkbench.py window=1 exch=1 pico_us=2000 seconds=5

Arguments are passed to the bench (see linkbench_harness.c's cost table).
The console code is built by console_host.py. See docs/link-throughput.md.
"""
import subprocess
import sys

from console_host import ROOT as root, KERNEL as kernel, console_objects

build = root / "pico/build/linkbench"
objects = console_objects(build)

host = ["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(root / "pico")]
for src in (root / "pico/link.c", root / "pico/ctrl.c", root / "pico/nat_core.c",
            root / "pico/tests/linkbench_harness.c"):
    obj = build / (src.stem + ".pico.o")
    subprocess.run([*host, "-c", str(src), "-o", str(obj)], check=True)
    objects.append(obj)
obj = build / "linkbench_app.o"
subprocess.run([*host, "-Wno-unused-function", "-I", str(kernel), "-I", str(root / "apps/lib"),
                "-c", str(root / "pico/tests/linkbench_app.c"),
                "-o", str(obj)], check=True)
objects.append(obj)
exe = build / "linkbench"
subprocess.run(["cc", *map(str, objects), "-o", str(exe)], check=True)
sys.exit(subprocess.run([str(exe), *sys.argv[1:]]).returncode)
