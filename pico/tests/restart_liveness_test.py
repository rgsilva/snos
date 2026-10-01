#!/usr/bin/env python3
"""Second-pass recovery through actual ip_service and actual net_reset."""
import subprocess
import resource
from console_host import ROOT, KERNEL, console_objects

build = ROOT / "pico/build/restart_liveness_test"
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
objects = console_objects(build)
flags = ["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "pico")]
for name in ("link.c", "ctrl.c", "nat_core.c", "tests/restart_liveness_harness.c"):
    source = ROOT / "pico" / name
    obj = build / (source.stem + ".pico.o")
    subprocess.run([*flags, "-c", str(source), "-o", str(obj)], check=True)
    objects.append(obj)
obj = build / "app.o"
subprocess.run([*flags, "-Wno-unused-function", "-I", str(KERNEL), "-I",
                str(ROOT / "apps/lib"), "-c", str(ROOT / "pico/tests/linkbench_app.c"),
                "-o", str(obj)], check=True)
objects.append(obj)
exe = build / "liveness"
subprocess.run(["cc", "-Wl,--wrap=ctrl_address", *map(str, objects), "-o", str(exe)], check=True)
for window in (1, 4, 8):
    for scenario in ("adapter", "late-address", "net-reset"):
        result = subprocess.run([str(exe), f"window={window}", scenario],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout.splitlines()[-1])
print("restart liveness: 9/9 cases passed")
