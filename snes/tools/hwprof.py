#!/usr/bin/env python3
"""Entry and return addresses of kernel C functions, as a Lua table.

    hwprof.py <build-dir> name [name...]  ->  { name = {entry, rtl, rtl...}, ... }

A name with no listing (a variable) comes out as { address } alone.

For test/hwspeed.lua's profiler (CONFIG.prof): Mesen exec callbacks at a
function's first instruction and at each of its `rtl`s time it in master
cycles without touching the kernel. Addresses come from the link map (one
section per function, so the listing's offsets are relative to it) and the
`rtl` offsets from the function's listing. Stale the moment the kernel is
rebuilt, which is why the Makefile regenerates it on every run.
"""
import pathlib
import re
import sys

build = pathlib.Path(sys.argv[1])
names = sys.argv[2:]
placed = {}
for m in re.finditer(r"^(\w+) in section '\w+'\s+placed at address ([0-9a-f]{6})",
                     (build / "snos.map").read_text(), re.M):
    placed[m.group(1)] = int(m.group(2), 16)
label = re.compile(r"^\s*\\ ([0-9a-f]{6})(?: [0-9a-f.]+)?\s+(\w+):")
ret = re.compile(r"^\s*\\ ([0-9a-f]{6}) 6b\s+rtl\b")
rtls = {}
for lst in build.glob("*.lst"):
    current = None
    for line in lst.read_text(errors="replace").splitlines():
        m = label.match(line)
        if m and int(m.group(1), 16) == 0:
            current = m.group(2) if m.group(2) in names else None
            if current:
                rtls[current] = []
            continue
        if current and (m := ret.match(line)):
            rtls[current].append(int(m.group(1), 16))
out = []
for name in names:
    if name in placed and not rtls.get(name):
        # A variable: one address, read by the profiler at a function's entry.
        out.append(f"{name} = {{{hex(placed[name])}}}")
        continue
    if name not in placed or not rtls.get(name):
        sys.exit(f"hwprof: {name} not found in {build}")
    base = placed[name]
    addrs = [base] + [base + off for off in rtls[name]]
    out.append(f"{name} = {{{', '.join(hex(a) for a in addrs)}}}")
print("{" + ", ".join(out) + "}")
