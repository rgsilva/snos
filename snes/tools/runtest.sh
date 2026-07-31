#!/bin/sh
# Run one Mesen test, with the controller borrowed for the duration.
#
# Mesen's SNES port 1 is normally left empty here, which makes emu.setInput a
# no-op and every controller-driven test fail for a reason that looks nothing
# like the cause. Rather than rely on anyone remembering, plug it in, run, and
# unplug — including when the test fails, which is exactly when a stray
# early exit would leave the setting changed behind your back.
set -u
MESEN="$1"; ROM="$2"; SCRIPT="$3"

# Ctrl-C between set and clear would strand the controller plugged in —
# the exact config drift the borrow/return scheme exists to prevent. The
# trap covers signals; the explicit clear below covers normal exits.
trap 'python3 tools/controller.py clear; exit 130' INT TERM

python3 tools/controller.py set
"$MESEN" --testrunner "$ROM" "$SCRIPT"
rc=$?
python3 tools/controller.py clear
exit $rc
