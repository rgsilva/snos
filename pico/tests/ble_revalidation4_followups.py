#!/usr/bin/env python3
"""Nonblocking synthetic ordering probes, deliberately outside *_test.py.

Both are expected assertion failures on a7038a1. They model a stale HIDS
service event after replacement connection but before current HIDS startup,
and an old successful connection completion after cancellation/replacement.
Actual SDK/controller ordering must be established before treating either
as a normal-use defect. An unexpected pass exits nonzero so this list must
be updated when fixed; unrelated crashes also fail this runner.
"""
import resource
import signal
import subprocess
import sys

from ble_revalidation4_test import build_harness


def main():
    exe = build_harness()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    assertions = {
        "followup_service_before_current_hids_start":
            "!connected && !reports_seen && !target_store_pending",
        "followup_old_connect_after_replacement_request":
            "le_handle == HCI_CON_HANDLE_INVALID && !pairing_requests",
    }
    names = subprocess.check_output([str(exe), "--followups"], text=True).splitlines()
    assert set(names) == set(assertions)
    unexpected = 0
    for name in names:
        result = subprocess.run([str(exe), name], capture_output=True, text=True)
        expected = (result.returncode == -signal.SIGABRT
                    and assertions[name] in result.stderr)
        print(f"{'XFAIL (nonblocking)' if expected else 'UNEXPECTED'} {name}")
        print(result.stdout + result.stderr, end="")
        unexpected += not expected
    print(f"Follow-up probes: {len(names) - unexpected} expected failures, "
          f"{unexpected} unexpected outcomes")
    return bool(unexpected)


if __name__ == "__main__":
    sys.exit(main())
