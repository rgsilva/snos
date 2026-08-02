#!/usr/bin/env python3
"""The gateway's file-relay discipline, tested from the bridge's side of it.

`make test` never touches tools/netbridge.lua or gateway.py's main loop: every
Mesen suite speaks the protocol straight into the WRAM mailbox. So the file
transport — join-at-current-offset, one-frame-out-per-frame-in, shrink-detect
restart, the F_SYN session reset — had zero coverage, and a bridge change
could pass 13/13 and still leave a console with no network. This is the
missing half: a scripted peer that does to the files exactly what
netbridge.lua does, driving a REAL gateway.py subprocess.

Covered, in order:
  1. a HELLO round trip through the files (the basic exchange works);
  2. a GATEWAY restart mid-session (it must join the UP file where it is,
     announce F_SYN, and answer again — the deadlock this prevents is
     documented in gateway.py's main());
  3. a BRIDGE restart (both files truncated, peer sequence reset — the
     gateway must notice the UP shrink, tear the session down, and serve the
     new one);
  4. after every reset, the session actually answers HELLO again — which is
     the observable form of teardown() having worked.

What this deliberately cannot cover: the Mesen memory-callback ordering the
Lua half relies on (write-fires-before, read-fires-after). Those are
documented as measured in netbridge.lua and only the emulator can test them.

Plain assertions, no pytest: run `python3 test/host/bridge_test.py`, exit 0
on pass, and it prints RESULT PASS/FAIL like every Mesen suite so the same
grep finds it.
"""
import os
import subprocess
import sys
import tempfile
import time

FRAME = 256
HDR = 4
SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
F_DATA, F_SYN = 0x01, 0x02
CH_CTRL, CH_PAD = 0, 0xFF
C2S_HELLO = 0x04
S2C_HELLO = 0x84

TOOLS = os.path.join(os.path.dirname(__file__), "..", "..", "tools")


def checksum(f):
    return sum(b for i, b in enumerate(f) if i != SUM) & 0xFF


class Peer:
    """The SNES+bridge side, reduced to its file behaviour."""

    def __init__(self, up, down):
        self.up, self.down = up, down
        self.tx_seq = 1
        self.rx_next = None      # adopt from the first F_SYN, like link.c
        self.syn = True
        self.down_off = 0

    def restart(self):
        """What netbridge.lua does when the emulator (re)starts: truncate
        both files and begin a new session announcing F_SYN."""
        open(self.up, "wb").close()
        open(self.down, "wb").close()
        self.tx_seq = 1
        self.rx_next = None
        self.syn = True
        self.down_off = 0

    def send(self, records=b""):
        f = bytearray([CH_PAD]) * FRAME
        f[SEQ] = self.tx_seq
        f[ACK] = (self.rx_next - 1) & 0xFF if self.rx_next is not None else 0
        f[FLAGS] = (F_DATA if records else 0) | (F_SYN if self.syn else 0)
        f[HDR:HDR + len(records)] = records
        f[SUM] = 0
        f[SUM] = checksum(f)
        with open(self.up, "ab") as fh:
            fh.write(bytes(f))

    def recv(self, timeout=5.0):
        """Newest complete DOWN frame past our offset, or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                size = os.path.getsize(self.down)
            except FileNotFoundError:
                size = 0
            usable = size // FRAME * FRAME
            if usable > self.down_off:
                with open(self.down, "rb") as fh:
                    fh.seek(usable - FRAME)   # newest, like the bridge
                    f = fh.read(FRAME)
                self.down_off = usable
                return bytearray(f)
            time.sleep(0.01)
        return None

    def hello_roundtrip(self, tries=30):
        """Send HELLO, exchange frames until S2C_HELLO comes back.

        A recv timeout retransmits rather than failing: the gateway joins the
        UP file at whatever size it sees WHEN IT STARTS, so a frame written
        while it was still coming up is skipped by design — exactly the case
        stop-and-wait retransmission exists for, on the real link too."""
        hello = bytes([CH_CTRL, 2, C2S_HELLO, 0])
        self.send(hello)
        resends = 0
        for _ in range(tries):
            f = self.recv(timeout=1.0)
            if f is None:
                resends += 1
                assert resends <= 5, "the gateway stopped answering"
                self.send(hello)
                continue
            if f[SUM] != checksum(f):
                continue
            if f[FLAGS] & F_SYN or self.rx_next is None:
                self.rx_next = f[SEQ]
            acked = f[ACK] == self.tx_seq
            got_hello = False
            if (f[FLAGS] & F_DATA) and f[SEQ] == self.rx_next:
                self.rx_next = (self.rx_next + 1) & 0xFF
                o = HDR
                while o + 2 <= FRAME:
                    ch, ln = f[o], f[o + 1]
                    if ch == CH_PAD or o + 2 + ln > FRAME:
                        break
                    if ch == CH_CTRL and ln >= 2 and f[o + 2] == S2C_HELLO:
                        got_hello = True
                    o += 2 + ln
            if acked:
                self.tx_seq = (self.tx_seq + 1) & 0xFF
                self.syn = False
            if got_hello:
                return
            self.send()          # keep the exchange moving: acks only
        raise AssertionError("no S2C_HELLO within %d exchanges" % tries)


def start_gateway(up, down):
    env = dict(os.environ, SNOS_UP=up, SNOS_DOWN=down)
    return subprocess.Popen(
        [sys.executable, "-u", os.path.join(TOOLS, "gateway.py")],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    tmp = tempfile.mkdtemp(prefix="snos-bridge-test-")
    up = os.path.join(tmp, "up.bin")
    down = os.path.join(tmp, "down.bin")
    peer = Peer(up, down)
    peer.restart()

    gw = start_gateway(up, down)
    try:
        # 1. the basic exchange
        peer.hello_roundtrip()

        # 2. gateway restart: it must join the UP file at its current size,
        # announce F_SYN, and answer the next session. (Killing it hard is
        # the realistic case — nobody Ctrl-Cs cleanly mid-demo.)
        gw.kill()
        gw.wait()
        gw = start_gateway(up, down)
        time.sleep(0.3)          # let it record the join offset
        peer.hello_roundtrip()

        # 3. bridge restart: both files truncated under the gateway. It must
        # notice the UP shrink, reset, and serve the fresh session.
        peer.restart()
        peer.hello_roundtrip()

        print("RESULT PASS bridge -- hello survives a gateway restart and a bridge restart")
        return 0
    except AssertionError as e:
        print(f"RESULT FAIL bridge -- {e}")
        return 1
    finally:
        gw.kill()
        gw.wait()


if __name__ == "__main__":
    sys.exit(main())
