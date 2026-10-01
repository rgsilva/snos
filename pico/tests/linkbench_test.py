#!/usr/bin/env python3
"""The link bench as a regression check (linkbench.py, docs/link-throughput.md).

Deterministic, so the bounds are loose only to leave room for deliberate
cost-table changes, not for noise:

- the speedtest at the defaults moves every phase both ways at well over the
  stop-and-wait link's ceiling, with no hazard and no checksum failure -- a
  stack bug that stalls one direction of the duplex phase (uIP sending a
  stale segment on a connection that had nothing to say did exactly that:
  146 B/s) fails here;
- the link alone, both directions saturated, does what its arithmetic says:
  58 bytes a fresh frame, every exchange fresh with a window, one in two at
  window 1, 1.5 exchanges a frame at the defaults.
"""
import re
import subprocess
import sys
import pathlib

here = pathlib.Path(__file__).resolve().parent


def bench(*args):
    r = subprocess.run([sys.executable, str(here / "linkbench.py"), *args],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, r.stderr or r.stdout
    return r.stdout


out = bench()
rates = dict(re.findall(r"^(download|upload|both download|both upload): (\d+) B/s", out, re.M))
assert set(rates) == {"download", "upload", "both download", "both upload"}, out
for label, rate in rates.items():
    assert int(rate) > 1800, f"{label} {rate} B/s"
assert re.search(r"nmi late 0 .*masked across vblank 0, nmi spans 0, autoread collisions 0", out), out
assert "bad sums 0" in out and "refused 0" in out and "desync 0" in out, out


def raw(*args):
    m = re.search(r"raw IP stream: down (\d+) B/s, up (\d+) B/s", bench("raw", *args))
    return int(m.group(1)), int(m.group(2))


for args, low, high in (((), 5000, 5300), (("exch=1",), 3400, 3520),
                        (("window=1", "exch=1"), 1700, 1760)):
    down, up = raw(*args)
    assert low <= down <= high and low <= up <= high, (args, down, up)
print(f"linkbench: speedtest {rates['download']} / {rates['upload']} / "
      f"{rates['both download']} + {rates['both upload']} B/s, no hazards; "
      "raw link at the arithmetic for windows 1 and 4, 1 and 2 exchanges a frame")
