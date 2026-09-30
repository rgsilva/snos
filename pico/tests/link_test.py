"""Differential test against the repository's Gateway, at the hardware size."""
import contextlib
import ctypes as C
import io
import pathlib
import random
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "snes/tools"))
import gateway as gw

gw.FRAME = 64
BUILD = ROOT / "pico/build"
BUILD.mkdir(exist_ok=True)
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared",
                "-fPIC", str(ROOT / "pico/link.c"), "-o", str(BUILD / "link_test.so")], check=True)
lib = C.CDLL(str(BUILD / "link_test.so"))
Byte = C.c_uint8
Frame = Byte * 64


class Record(C.Structure):
    _fields_ = [("ch", Byte), ("len", Byte), ("data", C.POINTER(Byte))]


class Rx(C.Structure):
    _fields_ = [("records", Record * 30), ("count", C.c_size_t), ("reset", C.c_bool)]


lib.link_on_rx.argtypes = [C.POINTER(Byte)]
lib.link_on_rx.restype = Rx
lib.link_push.argtypes = [Byte, C.POINTER(Byte), C.c_size_t]
lib.link_push.restype = C.c_bool
lib.link_push_hello.argtypes = [C.POINTER(Byte), C.c_size_t]
lib.link_push_hello.restype = C.c_bool
lib.link_queued.argtypes = [Byte]
lib.link_queued.restype = C.c_size_t


class Peer(gw.Gateway):
    def deliver(self, f):
        self.received = [(f[o], bytes(f[o + 2:o + 2 + f[o + 1]]))
                         for o in self._record_offsets(f)]


rng = random.Random(2310)
p = Peer()
lib.link_init()
try:
    with contextlib.redirect_stdout(io.StringIO()):
        for step in range(10000):
            if rng.random() < .5:
                ch = rng.choice([0, 0x10, 0x11])
                data = rng.randbytes(rng.randrange(1, 1501))
                raw = (Byte * len(data)).from_buffer_copy(data)
                if lib.link_push(ch, raw, len(data)):
                    if ch == 0x10:
                        p.push_ip(data)
                    else:
                        p.push(ch, data)
            if rng.random() < .1:
                payload = bytes([2]) + b"192.168.1.100"
                data = bytes([0x84, len(payload)]) + payload
                if lib.link_push_hello((Byte * len(data)).from_buffer_copy(data), len(data)):
                    p.push_hello(payload)
            f = bytearray(rng.randbytes(64))
            f[0] = rng.choice([p.rx_next, (p.rx_next - 1) & 255, rng.randrange(256)])
            f[1] = rng.choice([p.tx_seq, (p.tx_seq - 1) & 255])
            f[2] = rng.choice([0, 1, 1, 2, 3])
            if rng.random() < .8:
                f[4:] = bytes([0x11, 3, 97, 98, 99, 0xff]) + bytes([0xff]) * 54
            f[3] = gw.checksum(f)
            if rng.random() < .1:
                f[3] ^= 1
            p.received = []
            p.on_frame(f)
            frame = Frame.from_buffer_copy(f)
            received = lib.link_on_rx(frame)
            assert [(r.ch, bytes(r.data[:r.len])) for r in received.records[:received.count]] == p.received, step
            out = Frame()
            lib.link_next_out(out)
            assert bytes(out) == bytes(p.next_out()), step
            for ch in (0, 0x10, 0x11):
                assert lib.link_queued(ch) == sum(len(d) for c, d in p.out if c == ch), step
finally:
    p.teardown()
print("link: 10000 differential exchanges passed (SYN, ACK, checksum, queue, HELLO, records)")
