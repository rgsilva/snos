# SNOS — a native 65816 "OS" for the stock SNES

Not an emulator. SNOS is native 65816 code: a small supervisor that owns the
hardware, presents a shell, and runs applications that live in cartridge ROM.

The design rests on one property of the machine: **cart ROM is memory-mapped and
directly executable**, so "load a program" is a build-time decision rather than a
runtime one. There is no loader and no relocation. `exec` is *set the data bank,
direct page and stack, then `JML` into a ROM bank*; exit is a `COP #0`.

Target is a **stock SNES** — no SA-1, no expansion chips. HiROM + FastROM, 4 MB
cart, 256 KB battery SRAM.

See [`PLAN.md`](PLAN.md) for the full design and milestone list, and
[`docs/`](docs/) for hard-won toolchain notes.

## Status

| Milestone | State |
|---|---|
| M0 — Scaffold, build system, boot banner | ✅ |
| M1 — Console + input abstraction | ✅ |
| M2 — Syscall ABI (COP) + shell | ✅ |
| M3 — Apps: exec + return + kill switch | ✅ |
| M4 — Filesystem (ROMFS + SRAMFS) | ✅ |
| M5 — BASIC | ✅ |
| EDIT — menu-driven text editor | ✅ |
| M6 — Networking (framed link + gateway; `get`, `view`, `httpd`, `netecho`) | ✅ |
| M7 — Real hardware: Pico W adapter on port 2 (keyboard + network), `make LINK_HW=1` | ✅ live on silicon 2026-09-29: keystrokes and a real Minecraft client over the cable. Bluetooth keyboard and tier 2 (the Pico is the NAT, no PC) shipped 2026-10-01. Speed and comfort are the open list in [`PLAN.md`](PLAN.md) ([docs/pico-adapter.md](docs/pico-adapter.md)) |
| M8 — IRC client (`irc`, windows/tabs/user list) | ✅ |
| VNC — `vnc`, a Super Nintendo as a live VNC thin client | ✅ |
| M10 — Preemptive multitasking (`httpd &` or `run httpd &`, `jobs`, `kill`, `fg`, notify) | ✅ |
| M11 — Real TCP/IP: uIP on the console, user-mode NAT on the host (`ping`, `resolve`) | ✅ |
| M11.5 — Cutover: the stream gateway protocol deleted; uIP is the only backing | ✅ |
| M13 — `mc`: a Minecraft server (Java 1.16.5, one player, world baked into ROM) | ✅ |

(M9 was a browser; it shipped, was superseded by `vnc` — a real browser on the
far side of a VNC session beats a screenshot pipeline — and was deleted.)

Since M11 the console runs its own IPv4 stack: it composes and parses its own
headers, resolves its own names, and `httpd` answers connections opened from
outside. Since the cutover that followed, that stack is the *only* networking
there is: the old stream-gateway protocol is deleted on both sides, `get` is
a real HTTP/1.0 client (https is refused, period-correctly — TLS was never
going to run on a 3.58 MHz 65816), and the host daemon is purely a slirp-style
NAT — it routes the console's packets and holds inbound port-forwards, and
understands no protocol the console speaks.

## Building

Needs the vendored Calypsi 65816 toolchain (shared with the sibling projects)
and Mesen 2 for the test harness.

```sh
cd snes
make            # -> build/snos.sfc
make test       # run the automated Mesen tests
make LINK_HW=1  # -> build-hw/snos.sfc, the real-hardware ROM (port-2 adapter)
```

On Linux the toolchain and Mesen live elsewhere; `CALDIR`, `MESEN` and
`MESEN_SRM` are the three variables to override (Mesen's data directory is
`~/.config/Mesen2` there).

Override `CALDIR` if the toolchain lives elsewhere:

```sh
make CALDIR=/path/to/calypsi-65816-5.17
```

## Measuring the network

Run `python3 snes/tools/speedtest.py` on the gateway host, then
`net speedtest` on the console for download, upload and simultaneous B/s.
`net speedtest [host:port|-] [seconds]` overrides the destination or duration;
see [the measurement and protocol notes](docs/net-speedtest.md).

## The screen

Two text modes, both 28 rows. The hardware ROM (`make LINK_HW=1`) boots **32
columns in white**: Mode 0, 8 px glyphs at normal resolution, which survive
composite video into a CRT. The emulator ROM keeps the **64-column** Mode 5
hires console in green, which is what every screen-reading test was written
against; `make CON_COLS=32 CON_COLOUR=white` builds it with the hardware's
default instead.

`mode` says which one is up (`32x28 white`). `mode 32` and `mode 64` change
the width, which clears the screen; `mode white`, `green`, `mint` or `amber`
change the colour and leave the text alone; both can go in one command.
`edit` is laid out for 64 columns, so the kernel gives it 64
while it holds the screen and puts the shell's mode back when it exits or
is killed ([docs/console-notes.md](docs/console-notes.md)).

## Typing into it

On a stock SNES the on-screen keyboard is the only way in, which is fine for a
demo and tedious for development. SELECT opens it as a six-row panel along
the bottom; at 32 columns the controller hints move into its frame, since the
keys alone fill the width. The kernel's input layer has three
interchangeable backends behind `kbd_getchar()`, though, and one of them is a
byte ring in the test mailbox — which is how the automated tests type.

`snes/tools/kbdbridge.lua` puts your own keyboard on the other end of that same
ring, and `make bridge` combines it with the gateway bridge into a single
`build/snosbridge.lua` — the Script Window runs one script at a time, so loading
the network bridge would otherwise unload the keyboard. Open `snos.sfc` in the Mesen **GUI**, then Debug → Script Window → load
the script → Run, and type into the emulator window. It cannot work under
`--testrunner`, which never initialises the input subsystem.

If a key does nothing, check Mesen's own input bindings before suspecting the
script: Mesen consumes keys bound to a controller button or hotkey before a
script sees them. Backspace is bound by default.

Real hardware gets a fourth backend at M7: the Pico W adapter on port 2 carries
keystrokes as `LINK_CH_KBD` records on the same link frames as the network and
the kernel pushes them into that same ring (`docs/pico-adapter.md`,
`test/kbd.lua`). Nothing above `kbd_getchar()` has to change for any of them.

## Layout

```
PLAN.md              design + milestones
docs/                toolchain and hardware notes
snes/
  calypsi/           linker memory map, cart header, CPU vectors
  kernel/            the kernel itself
  tools/             ROM packer, test preprocessor
  test/              Mesen Lua tests (lib.lua is spliced in at build time)
apps/                applications, one ROM bank each (M3+)
```

## Credit where it's due

The console is reused from the sibling `snespdp11` project; the test mailbox
comes from `snesmac`. Design references are listed in `PLAN.md` §6 — in
particular Nintendo's own SFX-DOS (which is where the `COP`-as-syscall idea
comes from) and the BS-X launch ABI.
