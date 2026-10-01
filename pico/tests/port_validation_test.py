#!/usr/bin/env python3
"""Exercise real port.c queue/restart, both electrical encodings; SDK is mocked."""
import pathlib
import subprocess
root = pathlib.Path(__file__).resolve().parents[2]
build = root / 'pico/build/port_validation'
build.mkdir(parents=True, exist_ok=True)
for encoding in ([], ['-DDATA1_OPEN_DRAIN=1']):
    exe = build / ('od' if encoding else 'pp')
    subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-variable', *encoding,
                    '-I', str(root / 'pico/tests/port_mock'),
                    str(root / 'pico/tests/port_validation_harness.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
