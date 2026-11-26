#!/usr/bin/env python3
"""Host checks for termkeys.c: terminal bytes to console key bytes."""
import ctypes
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/termkeys_test"
BUILD.mkdir(parents=True, exist_ok=True)
SO = BUILD / "libtermkeys.so"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                "-I", str(ROOT), str(ROOT / "termkeys.c"), "-o", str(SO)], check=True)


class Term(ctypes.Structure):
    _fields_ = [("state", ctypes.c_uint8), ("prev_cr", ctypes.c_uint8), ("cr", ctypes.c_uint8),
                ("c3", ctypes.c_uint8), ("any", ctypes.c_uint8), ("num", ctypes.c_uint16),
                ("semis", ctypes.c_uint8), ("at", ctypes.c_uint32)]


class Out(ctypes.Structure):
    _fields_ = [("keys", ctypes.c_uint8 * 2), ("n", ctypes.c_uint8), ("leave", ctypes.c_bool)]


lib = ctypes.CDLL(str(SO))
lib.termkeys_init.argtypes = [ctypes.POINTER(Term), ctypes.c_bool]
lib.termkeys_feed.argtypes = [ctypes.POINTER(Term), ctypes.c_uint32, ctypes.c_uint8,
                              ctypes.POINTER(Out)]
lib.termkeys_tick.argtypes = [ctypes.POINTER(Term), ctypes.c_uint32, ctypes.POINTER(Out)]
ESC_MS = 50
checks = 0


class Rig:
    def __init__(self, after_cr=False):
        self.t, self.now, self.left, self.max_out = Term(), 1000, False, 0
        lib.termkeys_init(ctypes.byref(self.t), after_cr)

    def _take(self, o):
        self.left |= o.leave
        self.max_out = max(self.max_out, o.n)
        return bytes(o.keys[:o.n])

    def feed(self, data, gap=0):
        out = b""
        for b in data:
            self.now += gap
            o = Out()
            lib.termkeys_feed(ctypes.byref(self.t), self.now, b, ctypes.byref(o))
            out += self._take(o)
        return out

    def wait(self, ms):
        self.now += ms
        o = Out()
        lib.termkeys_tick(ctypes.byref(self.t), self.now, ctypes.byref(o))
        return self._take(o)


def eq(data, want, gap=0, after_cr=False):
    global checks
    r = Rig(after_cr)
    got = r.feed(data, gap) + r.wait(ESC_MS + 1)
    assert got == want, (data, got, want)
    assert not r.left and r.max_out <= 2
    checks += 1


UP, DOWN, LEFT, RIGHT = 0x1C, 0x1D, 0x1E, 0x1F
HOME, END, INS, DEL, PGUP, PGDN = 0x8E, 0x8F, 0x8D, 0x7F, 0x90, 0x91
for gap in (0, 1, 20):       # whole, and a byte per tick
    for i, ch in enumerate("ABCD"):
        want = bytes([[UP, DOWN, RIGHT, LEFT][i]])
        eq(b"\x1b[" + ch.encode(), want, gap)
        eq(b"\x1bO" + ch.encode(), want, gap)
        eq(b"\x1b[1;5" + ch.encode(), want, gap)
    for seq, k in ((b"[H", HOME), (b"OH", HOME), (b"[1~", HOME), (b"[7~", HOME),
                   (b"[F", END), (b"OF", END), (b"[4~", END), (b"[8~", END),
                   (b"[2~", INS), (b"[3~", DEL), (b"[5~", PGUP), (b"[6~", PGDN),
                   (b"[3;5~", DEL)):
        eq(b"\x1b" + seq, bytes([k]), gap)
    for i, ch in enumerate("PQRS"):
        eq(b"\x1bO" + ch.encode(), bytes([0x81 + i]), gap)
        eq(b"\x1b[1;2" + ch.encode(), bytes([0x81 + i]), gap)
    for i, n in enumerate((11, 12, 13, 14)):
        eq(b"\x1b[%d~" % n, bytes([0x81 + i]), gap)
    for i, n in enumerate((15, 17, 18, 19, 20, 21, 23, 24)):
        eq(b"\x1b[%d~" % n, bytes([0x85 + i]), gap)

