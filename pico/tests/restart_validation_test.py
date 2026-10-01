#!/usr/bin/env python3
"""Strict restart regression: same lwIP/console build as transition properties.

Current implementation fails: old datagram suffix poisons the new stream.
Retain the legacy run to distinguish inheritance from a window regression.
"""
import os
import pathlib
import subprocess
import sys
runner = pathlib.Path(__file__).with_name('transition_test.py')
failed = 0
for window in (0, 1, 4, 8):
    print(f"Restart regression: {'legacy' if not window else f'current window {window}'}", flush=True)
    env = dict(os.environ, TRANSITION_RESTART_TEST='1', TRANSITION_CURRENT_WINDOW=str(window))
    failed += subprocess.run([sys.executable, str(runner)], env=env).returncode != 0
sys.exit(bool(failed))
