"""Host checks for HID boot keyboard translation and repeat."""
import ctypes
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/hidkeys_test"
BUILD.mkdir(parents=True, exist_ok=True)
SO = BUILD / "libhidkeys.so"

subprocess.run([
    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
    "-I", str(ROOT), str(ROOT / "hidkeys.c"), "-o", str(SO),
], check=True)


class Keys(ctypes.Structure):
    _fields_ = [
        ("modifiers", ctypes.c_uint8),
        ("keys", ctypes.c_uint8 * 6),
        ("repeat_key", ctypes.c_uint8),
        ("repeat_modifiers", ctypes.c_uint8),
        ("repeat_byte", ctypes.c_uint8),
        ("repeat_at", ctypes.c_uint32),
    ]


lib = ctypes.CDLL(str(SO))
lib.hidkeys_init.argtypes = [ctypes.POINTER(Keys)]
lib.hidkeys_report.argtypes = [
    ctypes.POINTER(Keys), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
]
lib.hidkeys_report.restype = ctypes.c_size_t
lib.hidkeys_repeat.argtypes = [
    ctypes.POINTER(Keys), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
]
lib.hidkeys_repeat.restype = ctypes.c_size_t


def report(keys, now, modifiers=0, codes=()):
    r = (ctypes.c_uint8 * 8)()
    r[0] = modifiers
    for i, code in enumerate(codes):
        r[2 + i] = code
    out = (ctypes.c_uint8 * 8)()
    n = lib.hidkeys_report(ctypes.byref(keys), now, r, out, len(out))
    return bytes(out[:n])


def repeat(keys, now):
    out = (ctypes.c_uint8 * 8)()
    n = lib.hidkeys_repeat(ctypes.byref(keys), now, out, len(out))
    return bytes(out[:n])


k = Keys()
lib.hidkeys_init(ctypes.byref(k))

assert report(k, 0, codes=[4]) == b"a"
assert report(k, 10, codes=[4]) == b""
assert report(k, 20) == b""
assert report(k, 30, 0x02, [4]) == b"A"
assert report(k, 40) == b""
assert report(k, 50, 0x02, [30, 31, 45, 46, 47, 48,]) == b"!@_+{}"
assert report(k, 60) == b""

assert report(k, 70, codes=[40, 42, 43, 41]) == b"\n\b\t\x1b"
assert report(k, 80) == b""
assert report(k, 90, codes=[0x52, 0x51, 0x50, 0x4F]) == bytes([0x1C, 0x1D, 0x1E, 0x1F])
assert report(k, 100) == b""
assert report(k, 110, 0x01, [22]) == bytes([0x13])
assert report(k, 120) == b""

assert report(k, 200, codes=[4]) == b"a"
assert report(k, 250, codes=[4, 5]) == b"b"
assert repeat(k, 749) == b""
assert repeat(k, 750) == b"b"
assert repeat(k, 782) == b""
assert repeat(k, 783) == b"b"
assert report(k, 790, codes=[4]) == b""
assert repeat(k, 1290) == b""
assert report(k, 1300) == b""

print("hidkeys: translation, new-key rollover, release, and repeat timing passed")
