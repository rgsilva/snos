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

The same splice serves test/gwip.lua (the shared v2 gateway + IP/TCP peer)
through the optional line

    local G = dofile("test/gwip.lua")

Usage: mktest.py <test.lua> <lib.lua> <out.lua>
"""
import os
import re
import sys

MARKER = re.compile(r'^\s*local\s+L\s*=\s*dofile\(["\']test/lib\.lua["\']\)\s*$')
GW_MARKER = re.compile(
    r'^\s*local\s+G\s*=\s*dofile\(["\']test/gwip\.lua["\']\)\s*$')


def main(argv):
    test_path, lib_path, out_path = argv
    lib = open(lib_path).read()
    gwip_path = os.path.join(os.path.dirname(lib_path), "gwip.lua")
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
        elif GW_MARKER.match(line):
            out.append("-- >>> inlined from test/gwip.lua by tools/mktest.py")
            out.append("local G = (function()")
            out.append(open(gwip_path).read())
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
