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

    port1 = cfg.get("Snes", {}).get("Port1", {}).get("Type")
    if port1 in (None, "None"):
        print(
            "ERROR: Mesen has no controller in SNES port 1 "
            f"(Snes.Port1.Type = {port1}).\n"
            "       emu.setInput then has nothing to set, $4218 reads zero, and\n"
            "       every controller-driven test fails claiming the on-screen\n"
            "       keyboard is broken. It is not.\n"
            "       Fix: Mesen -> Settings -> Controllers -> Player 1 -> "
            "SNES Controller.",
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
