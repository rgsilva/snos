#!/usr/bin/env python3
"""Sixth-pass lifetime and deterministic foreground/NMI interleavings.

Run actual Pico link.c and SNES net.c. Queue cases compile a temporary copy
of net.c with explicit interrupt cut points; production files are untouched.
The middle-of-expression cut matches cc65816 5.17's -O0 listing: load head,
INC A, AND #511, STA head. This is a scheduling model, not an emulator.
Failures are not expected/skipped: each scenario is a strict regression.
"""
import pathlib
import resource
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
build = root / "pico/build/revalidation6_test"
build.mkdir(parents=True, exist_ok=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
common = ["cc", "-std=c11", "-g", "-O0", "-Wall", "-Wextra", "-Werror",
          "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
console_flags = ["-Wno-pointer-to-int-cast", "-Wno-unused-function",
                 "-Wno-unknown-pragmas", "-I", str(root / "snes/kernel")]
peer = build / "peer.o"
subprocess.run([*common, *console_flags, "-c",
                str(root / "pico/tests/revalidation4_console_peer.c"),
                "-o", str(peer)], check=True)
link_exe = build / "link"
subprocess.run([*common, str(root / "pico/link.c"),
                str(root / "pico/tests/revalidation6_link_harness.c"),
                str(peer), "-o", str(link_exe)], check=True)

source = (root / "snes/kernel/net.c").read_text()
original = '''  txq[txq_head] = b;
  txq_head = (uint16_t)((txq_head + 1) & (TXQ_SIZE - 1));'''
instrumented = '''  interrupt_cut(0);
  txq[txq_head] = b;
  interrupt_cut(1);
  uint16_t next_head = (uint16_t)((txq_head + 1) & (TXQ_SIZE - 1));
  interrupt_cut(2);
  txq_head = next_head;
  interrupt_cut(3);'''
assert source.count(original) == 1, "Review cut points against updated txq_put/codegen"
source = source.replace(original, instrumented)
# Model the hardware NMI enable bit; an implementation that guards entire
# records via these macros prevents the injected interrupt from firing.
source = source.replace('#define NET_NMI_OFF() (NET_R8(0x4200) = 0x01)',
                        '#define NET_NMI_OFF() (nmi_enabled = 0)')
source = source.replace('#define NET_NMI_ON() (NET_R8(0x4200) = 0x81)',
                        '#define NET_NMI_ON() (nmi_enabled = 1)')
(build / "net_instrumented.c").write_text(source)
fixture = (root / "pico/tests/revalidation3_console_peer.c").read_text()
fixture = fixture.replace('"../../snes/kernel/net.c"', '"net_instrumented.c"')
(build / "console_peer.c").write_text(fixture)
queue_exe = build / "queue"
subprocess.run([*common, *console_flags, "-I", str(build),
                str(root / "pico/tests/revalidation6_queue_harness.c"),
                "-o", str(queue_exe)], check=True)
failures = total = 0
for exe in (link_exe, queue_exe):
    names = subprocess.check_output([str(exe), "--list"], text=True).splitlines()
    for name in names:
        total += 1
        result = subprocess.run([str(exe), name], capture_output=True, text=True)
        print(result.stdout, end="")
        if result.returncode:
            failures += 1
            print(f"FAIL {name}: {result.stderr.strip()}")
        elif result.stderr:
            print(result.stderr, end="")
print(f"revalidation6 total: {total - failures} pass, {failures} fail, {total} total")
sys.exit(bool(failures))
