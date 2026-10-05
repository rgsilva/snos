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


# The keyboard as a controller.
class Pad(ctypes.Structure):
    _fields_ = [("map", ctypes.c_uint8 * 12), ("sent", ctypes.c_uint16),
                ("resend", ctypes.c_bool)]


lib.hidpad_init.argtypes = [ctypes.POINTER(Pad)]
lib.hidpad_set_map.argtypes = [ctypes.POINTER(Pad), ctypes.POINTER(ctypes.c_uint8)]
lib.hidpad_mask.argtypes = [ctypes.POINTER(Pad), ctypes.POINTER(Keys)]
lib.hidpad_mask.restype = ctypes.c_uint16
lib.hidpad_due.argtypes = [ctypes.POINTER(Pad), ctypes.POINTER(Keys), ctypes.POINTER(ctypes.c_uint8)]
lib.hidpad_due.restype = ctypes.c_size_t
lib.hidpad_sent.argtypes = [ctypes.POINTER(Pad), ctypes.POINTER(ctypes.c_uint8)]
lib.hidpad_restart.argtypes = [ctypes.POINTER(Pad)]
lib.hidpad_button_parse.argtypes = [ctypes.c_char_p]
lib.hidpad_button_parse.restype = ctypes.c_int
lib.hidpad_map_text.argtypes = [ctypes.POINTER(Pad), ctypes.c_char_p, ctypes.c_size_t]
lib.hidpad_map_text.restype = ctypes.c_size_t
lib.hidkeys_usage_name.argtypes = [ctypes.c_uint8, ctypes.c_char_p]
lib.hidkeys_usage_parse.argtypes = [ctypes.c_char_p]
lib.hidkeys_usage_parse.restype = ctypes.c_uint8

B, Y, SELECT, START = 0x8000, 0x4000, 0x2000, 0x1000
UP, DOWN, LEFT, RIGHT = 0x0800, 0x0400, 0x0200, 0x0100
A, X, L, R = 0x0080, 0x0040, 0x0020, 0x0010


def due(pad, keys):
    """What the next poll queues: the mask, or None; queued means sent."""
    out = (ctypes.c_uint8 * 2)()
    if lib.hidpad_due(ctypes.byref(pad), ctypes.byref(keys), out) == 0:
        return None
    lib.hidpad_sent(ctypes.byref(pad), out)
    return out[0] | (out[1] << 8)


k, p = Keys(), Pad()
lib.hidkeys_init(ctypes.byref(k))
lib.hidpad_init(ctypes.byref(p))
assert due(p, k) == 0            # a link start sends the mask even unchanged
assert due(p, k) is None
assert report(k, 0, codes=[0x1D]) == b"z"   # Z is B, and still types z
assert due(p, k) == B
assert report(k, 10, codes=[0x1D]) == b"" and due(p, k) is None   # held
assert report(k, 20, codes=[0x1D, 0x52]) == bytes([0x1C]) and due(p, k) == B | UP
assert report(k, 30, codes=[0x1D, 0x52, 0x0E]) == b"k" and due(p, k) is None  # k is no button
assert report(k, 40, codes=[0x52, 0x0E]) == b"" and due(p, k) == UP
r = (ctypes.c_uint8 * 8)(0, 0, 1, 1, 1, 1, 1, 1)             # rollover: no news
assert lib.hidkeys_report(ctypes.byref(k), 50, r, (ctypes.c_uint8 * 8)(), 8) == 0
assert due(p, k) is None
assert report(k, 60, 0x20, [0x28]) == b"\n" and due(p, k) == SELECT | START
assert report(k, 70, 0x02) == b"" and due(p, k) == 0     # left Shift is no button
assert report(k, 80, codes=[0x1B, 0x16, 0x04, 0x14, 0x1A, 0x50]) == b"xsaqw\x1e"
assert due(p, k) == A | X | Y | L | R | LEFT
assert report(k, 90, codes=[0x4F, 0x51]) == bytes([0x1F, 0x1D]) and due(p, k) == RIGHT | DOWN
assert report(k, 100) == b"" and due(p, k) == 0
assert due(p, k) is None
lib.hidpad_restart(ctypes.byref(p))
assert due(p, k) == 0 and due(p, k) is None
# A press the link could not take stays due, as whatever is held by then.
assert report(k, 110, codes=[0x1D]) == b"z"
assert lib.hidpad_due(ctypes.byref(p), ctypes.byref(k), (ctypes.c_uint8 * 2)()) == 2
assert report(k, 120, codes=[0x1D, 0x1B]) == b"x" and due(p, k) == B | A
assert report(k, 130) == b"" and due(p, k) == 0

# Remapping: B on the b key, everything else on its default; 0 keeps a default.
m = (ctypes.c_uint8 * 12)(0x05)
lib.hidpad_set_map(ctypes.byref(p), m)
assert report(k, 140, codes=[0x1D]) == b"z" and due(p, k) is None
assert report(k, 150, codes=[0x05, 0x1B]) == b"bx" and due(p, k) == B | A
assert report(k, 160) == b"" and due(p, k) == 0
m[2] = 0xE1                                                  # SELECT on left Shift
lib.hidpad_set_map(ctypes.byref(p), m)
assert report(k, 170, 0x02) == b"" and due(p, k) == SELECT
assert report(k, 180, 0x20) == b"" and due(p, k) == 0

# Names, for `kbd map`.
text = ctypes.create_string_buffer(160)
lib.hidpad_init(ctypes.byref(p))
lib.hidpad_map_text(ctypes.byref(p), text, len(text))
assert text.value == (b"B=z Y=a SELECT=rshift START=enter UP=up DOWN=down LEFT=left "
                      b"RIGHT=right A=x X=s L=q R=w"), text.value
short = ctypes.create_string_buffer(12)
lib.hidpad_map_text(ctypes.byref(p), short, len(short))
assert short.value == b"B=z Y=a", short.value
for name, usage in ((b"z", 0x1D), (b"0", 0x27), (b"1", 0x1E), (b"enter", 0x28),
                    (b"rshift", 0xE5), (b"f1", 0x3A), (b"f12", 0x45), (b"pageup", 0x4B),
                    (b"delete", 0x4C), (b"space", 0x2C), (b"up", 0x52), (b"0x64", 0x64)):
    assert lib.hidkeys_usage_parse(name) == usage, name
    out = ctypes.create_string_buffer(10)
    lib.hidkeys_usage_name(usage, out)
    assert out.value == name, (out.value, name)
for name in (b"Z", b"ENTER", b"RShift", b"F5"):
    assert lib.hidkeys_usage_parse(name) == lib.hidkeys_usage_parse(name.lower()), name
for name in (b"", b"f13", b"f0", b"f1x", b"foo", b"0x01", b"0x100", b"0xg", b"zz"):
    assert lib.hidkeys_usage_parse(name) == 0, name
for name, button in ((b"B", 0), (b"select", 2), (b"Start", 3), (b"r", 11)):
    assert lib.hidpad_button_parse(name) == button, name
for name in (b"q", b"", b"selectx"):
    assert lib.hidpad_button_parse(name) == -1, name

print("hidkeys: F-keys, navigation, Delete, Alt, and the pad mask and map passed")
