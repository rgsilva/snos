#!/usr/bin/env python3
"""Complete the 1..8 window matrix: the original tests cover powers of two.

First build and run that suite unchanged; add the missing 48 pairs with
its same clean run and three fault seeds (256 total runs, not 64 pairs).
"""
import subprocess
import sys
import pathlib
root = pathlib.Path(__file__).resolve().parents[2]
subprocess.run([sys.executable, str(root / 'pico/tests/window_test.py')], check=True)
exe = root / 'pico/build/window_test/window'
failed = 0
runs = 64
for console in range(1, 9):
    for pico in range(1, 9):
        if console in (1, 2, 4, 8) and pico in (1, 2, 4, 8):
            continue
        for seed in ('clean', '1', '2', '3'):
            args = [str(exe), str(console), str(pico),
                    '7' if seed == 'clean' else seed]
            if seed == 'clean':
                args.append('clean')
            result = subprocess.run(args, capture_output=True, text=True, timeout=120)
            runs += 1
            if result.returncode:
                failed += 1
                print(f'FAIL {console}/{pico}, seed {seed}: {result.stderr or result.stdout}')
print(f'All windows: {runs - failed}/{runs} runs passed over all 64 pairs')
sys.exit(bool(failed))
