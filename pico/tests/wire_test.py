"""Cycle model of the assembled PIO program; build the firmware first."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
header = (ROOT / "build/snoslink.pio.h").read_text()
body = header.split("snoslink_program_instructions[] = {", 1)[1].split("};", 1)[0]
code = [int(word, 16) for word in re.findall(r"0x([0-9a-f]{4})", body)]
wrap = int(re.search(r"#define snoslink_wrap (\d+)", header)[1])
wrap_target = int(re.search(r"#define snoslink_wrap_target (\d+)", header)[1])


def run(rx, tx, period, phase, low=12):
    pc = x = y = incoming = shifts = sent = data1 = 0
    received, sampled, polls = [], [], []
    first = 100 + phase
    end = first + len(rx) * period + 1700
    for cycle in range(end):
        elapsed = cycle - first
        bit, within = divmod(elapsed, period)
        active = 0 <= bit < len(rx)
        high = not (active and within < low)
        if active and within == 0:
            sampled.append(data1)
        word = code[pc]
        op, arg, value = word >> 13, (word >> 5) & 7, word & 31
        next_pc = wrap_target if pc == wrap else pc + 1
        if op == 0:  # JMP
            if arg == 6:
                polls.append(cycle)
                take = high
            elif arg == 2:
                take = x != 0
                x = (x - 1) & 0xffffffff
            elif arg == 4:
                take = y != 0
                y = (y - 1) & 0xffffffff
            else:
                assert arg == 0
                take = True
            if take:
                next_pc = value
        elif op == 1:  # WAIT 1 gpio 2 (0x2082) / WAIT 0 gpio 2 (0x2002)
            assert word in (0x2082, 0x2002)
            want_high = word == 0x2082
            if high != want_high:
                next_pc = pc
        elif op == 2:  # IN
            assert arg == 0 and value == 1 and active
            incoming = (incoming << 1) | rx[bit]
            shifts += 1
            if shifts == 8:
                received.append(incoming)
                incoming = shifts = 0
        elif op == 3:  # OUT, byte autopull
            assert arg == 0 and value == 1
            if sent == len(tx):
                # RX completion restarts the real SM at the exhausted TX FIFO.
                assert len(sampled) == len(rx)
                return received, sampled, shifts, cycle, polls
            data1 = tx[sent]
            sent += 1
        elif op == 6:  # IRQ WAIT
            assert word == 0xc020
            return received, sampled, shifts, cycle, polls
        elif op == 7:  # SET
            if arg == 1:
                x = value
            else:
                assert arg == 2
                y = value
        else:
            raise AssertionError(hex(word))
        pc = next_pc
    raise AssertionError("no completion or gap")


def bits(data):
    return [(byte >> shift) & 1 for byte in data for shift in range(7, -1, -1)]


tx = bytes([0xa5]) + bytes(ord("ABCDEFGHIJ"[i % 10]) for i in range(63))
rx = bytes([0x59]) + bytes(ord("1234567890"[i % 10]) for i in range(63))
for phase in range(50):
    received, sampled, partial, _, _ = run(bits(rx), bits(tx), 200, phase)
    assert bytes(received) == rx and sampled == bits(tx) and partial == 0
    received, _, partial, _, _ = run([1] * 16, bits(tx), 300, phase)
    assert received == [255, 255] and partial == 0
for count in range(1, 16):
    received, _, partial, _, _ = run([0] * count, bits(tx), 200, 0)
    assert len(received) * 8 + partial == count
# Idle: parked at `start`, no clock ever comes, and no gap may fire.
try:
    run([], bits(tx), 200, 0)
    raise AssertionError("idle must not time out")
except AssertionError as e:
    assert str(e) == "no completion or gap"
# One clock then silence: the countdown runs and the gap fires 64.08 us later.
received, _, partial, gap, polls = run([1], bits(tx), 200, 0)
assert not received and partial == 1
assert gap - 100 == 1616, gap  # 12-cycle pulse, in/out, then the 1602-cycle countdown
assert max(b - a for a, b in zip(polls, polls[1:])) == 4
print("wire: 50 pulse phases, MSB TX/RX, joypad, partial bursts, idle, 64.08 us gap passed")
