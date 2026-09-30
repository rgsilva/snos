"""Control stream fragmentation and relay payload layouts."""
import ctypes as C
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / "pico/build"
BUILD.mkdir(exist_ok=True)
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                str(ROOT / "pico/link.c"), str(ROOT / "pico/ctrl.c"),
                str(ROOT / "pico/tests/ctrl_stub.c"), "-o", str(BUILD / "ctrl_test.so")], check=True)
lib = C.CDLL(str(BUILD / "ctrl_test.so"))
Byte = C.c_uint8
Frame = Byte * 64


def raw(data):
    return (Byte * len(data)).from_buffer_copy(data)


def feed(data):
    for b in data:
        lib.ctrl_bytes(raw(bytes([b])), C.c_size_t(1))


def next_record():
    frame = Frame()
    lib.link_next_out(frame)
    return bytes(frame[6:6 + frame[5]]) if frame[2] & 1 else b""


lib.link_init()
lib.ctrl_reset()
feed(b"\x04\x00")
assert next_record() == b""  # HELLO waits for relay address.
lib.ctrl_address(raw(bytes([192, 168, 1, 123])))
lib.ctrl_poll()
assert next_record() == b"\x84\x0e\x02192.168.1.123"
for verb, typ in ((0x0b, 2), (0x0c, 3)):
    feed(bytes([verb, 3, 7, 0x90, 0x1f]))
    assert Byte.in_dll(lib, "sent_type").value == typ
    assert bytes((Byte * 3).in_dll(lib, "sent_payload")) == b"\x90\x1f\x07"
lib.link_init()
lib.ctrl_error(raw(b"\x90\x1f\x07"))
assert next_record() == b"\x83\x02\x07\x01"
lib.link_init()
C.c_bool.in_dll(lib, "send_ok").value = False
feed(b"\x0b\x03\x08\x50\x00")
assert next_record() == b"\x83\x02\x08\x01"
print("ctrl: fragmented HELLO, forward/release layouts, relay error and queue failure passed")