eq(b"abc XYZ~ 09", b"abc XYZ~ 09")
eq(b"\r", b"\n"); eq(b"\n", b"\n"); eq(b"\r\n", b"\n"); eq(b"\r\r", b"\n\n")
eq(b"\n\n", b"\n\n"); eq(b"\r\n\n", b"\n\n"); eq(b"\r\n\r\n", b"\n\n")
eq(b"\n", b"", after_cr=True); eq(b"\r\n", b"\n", after_cr=True)
eq(b"a\n", b"a\n", after_cr=True)
eq(b"\x7f\x08", b"\x08\x08")
eq(b"\t\x01\x03\x1a", b"\t\x01\x03\x1a")
eq(b"\x00", b"")

# Escape: alone by timeout, Alt+x, and bare ESC [ / ESC O.
r = Rig()
assert r.feed(b"\x1b") == b"" and r.wait(ESC_MS - 1) == b"" and r.wait(1) == b"\x1b"
assert r.wait(100) == b""
eq(b"\x1bx", b"\x1bx"); eq(b"\x1bX", b"\x1bX"); eq(b"\x1b\r", b"\x1b\n")
eq(b"\x1b\x1b", b"\x1b\x1b"); eq(b"\x1b\x1bx", b"\x1b\x1bx")
eq(b"\x1b[", b"\x1b["); eq(b"\x1bO", b"\x1bO")
eq(b"\x1b[1", b""); eq(b"\x1b[1;", b"")        # unfinished sequence: swallowed
r = Rig()
r.feed(b"\x1b["); assert r.wait(ESC_MS - 1) == b"" and r.wait(1) == b"\x1b["
# The timeout counts from the latest byte, not the Escape.
r = Rig()
r.feed(b"\x1b", 0); r.now += 40; r.feed(b"["); r.now += 40
assert r.wait(0) == b""; assert r.feed(b"A") == bytes([UP])
r = Rig(); r.feed(b"\x1b\x1b[A"); assert r.wait(60) == b""
checks += 6

# Unknown sequences are swallowed whole; what follows is typed.
for seq in (b"\x1b[99~", b"\x1b[Z", b"\x1b[200~", b"\x1b[?1;2c", b"\x1b[M", b"\x1bOj",
            b"\x1b[<0;1;1M", b"\x1b[1;5Z"):
    eq(seq + b"q", b"q")
eq(b"\x1bOM", b"\n")
eq(b"\x1b[A\x1b[A", bytes([UP, UP]))
eq(b"\x1b[1\x03", b"\x03")                       # a control byte aborts the sequence
eq(b"\x1b[\x1b[B", bytes([DOWN])); eq(b"\x1bO\x1b[B", bytes([DOWN]))

# Exit: Ctrl+] is reported, never emitted; also right behind an Escape.
r = Rig(); assert r.feed(b"a\x1d") == b"a" and r.left
r = Rig(); assert r.feed(b"\x1b\x1d") == b"\x1b" and r.left
r = Rig(); assert r.feed(b"\x1b[\x1d") == b"" and r.left
checks += 3

# UTF-8: dropped, except the Portuguese and other Latin-1 letters, mapped to the base letter.
eq("áàâãçéêíóôõúü ÁÀÂÃÇÉÊÍÓÔÕÚÜ".encode(), b"aaaaceeiooouu AAAACEEIOOOUU")
eq("ñÑ€😀".encode(), b"nN")
eq(b"\xc3", b""); eq(b"\xc3a", b"a"); eq(b"\xff\xfe\x80a", b"a")
print(f"termkeys: {checks} checks")
