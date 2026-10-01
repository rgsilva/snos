"""The console's kick, through the assembled PIO program (wire_test's model).

kernel/linkxfer.s link_serial_kick clocks port 2 eight times, ~126 master
cycles (~5.9 us) apart, with LATCH low and no data: a short burst like the
auto-joypad read's. The adapter must take it as one: eight bits in, one RX
byte that no frame will ever complete, and the gap interrupt -- which is what
makes port.c restart and stage its answer -- 64 us after the last clock, long
before the exchange's own first clock ~300 us later. Build the firmware first.
"""
import contextlib
import io

with contextlib.redirect_stdout(io.StringIO()):
    import wire_test  # runs its own checks on import; their output is its own

PIO_HZ = 25_000_000
period = round(126 / 21_477_270 * PIO_HZ)  # PIO cycles between the kick's clocks
tx = wire_test.bits(bytes(64))
for phase in range(40):
    received, sampled, partial, gap, _ = wire_test.run([0] * 8, tx, period, phase)
    assert received == [0] and partial == 0, (received, partial)
    last_clock = 100 + phase + 7 * period
    wait = (gap - last_clock) / PIO_HZ * 1e6
    assert 63.5 < wait < 65.5, wait
# The kick's own data is whatever the adapter had staged: the console drops it.
assert len(sampled) == 8
print(f"kick: eight clocks {period} PIO cycles apart, one byte, gap {wait:.2f} us later passed")
