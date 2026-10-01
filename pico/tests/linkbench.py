#!/usr/bin/env python3
"""Link throughput bench: real console and Pico link code, modelled cable.

    python3 pico/tests/linkbench.py                 # the speedtest, defaults
    python3 pico/tests/linkbench.py raw             # IP stream only, no TCP
    python3 pico/tests/linkbench.py window=1 exch=1 pico_us=2000 seconds=5

Arguments are passed to the bench (see linkbench_harness.c's cost table).
The console's net.c and ip.c reach the S-CPU's $4200 through a raw-pointer
macro, which a host cannot dereference; the build copies those two files and
rewrites exactly that one #define in each, and refuses to build if the copy
differs from the source in any other line. See docs/link-throughput.md.
"""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
kernel = root / "snes/kernel"
build = root / "pico/build/linkbench"
build.mkdir(parents=True, exist_ok=True)

(build / "rig_console.h").write_text('''
#ifndef RIG_CONSOLE_H
#define RIG_CONSOLE_H
#include <stdint.h>
#define SNES_MAILBOX_H
extern volatile uint16_t rig_in_windex, rig_in_rindex;
#define MB_IN_RING_SIZE 512u
#define MB_IN_WINDEX rig_in_windex
#define MB_IN_RINDEX rig_in_rindex
volatile uint8_t *rig_reg(unsigned long a);
#endif
''')

REG = {
    "net.c": ("#define NET_R8(a) (*(volatile uint8_t *)(a))",
              "#define NET_R8(a) (*rig_reg(a))"),
    "ip.c": ("#define R8(a) (*(volatile uint8_t *)(a))",
             "#define R8(a) (*rig_reg(a))"),
}
sources = []
for name in ("net.c", "ip.c"):
    text = (kernel / name).read_text()
    old, new = REG[name]
    if text.count(old) != 1:
        sys.exit(f"linkbench: {name}: expected exactly one '{old}'")
    copy = text.replace(old, new)
    a, b = text.splitlines(), copy.splitlines()
    changed = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    if len(a) != len(b) or len(changed) != 1:
        sys.exit(f"linkbench: {name}: host copy differs in more than the register macro")
    # Keep the copy beside its siblings' headers: quoted includes resolve
    # against the including file's directory first.
    path = build / name
    path.write_text(copy)
    sources.append(path)
sources += [kernel / "link.c", kernel / "link_serial.c", kernel / "uip/uip.c"]

console_flags = ["cc", "-std=gnu11", "-g", "-O1", "-DLINK_HW=1", "-D__far=", "-D__huge=",
                 "-Dlink_init=snes_link_init", "-include", str(build / "rig_console.h"),
                 "-I", str(kernel), "-I", str(kernel / "uip"), "-w", "-c"]
objects = []
for src in sources:
    obj = build / (src.stem + ".console.o")
    subprocess.run([*console_flags, str(src), "-o", str(obj)], check=True)
    objects.append(obj)

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
