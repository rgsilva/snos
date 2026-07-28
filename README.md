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
| M4 — Filesystem (ROMFS + SRAMFS) | |
| M5 — BASIC | |
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
