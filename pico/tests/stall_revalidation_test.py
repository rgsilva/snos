#!/usr/bin/env python3
"""Pin recovery when the revised bench actually overflows/evicts frames.

The existing linkbench_test covers defaults, not the newly introduced stall
model. These assertions concern this model's recovery, not physical timing.
"""
import pathlib
import re
import subprocess
import sys

bench = pathlib.Path(__file__).with_name("linkbench.py")
for stall_us, period_ms, expect_overflow in ((20000, 200, False),
                                           (50000, 500, True),
                                           (50000, 100, True)):
    result = subprocess.run([sys.executable, str(bench), f"pico_stall_us={stall_us}",
                             f"pico_stall_every_ms={period_ms}"],
                            capture_output=True, text=True, timeout=120)
    out = result.stdout
    assert result.returncode == 0, out + result.stderr
    rates = re.findall(r"^(?:download|upload|both download|both upload): (\d+) B/s", out, re.M)
    assert len(rates) == 4 and all(int(rate) > 0 for rate in rates), out
    assert "bad sums 0" in out and "refused 0" in out and "desync 0" in out, out
    assert re.search(r"nmi late 0 .*masked across vblank 0, nmi spans 0, autoread collisions 0", out), out
    lost = int(re.search(r"(\d+) frames lost to a full ring", out)[1])
    evicted = int(re.search(r"(\d+) replies evicted from the queue", out)[1])
    high = int(re.search(r"at most (\d+) waiting", out)[1])
    assert (lost > 0) == expect_overflow and evicted > 0 and high <= 4, out
    print(f"PASS stall={stall_us}us/{period_ms}ms: rates={rates}, RX lost={lost}, evicted={evicted}")
print("stall revalidation: 3/3 cases passed")
