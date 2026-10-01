#!/usr/bin/env python3
"""The restarted receiver's IP hold, in pico/link.c and tools/gateway.py.

link_test.py holds the two to the same records frame for frame, but its
random frames rarely carry IP while the hold is on. This scripts it: after a
restart under a console that did not restart (its first frame has no F_SYN),
IP records are dropped -- control is not -- until a frame AFTER the first
acks inside the receiver's window; a console F_SYN ends the hold too; a
console that restarted alongside is never held. The gateway's own IP
assembler must stay empty while held.
"""
import contextlib
import ctypes as C
import io
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "snes/tools"))
import gateway as gw

gw.FRAME = 64
BUILD = ROOT / "pico/build"
BUILD.mkdir(exist_ok=True)
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                str(ROOT / "pico/link.c"), "-o", str(BUILD / "ip_hold_test.so")], check=True)
lib = C.CDLL(str(BUILD / "ip_hold_test.so"))
Byte = C.c_uint8


class Record(C.Structure):
    _fields_ = [("ch", Byte), ("len", Byte), ("data", C.POINTER(Byte))]


class Rx(C.Structure):
    _fields_ = [("records", Record * 30), ("count", C.c_size_t), ("reset", C.c_bool),
                ("valid", C.c_bool)]


lib.link_on_rx.argtypes = [C.POINTER(Byte)]
lib.link_on_rx.restype = Rx
lib.link_push.argtypes = [Byte, C.POINTER(Byte), C.c_size_t]
lib.link_push.restype = C.c_bool

CTRL, IP, KBD = 0, 0x10, 0x11


class Recorder(gw.Gateway):
    def deliver(self, f):
        self.received += [(f[o], bytes(f[o + 2:o + 2 + f[o + 1]]))
                          for o in self._record_offsets(f)]
        super().deliver(f)


def frame(seq, ack, flags, records):
    f = bytearray([gw.CH_PAD]) * 64
    f[0], f[1], f[2] = seq, ack, flags
    o = 4
    for ch, data in records:
        f[o], f[o + 1] = ch, len(data)
        f[o + 2:o + 2 + len(data)] = data
        o += 2 + len(data)
    f[3] = gw.checksum(f)
    return f


class Pair:
    """A fresh pico/link.c and a fresh Gateway, fed the same frames."""

    def __init__(self):
        lib.link_init()
        with contextlib.redirect_stdout(io.StringIO()):
            self.g = Recorder()
        self.g.received = []

    def feed(self, f):
        raw = (Byte * 64).from_buffer_copy(f)
        r = lib.link_on_rx(raw)
        pico = [(x.ch, bytes(x.data[:x.len])) for x in r.records[:r.count]]
        self.g.received = []
        with contextlib.redirect_stdout(io.StringIO()):
            self.g.on_frame(bytearray(f))
        assert pico == self.g.received, (pico, self.g.received)
        return [ch for ch, _ in pico]

    def close(self):
        self.g.teardown()


D, S = gw.F_DATA, gw.F_SYN
STALE = bytes([0x00, 0x02]) + bytes(range(40))      # the middle of a datagram
FRESH = bytes([60, 0]) + bytes([0x45]) + bytes(20)  # the start of one
checks = 0

# A console that lived on: its first frame is held whatever its ack says --
# 255 is the new receiver's base - 1 -- and so is one acking outside the window.
p = Pair()
assert p.feed(frame(40, 255, D, [(CTRL, b"\x04\x00"), (IP, STALE)])) == [CTRL]
assert p.feed(frame(41, 17, D, [(IP, STALE)])) == []
assert p.g.ipbuf == b""
# The next acking inside it (base - 1) is proof of adoption: IP flows again.
assert p.feed(frame(42, 255, D, [(KBD, b"k"), (IP, FRESH)])) == [KBD, IP]
assert p.g.ipbuf == FRESH
p.close()
checks += 1

# With a frame of ours in flight, acking it is inside the window too.
p = Pair()
data = (Byte * 1)(0x61)
assert lib.link_push(KBD, data, 1)
p.g.push(KBD, b"a")
out = (Byte * 64)()
lib.link_next_out(out)
assert bytes(out) == bytes(p.g.next_out())
assert p.feed(frame(7, 99, D, [(IP, STALE)])) == []
assert p.feed(frame(8, 0, D, [(IP, FRESH)])) == [IP]
p.close()
checks += 1

# The console restarting (F_SYN) ends the hold: its streams begin again.
p = Pair()
assert p.feed(frame(40, 3, D, [(IP, STALE)])) == []
assert p.feed(frame(0, 3, D | S, [(IP, FRESH)])) == [IP]
assert p.g.ipbuf == FRESH
p.close()
checks += 1

# A console that restarted alongside (F_SYN first) is never held.
p = Pair()
assert p.feed(frame(0, 3, D | S, [(IP, FRESH)])) == [IP]
p.close()
checks += 1

# A corrupt first frame is not a first frame.
p = Pair()
bad = frame(40, 255, D, [(IP, STALE)])
bad[3] ^= 1
assert p.feed(bad) == []
assert p.feed(frame(0, 3, D | S, [(IP, FRESH)])) == [IP]
p.close()
checks += 1

print(f"ip hold: {checks} restart cases, pico/link.c and gateway.py agree")
