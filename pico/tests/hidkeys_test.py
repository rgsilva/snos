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

# The rest of the keyboard: F-keys, the navigation block, Delete and Alt.
F1, INSERT, HOME, END, PAGEUP, PAGEDOWN, DELETE = 0x81, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x7F
k = Keys()
lib.hidkeys_init(ctypes.byref(k))
assert report(k, 0, codes=list(range(0x3A, 0x40))) == bytes(range(F1, F1 + 6))
assert report(k, 10) == b""
assert report(k, 20, codes=list(range(0x40, 0x46))) == bytes(range(F1 + 6, F1 + 12))
assert report(k, 30) == b""
assert report(k, 40, 0x02, [0x3A]) == bytes([F1])           # Shift+F1 is F1
assert report(k, 50) == b""
assert report(k, 60, codes=[0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E]) == \
    bytes([INSERT, HOME, PAGEUP, DELETE, END, PAGEDOWN])
assert report(k, 70) == b""
assert report(k, 80, 0x04, [0x1B]) == b"\x1bx"               # left Alt
assert report(k, 90) == b""
assert report(k, 100, 0x40, [0x52]) == b"\x1b\x1c"           # right Alt + Up
assert report(k, 110) == b""
assert report(k, 120, 0x06, [0x04]) == b"\x1bA"              # Alt+Shift
assert report(k, 130) == b""
assert report(k, 140, 0x05, [0x06]) == b"\x1b\x03"           # Ctrl+Alt+c
assert report(k, 150) == b""
assert report(k, 160, 0x04, [0x4A]) == bytes([0x1B, HOME])
assert report(k, 170) == b""
# Alt+x held repeats as the pair; a pair that does not fit is not half-sent.
assert report(k, 200, 0x04, [0x1B]) == b"\x1bx"
assert repeat(k, 700) == b"\x1bx"
one = (ctypes.c_uint8 * 1)()
assert lib.hidkeys_repeat(ctypes.byref(k), 800, one, 1) == 0
assert repeat(k, 800) == b"\x1bx"
assert report(k, 810) == b""
r = (ctypes.c_uint8 * 8)(0x04, 0, 0x1B)
assert lib.hidkeys_report(ctypes.byref(k), 820, r, one, 1) == 0
assert report(k, 840, 0x04, [0x1B, 0x1C]) == b"\x1by"        # the held x is not new
assert report(k, 850) == b""


print("hidkeys: F-keys, navigation, Delete and Alt passed")
