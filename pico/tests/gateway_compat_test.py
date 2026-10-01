#!/usr/bin/env python3
"""Real gateway.py against actual 99b6002/current ROM link code, both sizes.

Exercise ordinary HELLO/IP, gateway restart mid-upload, and an idle console
restart. The old ROM's known mid-download reboot limitation is not claimed
fixed. These are protocol tests, not emulation or automatic-HELLO tests.
"""
import contextlib
import io
import pathlib
import struct
import subprocess
import sys
from console_main_fixture import prepare

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "snes/tools"))
import gateway as gw

BUILD = ROOT / "pico/build/gateway_compat_test"
BUILD.mkdir(parents=True, exist_ok=True)


def checksum(data):
    data += bytes(len(data) % 2)
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return total ^ 65535


def packet(size=40):
    body = bytearray(bytes([8, 0, 0, 0, 0x12, 0x34, 0, 1]) + bytes(size - 28))
    body[2:4] = struct.pack("!H", checksum(body))
    header = bytearray.fromhex("4500000000010000400100000a00020f0a000202")
    header[2:4] = struct.pack("!H", size)
    header[10:12] = struct.pack("!H", checksum(header))
    return bytes(header + body)


class Console:
    def __init__(self, exe, size):
        self.size = size
        self.proc = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def command(self, op, data=b"", n=2):
        self.proc.stdin.write(op + data)
        self.proc.stdin.flush()
        reply = self.proc.stdout.read(n)
        assert len(reply) == n
        return reply

    def exchange(self, peer):
        peer.on_frame(bytearray(self.command(b"x", peer.next_out(), self.size)))

    def pump(self, peer, count=30):
        for _ in range(count):
            self.exchange(peer)

    def hello(self, peer):
        self.pump(peer, 8)  # let SYN adoption discard the previous session
        before = struct.unpack("<HH", self.command(b"s", n=4))[1]
        assert self.command(b"h") == b"\0\0"
        self.pump(peer)
        version, hellos = struct.unpack("<HH", self.command(b"s", n=4))
        assert version == 2 and hellos > before

    def write(self, pkt):
        stream = struct.pack("<H", len(pkt)) + pkt
        assert self.command(b"w", struct.pack("<H", len(stream)) + stream) == struct.pack("<H", len(stream))

    def drain(self):
        data = bytearray()
        while True:
            n = struct.unpack("<h", self.command(b"r"))[0]
            if n == -1:
                data.clear()
            elif n == 0:
                return data
            else:
                data += self.proc.stdout.read(n)

    def ping(self, peer):
        self.drain()
        self.write(packet())
        self.pump(peer, 80)
        response = self.drain()
        assert len(response) == 42, (len(response), len(peer.ipbuf))
        assert response[:2] == b"\x28\0" and response[22] == 0
        assert checksum(response[2:22]) == checksum(response[22:]) == 0
        assert not peer.ipbuf and not peer.ip_hold

    def close(self):
        self.proc.stdin.close()
        assert self.proc.wait(timeout=5) == 0
        self.proc.stdout.close()


cases = 0
for current in (False, True):
    flags = (["-I", str(ROOT / "snes/kernel"),
              '-DCONSOLE_NET_SOURCE="' + str(ROOT / "snes/kernel/net.c") + '"',
              "-DTRANSITION_CURRENT=1", "-DTEST_CONSOLE_WINDOW=1"]
             if current else prepare(ROOT, BUILD))
    for size in (64, 256):
        exe = BUILD / f"console-{current}-{size}"
        subprocess.run(["cc", "-std=c11", "-O0", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-Wno-pointer-to-int-cast", *flags,
                        *(["-DTEST_EMULATED_LINK=1"] if size == 256 else []),
                        str(ROOT / "pico/tests/gateway_compat_harness.c"), "-o", str(exe)], check=True)
        gw.FRAME = size
        console = Console(exe, size)
        with contextlib.redirect_stdout(io.StringIO()):
            peer = gw.Gateway()
            try:
                console.hello(peer)
                console.ping(peer)
                console.write(packet(1000))
                console.exchange(peer)  # first upload fragment reaches the old gateway
                stale = console.command(b"x", peer.next_out(), size)
                peer.teardown()
                peer = gw.Gateway()
                peer.on_frame(bytearray(stale))  # headerless in-flight fragment
                assert not peer.ipbuf
                console.hello(peer)
                console.ping(peer)
                assert console.command(b"i", n=1) == b"\0"
                console.hello(peer)
                console.ping(peer)
            finally:
                peer.teardown()
                console.close()
        cases += 3
        print(f"PASS gateway.py {'current' if current else '99b6002'} frame={size}: boot, gateway restart, console restart")
print(f"gateway compatibility: {cases}/12 cases passed")
