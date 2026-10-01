#!/usr/bin/env python3
"""Inline test/lib.lua into a test script.

Mesen's Lua sandbox disables `dofile` and `require` ("I/O functions are disabled
by default for security"), so a test cannot pull in a shared helper module at
runtime. Rather than copy-paste the mailbox/VRAM helpers into every test, the
build splices them in: the line

    local L = dofile("test/lib.lua")

is replaced by the whole library wrapped in an immediately-invoked function, so
`L` ends up bound to exactly what lib.lua returns. Test sources stay readable
and there is still one copy of the harness.

The same splice serves the other shared modules -- test/gwip.lua (the v2
gateway + IP/TCP peer) and test/hwport.lua (the port-2 adapter model) --
through optional lines of the same shape:

    local G = dofile("test/gwip.lua")

Usage: mktest.py <test.lua> <lib.lua> <out.lua>
"""
import os
import re
import sys

MARKER = re.compile(r'^\s*local\s+L\s*=\s*dofile\(["\']test/lib\.lua["\']\)\s*$')
MODULE_MARKER = re.compile(
    r'^\s*local\s+(\w+)\s*=\s*dofile\(["\']test/(\w+)\.lua["\']\)\s*$')


def main(argv):
    test_path, lib_path, out_path = argv
    lib = open(lib_path).read()
    test_dir = os.path.dirname(lib_path)
    lines = open(test_path).read().splitlines()

    out, spliced = [], False
    for line in lines:
        if MARKER.match(line):
            out.append("-- >>> inlined from test/lib.lua by tools/mktest.py")
            out.append("local L = (function()")
            out.append(lib)
            out.append("end)()")
            out.append("-- <<< end of inlined library")
            spliced = True
        elif MODULE_MARKER.match(line):
            name, module = MODULE_MARKER.match(line).groups()
            out.append(f"-- >>> inlined from test/{module}.lua by tools/mktest.py")
            out.append(f"local {name} = (function()")
            out.append(open(os.path.join(test_dir, module + ".lua")).read())
            out.append("end)()")
            out.append("-- <<< end of inlined library")
        else:
            out.append(line)

    if not spliced:
        raise SystemExit(
            f"{test_path}: no `local L = dofile(\"test/lib.lua\")` line found. "
            "Every test must pull the harness in through that exact line so "
            "the build can splice it."
        )

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    open(out_path, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
