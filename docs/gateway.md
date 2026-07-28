# Getting SNOS online

The SNES has no TCP/IP stack and never will. A gateway on the other side of the
link does DNS, TCP, TLS and HTTP, and hands the SNES plain bytes. It is a
forward proxy making ordinary, honest TLS connections as itself — nothing forges
a certificate, so there is no CA to install and nothing to trust on the SNES.

This is not a compromise for convenience. A TLS handshake on a 3.58 MHz 65816
runs to *minutes* before a byte of payload moves, so terminating TLS on the far
side is the only thing that makes IRC, HTTPS or anything modern reachable at all.

## One-time setup

Mesen's Lua sandbox blocks file and process access by default, and the bridge
needs it (there is no socket library — `require("socket")` fails even with
access on, which is why a separate program does the networking).

> **Debug → Script Window → "Allow access to I/O and OS functions"**

Turn it **off again when you are done**. It grants file and process access to
*any* Lua script you subsequently run, not just this one.

## Running it

```sh
cd snos/snes
python3 tools/gateway.py          # leave this running
```

Then in Mesen: load `snos.sfc`, open **Debug → Script Window**, load the
combined bridge (built by `tools/mkbridge.py`, which fuses the keyboard and
gateway bridges into one script so loading one no longer unloads the other), and
press **Run**.

In SNOS:

```
net                 gateway status
get URL             fetch and print
download URL        fetch and save to the SRAM volume
```

## Why two files instead of a socket or a pipe

Mesen's Lua has no sockets, so the process that opens them has to be a separate
one and something must carry bytes to it. A FIFO is the obvious choice and the
wrong one: a blocking read inside an `endFrame` callback freezes the emulator.
Reading a regular file at a tracked offset returns whatever is there and never
blocks, which is the property that matters.

`/tmp/snos-up.bin` and `/tmp/snos-down.bin` grow for the length of a session and
are truncated at startup.

## The gateway must not free-run

It emits **exactly one frame per frame received**. This is not a throttle, it is
the protocol: the real link is clocked by the SNES, so the gateway physically
cannot transmit unless an exchange happens.

An earlier version emitted a frame every 4 ms "so acks keep flowing". Its loop
ran at 250 Hz against a console polling at 60, so the down file grew four times
faster than it was consumed; the bridge fell permanently behind, read frames
from minutes earlier, and the file passed two megabytes in about two minutes. A
file bridge that free-runs is not the same protocol as the cable, and would hide
exactly the bugs that only appear on hardware.

## When it does not work

**"no gateway attached"** — the bridge has not claimed the link. It re-asserts
the magic word every frame precisely because `link_init()` zeroes the mailbox
*after* the script starts; if you see this persistently, the script is not
running.

**"timeout waiting for the gateway"** with the daemon plainly alive — restart
the daemon. It tracks file offsets, and restarting the emulator truncates both
files underneath it.

**The script produces no output whatsoever, and Mesen looks hung** — that is
what a Lua *syntax error* looks like in Mesen. There is no error message.
Suspect syntax before anything else; it has cost hours twice.

**A key does nothing** — check Mesen's own input bindings first. It consumes
keys bound to a controller button or a hotkey before any script sees them.
Backspace is bound by default, and F1–F11 are taken by the debugger, which is
why the break key is F12.

**Controller-driven tests fail claiming the keyboard is broken** — `make test`
checks for this now. Mesen resets `Snes.Port1.Type` to `None` when it exits, and
with no controller `emu.setInput` has nothing to set.

## Security

The SNES↔gateway hop is plaintext. Over a cable in your house that is fine, but
it is a decision rather than an oversight.

The daemon **listens on no socket** — traffic reaches it through a file the
bridge writes, so it cannot accidentally become an open proxy on your network.
Worth keeping that way if it ever grows a socket for convenience.
