# SNOS on an SA-1 cartridge: the port plan (2026-11-22, not started)

Read `docs/sa1-notes.md` first. It has the register facts and the numbers
from the emulator. The numbers from the real cartridge (sd2snes, firmware
1.11.0b1) are in the last section of this file.

## Goal

Build SNOS as an SA-1 cartridge image. The kernel stays on the S-CPU and
does not change its design. An application can start a routine on the SA-1
and exchange data with it. The stock (no chip) image stays the default build
and must not change.

Not a goal: to run the kernel, the scheduler or system calls on the SA-1.
The SA-1 cannot see WRAM, the PPU, DMA or the controller ports.

## Decisions

| Decision | Choice | Reason |
|---|---|---|
| Build | `make SA1=1` builds into `build-sa1/`; with `LINK_HW=1` into `build-hw-sa1/` | Same pattern as `LINK_HW`. One header cannot serve both cartridges. |
| Kernel code | Same sources. Differences behind `#ifdef SA1` in `sram.c`, `main.c`, and in the linker file and header | The suite must pass on both images. |
| ROM speed | FastROM stays on (`$420D`) | Measured on the cartridge: it works with the SA-1 (`a:F` 1.2x). |
| Storage | BW-RAM, linear at `$40:0000-$43:FFFF` | One far pointer, no 8 KB windows. DMA from it to VRAM works on the cartridge. |
| BW-RAM split | Filesystem: first 192 KB. SA-1 work area: last 64 KB (`$43:0000-$43:FFFF`) | The SA-1 has no other large RAM. One constant; change it if DOOM needs more. |
| SA-1 units not used | Division, cumulative sum, type 1 character conversion, write protection | Wrong or absent on the cartridge. Multiply is correct. |
| S-CPU IRQ from the SA-1 | Off (`SIE` = 0). The S-CPU polls a flag | The kernel has no IRQ handler, and it does not need one. |

## Phase 1: the memory map (no new feature)

Done when: `make SA1=1 test` passes all tests that the default image
passes, the default image is byte-identical to before, the SA-1 image starts
on the cartridge, and a file survives a power cycle.

1. `calypsi/linker-sa1.scm`. Bank `$00:8000-$FFFF` is ROM offset 0, not
   `$8000`. Scatter `code` (trap shims, vector targets, Calypsi startup) to
   `$C0:0000`, the header to `$C0:7FB0`, the vectors to `$C0:7FE0`. `farcode`
   and `cfar` start at `$C0:8000`. Today `code` uses 2.8% of its 32 KB, so
   size is not a problem.
2. `calypsi/header-sa1.s`. Map mode `$23`, cartridge type `$35`, RAM size
   `$08`. The trap shims are the same code; share them with an include.
3. `tools/raw2sfc.py`: an option for the header at `$7FC0` (size byte
   `$7FD7`, checksum). `app.c` reads the size byte at `$00:FFD7`; that
   address is ROM `$7FD7` on SA-1, so the code does not change.
4. Applications, ROMFS and the Minecraft data stay at `$C8+`, `$CF`, `$E0+`.
   The four ROM bank registers at reset give a linear `$C0-$FF`. Write them
   at boot to be sure.
5. `main.c`, before anything else: keep the SA-1 in reset, `SIE` = 0, clear
   its IRQ flags, enable BW-RAM writes (`SBWE` = `$80`) and I-RAM writes
   (`SIWP` = `$FF`). The cartridge ignores the write enables, the real chip
   does not; write them anyway.
6. `sram.c`: `sram_ptr()` becomes `$400000 + offset`. `sram_probe()` counts
   8 KB blocks in the same way and stops at the filesystem limit.
   `fs.c` `dma_block()`: the SRAM case uses bank `$40 + (offset >> 16)` and
   clips at the bank end, not at the 8 KB window.
7. `mem` prints one more line: `SA-1 yes/no`, and the BW-RAM split.
8. Makefile: `SA1=1`, the build directories, and `MESEN_SRM` for the new
   image name. `tools/checkenv.py` and the test library do not change.

