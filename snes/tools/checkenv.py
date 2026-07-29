#!/usr/bin/env python3
"""Pre-flight check on Mesen's configuration before running the test suite.

This exists because a misconfigured emulator produces failures that look exactly
like code defects, and it has now cost time twice. With no controller plugged
into SNES port 1, `emu.setInput` has nothing to set, `$4218` reads zero, and
every controller-driven test fails complaining that the on-screen keyboard is
broken. It is not; the button press never existed.

Two people independently misdiagnosed that as a collision with a running Mesen
GUI. That theory was WRONG: the full suite, controller tests included, passes
with a GUI open once a controller is actually configured. The GUI was innocent
both times and the missing controller was the whole story, so the GUI check is
now only a note.

Recorded because it is the interesting failure here: a plausible diagnosis that
sits adjacent to the truth is worse than none at all. It explained the symptom,
it had corroborating prior art in emu8086, and it survived two independent
investigations before the evidence finally contradicted it.

The GUI does have one real effect: exiting it rewrites settings.json, and that
has twice reset Snes.Port1.Type to None — which is how the actual cause kept
coming back.
"""
import json
import os
import pathlib
import sys

SETTINGS = pathlib.Path.home() / "Library/Application Support/Mesen2/settings.json"


def main() -> int:
    if not SETTINGS.exists():
        return 0  # nothing to check against; let the tests speak for themselves

    try:
        cfg = json.loads(SETTINGS.read_text(encoding="utf-8-sig"))
    except Exception as exc:
        print(f"warning: could not read Mesen settings ({exc})", file=sys.stderr)
        return 0

    # The controller is no longer checked here. tools/runtest.sh borrows it for
    # the duration of each run and gives it back afterwards, because Ricardo
    # deliberately keeps port 1 empty and exiting the Mesen GUI rewrites the
    # setting anyway — asking a human to remember lost that race three times.
    #
    # What is worth failing on is being unable to change it at all, since then
    # the tests would silently run without input and blame the keyboard.
    if not os.access(SETTINGS, os.W_OK):
        print(
            f"ERROR: cannot write {SETTINGS}.\n"
            "       The harness needs to plug a controller into SNES port 1 for\n"
            "       the duration of a run; without one, emu.setInput does\n"
            "       nothing and controller tests fail blaming the keyboard.",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
