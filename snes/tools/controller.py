#!/usr/bin/env python3
"""Plug a controller into Mesen's SNES port 1, or unplug it again.

Ricardo runs the emulator with the controller disabled — that is a deliberate
choice about how he uses Mesen, not a mistake. But `emu.setInput` has nothing to
set when no controller exists, so `$4218` reads zero and every controller-driven
test fails claiming the on-screen keyboard is broken. That misdiagnosis has cost
this project real time twice, and the setting reverted three separate times
because exiting the GUI rewrites settings.json.

So the test harness borrows the controller for the duration of a run and gives
it back afterwards, rather than asking anyone to remember. `set` before, `clear`
after; see tools/runtest.sh.

Usage: controller.py set|clear
"""
import json
import pathlib
import sys

SETTINGS = pathlib.Path.home() / "Library/Application Support/Mesen2/settings.json"
PLUGGED = '"Type": "SnesController"'
UNPLUGGED = '"Type": "None"'


def swap(want_plugged: bool) -> int:
    if not SETTINGS.exists():
        return 0  # not this machine's problem

    text = SETTINGS.read_text(encoding="utf-8-sig")
    # Only the SNES section: every console in the file has a Port1.Type, and a
    # blind replace would rewire the NES and Game Boy too.
    try:
        start = text.index('"Snes"')
        end = text.index('"Nes"', start)
    except ValueError:
        return 0

    frm, to = (UNPLUGGED, PLUGGED) if want_plugged else (PLUGGED, UNPLUGGED)
    section = text[start:end]
    if frm not in section:
        return 0  # already in the wanted state
    text = text[:start] + section.replace(frm, to, 1) + text[end:]
    SETTINGS.write_text("﻿" + text, encoding="utf-8")
    return 0


def main(argv) -> int:
    if len(argv) != 1 or argv[0] not in ("set", "clear"):
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    return swap(argv[0] == "set")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
