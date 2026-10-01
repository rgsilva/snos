"""The console's link and IP code, compiled for the host.

net.c and ip.c reach the S-CPU's $4200 through a raw-pointer macro, which a
host cannot dereference; console_objects() copies those two files and
rewrites exactly that one #define in each to go through rig_reg(), and
refuses if the copy differs from the source in any other line. Everything
else compiles unchanged. The caller provides the hardware boundary: the
netasm.s routines, link_serial_xfer, the mailbox backend stubs, rig_reg,
rig_in_windex/rig_in_rindex, snos_frames and the scheduler hooks.
"""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
KERNEL = ROOT / "snes/kernel"
REG = {
    "net.c": ("#define NET_R8(a) (*(volatile uint8_t *)(a))",
              "#define NET_R8(a) (*rig_reg(a))"),
    "ip.c": ("#define R8(a) (*(volatile uint8_t *)(a))",
             "#define R8(a) (*rig_reg(a))"),
}
HEADER = '''
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
'''


def console_objects(build):
    """Object files for net.c, ip.c, link.c, link_serial.c and uIP (LINK_HW)."""
    build.mkdir(parents=True, exist_ok=True)
    (build / "rig_console.h").write_text(HEADER)
    sources = []
    for name in ("net.c", "ip.c"):
        text = (KERNEL / name).read_text()
        old, new = REG[name]
        if text.count(old) != 1:
            raise SystemExit(f"console_host: {name}: expected exactly one '{old}'")
        copy = text.replace(old, new)
        a, b = text.splitlines(), copy.splitlines()
        if len(a) != len(b) or sum(x != y for x, y in zip(a, b)) != 1:
            raise SystemExit(f"console_host: {name}: copy differs beyond the register macro")
        # Quoted includes resolve against the including file's directory
        # first, so the copy needs the kernel directory on the path instead.
        (build / name).write_text(copy)
        sources.append(build / name)
    sources += [KERNEL / "link.c", KERNEL / "link_serial.c", KERNEL / "uip/uip.c"]
    flags = ["cc", "-std=gnu11", "-g", "-O1", "-DLINK_HW=1", "-D__far=", "-D__huge=",
             "-Dlink_init=snes_link_init", "-include", str(build / "rig_console.h"),
             "-I", str(KERNEL), "-I", str(KERNEL / "uip"), "-w", "-c"]
    objects = []
    for src in sources:
        obj = build / (src.stem + ".console.o")
        subprocess.run([*flags, str(src), "-o", str(obj)], check=True)
        objects.append(obj)
    return objects