Risk: a part of the kernel that depends on `$00:8000` = `$C0:8000` and is
not in this list. The full suite on the SA-1 image is the check.

## Phase 2: an application uses the SA-1

Done when: a new application `sa1demo` starts an SA-1 routine, gets a
correct result, shows the speed ratio, and stops the SA-1 when it exits or
is killed. A test proves each of these.

- **Where SA-1 code is.** In the application's ROM bank, executed in place.
  This is the same idea as the rest of SNOS. Hand-written assembly first. C
  is possible later (direct page and stack in I-RAM), not in this phase.
- **Where SA-1 data is.** Direct page and stack in I-RAM (2 KB in total).
  Large data in the BW-RAM work area. The S-CPU side reads results there.
- **I-RAM layout** (S-CPU address): `$3000-$30FF` SA-1 direct page,
  `$3100-$36FF` SA-1 stack, `$3700-$37FF` kernel mailbox (command, state,
  done flag, 16-bit arguments). One layout, in `snos_abi.h`.
- **System calls (ABI 14).**
  - `SYS_SA1_START`: p0/p1 = entry address. Starts the SA-1 there. Fails if
    there is no SA-1 or it is in use.
  - `SYS_SA1_STOP`: puts the SA-1 in reset.
  - `SYS_SA1_WAIT`: A = maximum frames. Waits for the done flag in a loop
    that runs from WRAM, so that the S-CPU does not read ROM. Returns early
    when the flag is set.
- **Why the WRAM wait matters.** While the S-CPU runs from ROM, the SA-1
  loses speed (3.8x becomes 3.0x on the cartridge). The kernel copies a
  small wait loop to WRAM at boot.
- **One owner.** The SA-1 belongs to the process that started it. The exit
  path, the kill switch, `kill`, `restart` and a background death all stop
  it. A second start by another process fails.
- **Apps declare it.** A header flag (bit 1 of the flags byte): "needs the
  SA-1". The kernel refuses to launch such an app on a stock cartridge and
  gives the reason.

Risk: the kill path. An app killed while the SA-1 runs must not leave it
running into the next app's data.

## Phase 3 (separate decision): the DOOM prototype

Not part of the port. A measurement: Mode 7 with direct pixels at about
112 x 80, a wall-column routine on the SA-1 that writes to BW-RAM, DMA to
VRAM from the S-CPU side, a frame counter. Go on only if it gives 5 frames
per second or more. Open problem: with the network adapter active, the
kernel's NMI uses about 120 of 262 lines; the display copy must fit around it.

## Order of work

Phase 1, then a run on the cartridge, then phase 2, then a run on the
cartridge. One branch for each phase, from `feat/sa1-probe`. Opus writes the
code; Fable reviews it and runs the tests again.

## Measured on the cartridge (2026-11-22)

SA-1 against the S-CPU, the same routine, 16 frames. Ii = data in I-RAM and
S-CPU idle in WRAM; Bi = data in BW-RAM, idle; Ib and Bb = S-CPU busy in ROM.

| Routine | Ii | Bi | Ib | Bb | Mesen Ii |
|---|---|---|---|---|---|
| (a) add, compare, branch | 2.3 | 2.3 | 1.8 | 1.8 | 3.5 |
| (b) software multiply | 3.8 | 2.7 | 3.0 | 2.2 | 3.9 |
| (c) hardware multiply | 10.0 | 8.5 | 8.5 | 7.4 | 11.8 |
| (d) 256-byte copy | 3.4 | 2.7 | 2.7 | 2.3 | 3.8 |

- The S-CPU counts are the same as in Mesen (2616, 1120, 3785, 237).
- The ROM map is the same as in Mesen, all lines.
- BW-RAM: 256 KB, battery-backed (`boots=1`, then `boots=2`).
- DMA from BW-RAM to VRAM: 677 8bpp tiles in a frame, 95 in a vertical blank.
- Failures: `iramwp`, `bwprot` (no write protection, as in Mesen); `div`
  (-1000 / 7 gave -142); `sum` (3 of 6 correct); `cc8`, `cc4` (type 1
  conversion gives wrong data; type 2 is correct); `vc` reads 0.
