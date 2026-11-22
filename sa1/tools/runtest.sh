#!/bin/sh
# One Mesen --testrunner run, with port 1's controller plugged in for it
# (snes/tools/controller.py; a no-op where Mesen's settings are elsewhere).
set -u
MESEN="$1"; ROM="$2"; SCRIPT="$3"
CTL="$(dirname "$0")/../../snes/tools/controller.py"
trap 'python3 "$CTL" clear; exit 130' INT TERM
python3 "$CTL" set
"$MESEN" --testrunner "$ROM" "$SCRIPT"
rc=$?
python3 "$CTL" clear
exit $rc
