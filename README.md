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
| M3 — Apps: exec + return | ✅ |
| M4 — Filesystem (ROMFS + SRAMFS) | ✅ |
| M5 — BASIC | ✅ |
| M6 — Network stack (uIP) | |
| M7 — Real hardware | |
| M8 — IRC client | |
| M9 — Browser | |
| M10 — Preemptive multitasking | |

## Building

Needs the vendored Calypsi 65816 toolchain (shared with the sibling projects)
and Mesen 2 for the test harness.

```sh
cd snes
make            # -> build/snos.sfc
make test       # run the automated Mesen tests
```

Override `CALDIR` if the toolchain lives elsewhere:

```sh
make CALDIR=/path/to/calypsi-65816-5.17
```

## Typing into it

On a stock SNES the on-screen keyboard is the only way in, which is fine for a
demo and tedious for development. The kernel's input layer has three
interchangeable backends behind `kbd_getchar()`, though, and one of them is a
byte ring in the test mailbox — which is how the automated tests type.

`snes/tools/kbdbridge.lua` puts your own keyboard on the other end of that same
ring. Open `snos.sfc` in the Mesen **GUI**, then Debug → Script Window → load
the script → Run, and type into the emulator window. It cannot work under
`--testrunner`, which never initialises the input subsystem.

If a key does nothing, check Mesen's own input bindings before suspecting the
script: Mesen consumes keys bound to a controller button or hotkey before a
script sees them. Backspace is bound by default.

Real hardware gets a fourth backend at M7: the USB-keyboard-over-controller-port
bridge already proven in `../keyboard-test-rom`. Nothing above `kbd_getchar()`
has to change for any of them.

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
