# SA-1: what sa1probe proves, measures and leaves open — 2026-11-22

`sa1/` builds `sa1/build/sa1probe.sfc`, a 4 MB SA-1 cartridge that boots the
second CPU, exercises the parts of the chip a kernel port would lean on, and
prints one line per check on three 32-column pages (A pages through them).
`make test` in `sa1/` runs it twice under Mesen; the second run is the
power-on that proves BW-RAM is battery-backed. Nothing in `snes/` changed.

## Sources

- **fullsnes**, "SNES Cart SA-1" chapter: <https://problemkaputt.de/fullsnes.htm>
  (register bits, reset values, memory map, conversion and maths semantics).
- **sneslab SA-1 pages** (Vitor Vilela's writeup, Book II page references):
  <https://sneslab.net/wiki/SA-1>, `/wiki/VC`, `/wiki/BWPA`, `/wiki/CXB`.
- **SnesSpeedTest** (Vilela, hardware-measured SA-1 clock per bus case):
  <https://github.com/VitorVilela7/SnesSpeedTest>, and byuu's summary of it
  at <https://nesdev.nes.science/f12/t17779.xhtml>.
- **bsnes** `sfc/coprocessor/sa1/{io,memory,bwram,iram,rom}.cpp` as a second
  model where the documents are silent.
- **Mesen** `Core/SNES/Coprocessors/SA1/` (cited below as `Sa1.cpp:Fn`): what
  the test rig actually does. The SNESdev wiki SA-1 pages sit behind a bot
  check and could not be read.

## The image

4 MB: the largest ROM a retail SA-1 cart used (Super Mario RPG, Kirby Super
Star), so every SA-1 implementation must accept it, and the smallest that
gives the four MMC registers four distinct 1 MB blocks to tell apart.
`tools/mksfc.py` (a sibling of `snes/tools/raw2sfc.py`, which patches the
HiROM header at `$FFxx`) pads the linked bank `$00` to 4 MB, writes an 8-byte
marker (`MK`, its own ROM offset, the offset inverted) at `+$7F80` of every
32 KB block, and fixes size and checksum at `$7FC0`. Mesen's header scoring
(`BaseCartridge::GetHeaderScore`) gives map `$23` no LoROM/HiROM point, but
the `$7FC0` candidate wins on checksum, sizes and the `SEI` at the reset
vector, while the HiROM candidate at `$FFC0` sits in `$FF` filler and scores
0; it then reports the cart as SA-1 with battery.

## Register facts the ROM relies on

| What | Fact | Source |
|---|---|---|
| Header | map `$23`, type `$35` (SA-1+ROM+RAM+battery), size `$0C`, RAM `$08`, at ROM `$7FC0` | fullsnes; Mesen `BaseCartridge::GetCoprocessorType` |
| Start | S-CPU writes CRV/CNV/CIV (`$2203-$2208`), then CCNT `$2200` bit 5 = 0 | fullsnes; `Sa1.cpp:CpuRegisterWrite` |
| SA-1 vectors | reset/NMI/IRQ from those registers; BRK/COP from ROM `$00:FFE4/6` | fullsnes; `Sa1::ReadVector` |
| S-CPU vectors | NMI/IRQ optionally from SNV/SIV when SCNT bits 4/6 are set | fullsnes; `Sa1VectorHandler` |
| Write enables | SIWP/CIWP (I-RAM, bit per 256 B), SBWE/CBWE, BWPA all reset to "protected" | fullsnes "Reset"; sneslab BWPA |
| BW-RAM protect | BWPA's area is protected only while SBWE **and** CBWE are 0 | bsnes `bwram.cpp` (needed by Kirby's Dream Land 3) |
| Signals | CCNT bit 7/4 = IRQ/NMI to SA-1 + 4-bit message; SCNT bit 7 = IRQ to S-CPU; CIC/SIC clear | fullsnes |
| Maths | MCNT 0 mul, 1 div, 2 sum (bit 1 clears it); MB high byte starts; 5/5/6 cycles | fullsnes "Arithmetic Maths" |
| Division | signed dividend / unsigned divisor, quotient rounded down, remainder unsigned, x/0 = 0 | fullsnes; bsnes `io.cpp` |
| Type 1 conv. | SA-1 DCNT = `$B0`; S-CPU SDA, CDMA, DDA (`$2236` starts it); wait SFR bit 5; DMA from BW-RAM | fullsnes "Character Conversion" |
| Super MMC | CXB-FXB bits 0-2 = which MB at `$C0/$D0/$E0/$F0`; bit 7 also maps it at `$00/$20/$80/$A0` | fullsnes; sneslab CXB |
| VC `$230E` | open bus on real carts: `$23` via `lda $230E`, `$80` via `$80:230E` | sneslab VC |

## Memory map: SNOS today against an SA-1 cart

| S-CPU address | SNOS (HiROM `$31`) | SA-1 cart (MMC at reset `$00-$03`) |
|---|---|---|
| `$00-$3F:8000-FFFF` | ROM; `$00:8000` = ROM `$8000` = `$C0:8000` | 32 KB per bank: `$00:8000` = ROM **0** = `$C0:0000`; `$01:8000` = `$C0:8000`; `$20-$3F` = 2nd MB |
| `$80-$BF:8000-FFFF` | FastROM mirror of the above | 3rd/4th MB, unless EXB/FXB bit 7 |
| `$C0-$FF` | 64 KB HiROM banks, linear | same, as four 1 MB blocks (linear at reset) |
| header, vectors | ROM `$FFC0-$FFFF` | ROM `$7FC0-$7FFF` |
| `$40-$4F` | ROM mirror | BW-RAM, 256 KB linear at `$40-$43`, mirrored to `$4F` |
| `$xx:6000-7FFF` | SRAM, a different 8 KB window per bank (`$A0-$BF`) | BW-RAM, **one** 8 KB window in every bank, block from BMAPS |
| `$xx:2200-23FF`, `3000-37FF` | unused | SA-1 registers; I-RAM (2 KB) |
| `$00:0000-1FFF` | WRAM | WRAM for the S-CPU; the SA-1 sees only I-RAM there (`$0000-$07FF`) |

What a kernel port has to change, as far as this ROM shows:

1. **Bank `$00` is ROM offset 0, not `$8000`.** `linker.scm` scatters `code`
   (trap shims, vector targets) to `$C0:8000`; on SA-1 it must go to
   `$C0:0000-$7FAF`, header and vectors to `$7FB0`, and `farcode` moves up into
   `$C0:8000`. Still 32 KB for everything COP/BRK/NMI can reach.
2. **Vectors**: COP and BRK always come from ROM `$7FE4/6`. NMI and IRQ can be
   pointed anywhere in bank `$00` through SNV/SIV, which a kernel can use to
   swap handlers without rewriting ROM.
3. **SRAM becomes BW-RAM**: `sram.c`'s per-bank windows turn into either the
   linear `$40:0000-$43:FFFF` (simplest: one far pointer, no windows) or the
   single `$6000` window steered by BMAPS. Writes need SBWE = `$80` first.
4. **The 8 KB of bank `$00` RAM stays the S-CPU's.** The SA-1 has no WRAM: its
   direct page and stack must be in I-RAM (2 KB, 10.74 MHz) or in BW-RAM
   through its `$6000` window (BMAP; half speed). The `$1A00` syscall block,
   the console shadow and everything in `$7E/$7F` are invisible to it.
5. **Shared between the CPUs**: ROM, I-RAM, BW-RAM, the SA-1 registers. Not
   shared: WRAM, PPU, DMA, joypads, the `$4202` ALU. SA-1 code computes; the
   S-CPU moves results to VRAM/WRAM. So syscalls stay S-CPU-only and an SA-1
   app needs a mailbox ABI in I-RAM, like `IM_CMD` here.
6. **ROM is contended.** Whenever the S-CPU runs from ROM the SA-1's ROM
   accesses slow down (numbers below): the kernel should wait for SA-1 work
   in a WRAM loop, not in its ROM idle loop.
7. FastROM: this ROM is SlowROM like retail SA-1 games; fullsnes doubts 3.58
   MHz is usable with an SA-1. The `a:F` line runs bench (a) from bank `$80`
   with MEMSEL = 1 (EXB = `$80` maps the first MB there). Mesen: 1.25x.

## Measured under Mesen (`make test`, 2026-11-22)

Checks: everything OK except the three lines Mesen provably gets wrong (next
section). I-RAM 2048 B; SA-1 signature after 0 frames; all four signals
answered within the frame; VC reads `a=23 00=00 80=80` (open bus); with SIE
off the request still shows in SFR and SIC clears it; BW-RAM 256 KB (32 of
128 8 KB windows at `$40-$4F` distinct); both windows map blocks 0, 1, 31;
the battery counter reads 1, then 2 after a second power-on.

ROM map, as ROM offsets: `00:8000=000000 01:8000=008000 20:8000=100000
80:8000=200000 A0:8000=300000 C0:0000=000000 C0:8000=008000
D0:0000=100000 F0:0000=300000 FF:8000=3F8000`. FXB = `$00` moves only
`$F0` (to 0); FXB = `$80` moves `$A0` too. The SA-1 reads the same five.

Character conversion: 8bpp and 4bpp type 1 match the build-time tiles; the
picture on page 2 is drawn with them; type 2 (one tile through BRF) matches.
Throughput **677 8bpp tiles per frame** if DMA'd for a whole frame, **95 per
vertical blank** (37 lines), the same with conversion off: Mesen converts for
free, so this is only the DMA rate. At 4bpp it is twice the tiles. A full
32 x 28 screen of 4bpp tiles is ~0.7 frame of DMA, against the ~1.4 s the
software path costs today.

Speed, iterations in 16 frames (S = S-CPU, SlowROM, data in WRAM):

| bench | S | SA-1, I-RAM, S idle | BW-RAM, idle | I-RAM, S busy | BW-RAM, busy |
|---|---|---|---|---|---|
| (a) add/cmp/branch | 2616 | 10138 **x3.9** | 9996 x3.8 | 5909 x2.3 | 5851 x2.2 |
| (b) soft 16x16 mul | 1120 | 4365 **x3.9** | 3091 x2.8 | 3106 x2.8 | 2400 x2.1 |
| (c) hardware mul | 3785 | 43316 **x11.4** | 36652 x9.7 | 28088 x7.4 | 25122 x6.6 |
| (d) 256 B copy | 237 | 924 **x3.9** | 694 x2.9 | 642 x2.7 | 522 x2.2 |

S-CPU bench (a) in FastROM: 3263, x1.2. "Busy" is the S-CPU polling HVBJOY
from ROM with a delay loop between polls; "idle" is the same loop copied to
WRAM. (c) is one SA-1 multiply against four 8 x 8 products on `$4202`.

What the documents predict: SA-1 10.74 MHz against 2.68 (x4) with code in ROM
and data in I-RAM, minus a cycle per jump; BW-RAM at 5.37 MHz; ROM drops to
5.37 MHz whenever the S-CPU is also on ROM, "even if the SNES only does an
access every 8 master cycles" (SnesSpeedTest notes 1-2, 5). The Mesen numbers
are in that shape. Hardware SnesSpeedTest results add: I-RAM data with the
S-CPU in ROM stays 10.74 MHz; both CPUs on I-RAM gives ~3.7 MHz.

**The SA-1 numbers move with code alignment.** A taken branch to an odd
address in ROM costs the SA-1 a cycle (`Sa1Cpu::IdleTakeBranch`, matching
Vilela's note); one earlier build of this ROM read (a) Ii = 9163, x3.5. Compare
hardware and emulator on the same `.sfc`, never across builds.

## Where Mesen differs, or may differ, from hardware

Proven from the source; the test expects Mesen's values on these lines:

- **No write protection at all** (`Sa1.h`: "TODO: Implement write protection
  flags"). `Sa1IRamHandler::Write` ignores SIWP/CIWP, `CpuBwRamHandler::Write`
  ignores SBWE/CBWE/BWPA: `iramwp FAIL rst=W b3=W`, `bwprot FAIL p0=W ...`.
  Hardware should show `P` for those.
- **Exact negative division** (`Sa1.cpp:CalculateMathOpResult`): it adds the
  divisor to a remainder that is already 0, so -25536 / 3 gives -8513 r 3
  instead of -8512 r 0 (bsnes is right): `div FAIL 4/5 #09=DEBF`.

Simplified, for the hardware run to settle:

- **Overflow flag.** Mesen and bsnes set OF from bit 40 of the 64-bit sum of
  the 40-bit total and the sign-extended product: a sum that goes negative
  sets it. fullsnes: set only on a real 40-bit overflow. `of=` lists all six
  sum records (only the first is judged); Mesen `000101`.
- **Arithmetic latency.** Results are instant; hardware needs 5-6 cycles.
  The ROM always waits, so a FAIL on hardware would not be timing.
- **Bus conflicts** (`Sa1Cpu::ProcessCpuCycle`, `IsAccessConflict`): a wait
  is added when the S-CPU's *last* bus-A access was the same memory type, not
  when the two actually overlap; ROM +1, BW-RAM +2, I-RAM +1 (+1 at FastROM
  speed). The busy figures are where hardware will differ most.
- **Character conversion costs nothing** (`Sa1::ReadCharConvertType1` runs
  inside the DMA read): `ccrate` equals `ccplain`. If hardware converts slower
  than the DMA reads, `ccrate` drops below `ccplain`. Whether a second DMA
  pass over the same bitmap restarts cleanly is also untested on hardware;
  the ROM starts a fresh conversion for each pass.
- **SCNT/CCNT bit 7 = 0 clears** a pending IRQ in Mesen; fullsnes says
  "no change?". The ROM acknowledges through SIC/CIC, so it does not depend
  on it.
- **Version register**: open bus in Mesen and bsnes, `$23` on carts per
  sneslab. Mesen's open bus agreed (`a=23 00=00 80=80`).
- **Timer** (`$2210-$2215`, HCR/VCR) is not emulated, so it is not tested.
- SA-1 reads of I-RAM's `$0800-$0FFF` mirror return 0 in Mesen; probably
  open bus on hardware. Not used.

## On the real cart

The FXPAK Pro's SA-1 is an FPGA reimplementation, not the RF5A123: a
difference from the documents may be the core's. Vilela's speed test has a
photo of an sd2snes SA-1 core run to compare against.

1. Copy `sa1/build/sa1probe.sfc` to the SD card; delete any `sa1probe.srm`.
2. Boot it. Page 1 fills within a second; the verdict appears after about
   seven seconds (page 3, the timing runs, fills last). Expected: `verdict OK`, or FAIL with only
   hardware-vs-document lines in `fails:`.
3. Photograph all three pages (A, B, X, Y, L or R flips them). Lines that
   matter most: page 1 `iramwp`, `vc`, `sfr`, `div`, `sum ... of=`,
   `bwram`, `bwprot`; page 2 every `xx:xxxx` line, `ccrate` against
   `ccplain`; page 3 all of it, especially `Ib`/`Bb` and `a:F`.
4. Power-cycle and photograph page 1 again: `bwboot` must read `boots=2`.
5. If it hangs: the line it stopped after is the culprit; every wait in the
   ROM is bounded, so a hang means a crash, not a missing answer.

## The kernel on SA-1 (phase 1)

`make SA1=1` (2026-11-22) builds the whole system, kernel, apps, ROMFS and
the `mc` world, as `snes/build-sa1/snos-sa1.sfc`; with `LINK_HW=1` it is
`snes/build-hw-sa1/snos-sa1.sfc`. The SA-1 is held in reset all the time; the
change is only the memory map. Without `SA1` both stock images are
byte-identical to `main`'s.

### What changed

- `calypsi/linker-sa1.scm`: linker.scm's RAM forms; the bank-`$00` window
  (`code`, `switch`, `data_init_table`: trap shims, vector targets, Calypsi
  startup, `app_exec`, the app-side syscall stubs) stored at `$C0:0000`,
  header `$C0:7FB0`, vectors `$C0:7FE0`, far code and constants from
  `$C0:8000`.
- `calypsi/header-sa1.s`: map `$23`, type `$35`, RAM `$08`. The vectors and
  shims moved from `header.s` to `calypsi/traps.inc`, which both headers
  `#include` (the assembler runs the C preprocessor; `.include` is not an
  instruction).
- `tools/raw2sfc.py --sa1`: size byte and checksum at `$7FD7`/`$7FDC`.
- `kernel/sram.c`: `sa1_hold()`, first in `hw_init`: CCNT `$20`, SIE 0, SIC
  `$A0`, CXB-FXB 0-3 (dropped 2026-11-23: their reset values), SBWE `$80`,
  SIWP `$FF`, and a write/read of I-RAM
  `$3000` (put back) as the presence test. `sram_ptr()` is `$40:0000 + off`;
  `sram_probe()` steps 8 KB through the first 192 KB only (`SA1_FS_KB` in
  `sram.h`, the one constant). `fs.c` clips its BW-RAM DMA at the bank end.
  `exec.s` writes CCNT `$20` in `snos_restart` before the runtime restarts.
- `shell.c`: three `mem` lines and the `ver` line, `#ifdef SA1`.
- Makefile: `SA1=1`, the build directories, `ROM` (the image name), and
  `MESEN_SRM` rewritten from `snos.srm` to `snos-sa1.srm`: Mesen names the
  save after the ROM file, so neither suite deletes the other's save.
- Tests: `fs.lua` and `restart.lua` asserted 256 KB; they now read
  `@SRAM_KB@`, spliced like `@XFER_LAST@` (256 stock, `SA1_FS_KB` on SA-1).
  New `test-sa1` (in `make SA1=1 test`, refused without `SA1=1`).

Apps, `apps/app.scm.in` and `tools/buildapp.sh` are unchanged: every app
`.raw`, `romfs.bin` and `mcdata.bin` is byte-identical between the two
images. Only ROM banks `$C0-$C1` differ. Nothing in the kernel or the apps
has a literal address in `$2200-$23FF`, `$3000-$37FF` (`console.c`'s `$3000`
is a VRAM word address) or banks `$80-$BF`.

### Where things are

| Kernel region | Stock (HiROM `$31`) | SA-1 (`$23`) |
|---|---|---|
| `code` etc. as the CPU runs it | `$00:8000-$838D` | `$00:8000-$8393` |
| ... stored at (ROM offset) | `$C0:8000` (`$008000`) | `$C0:0000` (`$000000`) |
| header, vectors | `$C0:FFB0-$FFFF` | `$C0:7FB0-$7FFF` |
| far code and constants | `$C0:0000-$7FFF`, `$C1:0000-$FD47` | `$C0:8000-$FFFF`, `$C1:0000-$FF0D` |
| apps, ROMFS, `mc` data | `$C8-$D6` (ROMFS `$CF`), `$E0:0000-$E2:45C8` | same |
| filesystem | SRAM `$A0-$BF:6000-$7FFF`, 256 KB | BW-RAM `$40:0000-$42:FFFF`, 192 KB |
| SA-1 work area | none | BW-RAM `$43:0000-$43:FFFF`, untouched |
| bank `$00` RAM, `$7E`, `$7F` | unchanged | unchanged |

### The cable gates on the new map

The LINK_HW tests were measured on the stock map; run on both images
(2026-11-22, `make LINK_HW=1` and `make SA1=1 LINK_HW=1`, all PASS):

| | stock | SA-1 |
|---|---|---|
| hwspeed download / upload | 3650 / 3375 B/s | 3650 / 3375 B/s |
| hwspeed both ways down / up | 2920 / 2148 B/s | 3066 / 2148 B/s |
| latest checked exchange, HDMA off (gate 131) | line 131 | line 131 |
| NMI unmasked before vblank, at least | 4.0 lines | 4.0 lines |
| hwresponse key / START+SELECT stop | 21 / 47 frames | 21 / 47 frames |
| hwpanel SELECT to panel / RIGHT to highlight | 18 / 4 frames | 20 / 4 frames |
| hwpanel latest checked, HDMA on (gate 128) | line 128 | line 128 |

The kernel code is the same and the ROM speed the same (FastROM, the trap
shims in slow bank `$00` on both), so the gates hold unchanged; the small
differences are where the frames fall in a run, not timing. *(2026-11-23:
the SA-1 image is SlowROM now and these figures are superseded; see
"SlowROM" below.)*

### What surprised

- Nothing in the kernel relied on `$00:8000` = `$C0:8000` beyond the linker
  placement: the full suite passes on the new map with no other change.
- Mesen picks the `$7FC0` header although ROM `$FFC0` now holds kernel code
  rather than `$FF` filler, and loads the cart as SA-1 with a 256 KB battery
  save (`emu.getMemorySize(snesSaveRam)`).
- Mesen's state has the chip in `emu.getState()` as `cart.coprocessor.*`:
  `sa1Reset`, the SA-1's `cpu.pc`, and the S-CPU side registers
  (`cpuBwWriteEnabled`, `cpuIRamWriteProtect`, `cpuIrqEnabled`, `banks[n]`),
  so `test-sa1` asserts on them. `cpu.cycleCount` advances in reset; it
  proves nothing.
- `sram_probe` puts back what it finds, so a final-state check cannot see it
  overrun the limit. `test-sa1` also watches the bus: no S-CPU write to
  `$43:0000-$43:FFFF` or its mirrors, no access to a `$6000` window.
  A probe changed to 32 windows fails it, on the bus watch among others.

### On the real cart

1. `make SA1=1 LINK_HW=1` and copy `snes/build-hw-sa1/snos-sa1.sfc` to the
   SD card (32 columns, keyboard and network through the Pico on port 2;
   `snes/build-sa1/snos-sa1.sfc` is the emulator image and works with the
   on-screen keyboard only). Delete any `snos-sa1.srm` an earlier copy left.
2. Boot it. Expected: `v0.1`, `192K SRAM`, `SELECT opens the keyboard`, the
   prompt. A hang or a black screen here is the map: note whether the
   sd2snes menu named the cart SA-1.
3. `mem`. The last four lines must be:
   `cart SRAM 192 KB, free 190 KB, ...` (wrapped at 32 columns),
   `SA-1 yes, held in reset`, `BW-RAM files 192K at $400000`,
   `BW-RAM SA-1   64K at $430000`.
4. `debug ver`: `65816 native, SA-1 cart slowrom, SA-1 in reset`.
5. `ls`, `cat MOTD.TXT`, `run hello` (`[hello exited 42]`): ROMFS at `$CF`
   and an app at `$C8`.
6. `write KEEP.TXT survived a power cycle`, then `cat KEEP.TXT`. Wait ten
   seconds so the sd2snes writes the save to the card, then power off.
7. Power on, boot it again, `cat KEEP.TXT`: the line must be there. Then
   `restart` and `cat KEEP.TXT` once more.
8. With the adapter up, `get URL DEST` of a small file and `cat DEST`: the
   network path writing BW-RAM.

## The boot beacon (2026-11-23)

On the sd2snes (firmware 1.11.0b1) `snos-sa1.sfc` gives a black screen and
nothing else, while the probe and the stock image work. There is no
debugger on the console, so `BEACON=1` makes the boot report where it got
to: before the console exists, each checkpoint paints the whole screen one
colour and holds it for half a second (`BEACON_HOLD`, 30 frames); after
`kcon_init` the checkpoints are words on one console line. The last colour
on the TV, or the last word, names the last checkpoint reached; the step
after it is the suspect.

    make SA1=1 LINK_HW=1 BEACON=1                   # build-hw-sa1-beacon/snos-sa1-beacon.sfc
    make SA1=1 LINK_HW=1 BEACON=1 BEACON_FASTROM=1  # build-hw-sa1-beacon-fastrom/snos-sa1-beacon-fastrom.sfc

The plain beacon is the SA-1 image as it now ships, SlowROM, with the
colours. `BEACON_FASTROM` sets MEMSEL where the image used to, between cyan
and white, and is otherwise the same image, SlowROM constants included. Until
2026-11-23 there were two others: the plain beacon wrote CXB-FXB and set
MEMSEL, and `BEACON_SAFE` did neither; the next section says what the cart
made of them. `BEACON` without `SA1` is refused.

### What the cart said (2026-11-23)

The owner's sd2snes, firmware 1.11.0b1, with the Pico on port 2:

- The beacon of the time (CXB-FXB written, FastROM on): the colours ran
  through orange, then the screen went black and stayed so, no
  `beacon: con`. So reset, far code, all of `sa1_hold`, MEMSEL, a far call
  and `mb_init`/`stack_paint` ran, and it died in or just after
  `kcon_init`, the first long stretch of code with FastROM on.
- beacon-safe (neither): to the prompt, `beacon: con nmi scan sram fs`,
  `192K SRAM`, the caret. "Super slow, but it loaded"; what was slow was
  the text appearing ("SlowROM" below).

The working conclusion: on this cart the S-CPU cannot run from ROM at
FastROM speed while the cart is an SA-1, as no retail SA-1 game does; the
probe's one short FastROM loop from bank `$80` proved less than it was
taken to. CXB-FXB looked innocent (four checkpoints after them) but were
redundant, so they went too. `snos-sa1-beacon-fastrom.sfc` is the
confirmation: it should die after orange again. If it boots, FastROM was
not the cause.

### The checkpoints

| Screen | Reached | Next step: the suspect if the screen stays here |
|---|---|---|
| black from the start, no colour at all | nothing | the reset vector, the header, the cart not starting |
| red | reset stub (bank `$00`, emulation mode, PPU set up) | C startup: first `jsl` into bank `$C1`/`$C2` at SlowROM |
| green | `__low_level_init`, the first code from `$C0`+ | 43 KB RAM clear and data copy (about 1 s), then `main` |
| blue | `main` | CCNT, SIE, SIC writes |
| yellow | CCNT `$20`, SIE `0`, SIC `$A0` | SBWE, SIWP writes |
| magenta | SBWE, SIWP | the I-RAM test at `$3000` |
| cyan | the I-RAM test | `BEACON_FASTROM`: FastROM on; then a far call to `$C0:FFE0` |
| white | (FastROM on,) the far call returned | rest of `hw_init`, `mb_init`, `stack_paint` |
| orange | `mb_init`, `stack_paint` | `kcon_init`: forced blank, fonts by DMA from ROM, display on |
| colours, then black, no text | (orange was last) | `kcon_init` or its first flush |
| `v0.1` and `beacon: con` | the console | `kbd_init`: NMI on, then 30 frames waited |
| `... con nmi` | NMI handler ran (` NONMI`: NMI on but it never ran) | `app_scan` |
| `... nmi scan` | `app_scan` | `sram_probe`: BW-RAM `$40:0000-$42:FFFF` |
| `... scan sram` | `sram_probe` | `fs_init` |
| `... sram fs`, then the prompt | the shell | |

The full sequence takes about five seconds: red, green, then a second of
green while the RAM is cleared, blue, yellow, magenta, cyan, white, orange,
then text. Filming it with a phone is the easiest record.

Why the words matter: without the beacon, `con_init` turns the display on
with a black backdrop and an empty tilemap, and `kcon_putc` flushes rows
in batches, so nothing is painted until the shell first waits for a key.
A black screen on the plain image therefore covers everything from reset
to the prompt, NMI, `app_scan`, `sram_probe` and `fs_init` included, not
only the steps before the console.

### What each image changes

- The reset vector points at `beacon_reset` (`calypsi/beacon.inc`) instead
  of `__program_start`. It touches no RAM and no stack: forced blank, NMI,
  IRQ, auto-joypad and HDMA off, mode 0, no mosaic, windows, window masks,
  colour math or interlace, nothing on main or sub screen, fixed colour
  black, CGRAM 0 red, display on at full brightness, then the C startup.
- `__low_level_init` (empty in the library) paints green.
- `beacon_far` sits at `$C0:FFE0`, a bank away from `main` (`$C1`): the
  Makefile links the beacon with `linker-sa1-beacon.scm`, made from
  `linker-sa1.scm` by cutting 32 bytes off bank `$C0`. It loops 4096
  times, reading ROM, and returns.
- The C checkpoints (`kernel/beacon.h`) are five register writes and a
  `$4212` poll: no NMI, no library code, the same at either ROM speed.
- The colour checkpoints set PPU state the plain image never sets (mosaic,
  windows, window masks, fixed colour): `con_init` leaves those as power-on
  or the sd2snes menu left them. If the beacon image reaches the prompt
  and the plain one does not, that is the next lead.
- **`BEACON_FASTROM`** sets MEMSEL and so runs everything after cyan at
  FastROM with constants measured at SlowROM: safe for the cable (its
  gates only get more room) but not representative of anything else.

### Under Mesen

`make SA1=1 BEACON=1 test-beacon` (also with `LINK_HW=1` and
`BEACON_FASTROM=1`) watches CGRAM entry 0 and the boot's register writes:
the eight colours in order, each after its step and before the next, red
from bank `$00` before anything else, green from `$C0`+, SBWE and SIWP
before magenta, the I-RAM test's write to `$3000` before cyan,
`beacon_far` entered from another bank, the mailbox up by orange,
`con_init`'s black after orange, then the word line on screen and the
shell prompt. No image may write CXB-FXB; the plain beacon may not write
MEMSEL, `BEACON_FASTROM` must write it once, = 1, before the far call.
`test-boot` times its settling window from power-on, which the holds
overrun, so it runs on `BEACON_HOLD=1` builds (their own directories).
Results for the current variants are under "SlowROM" below.

### On the real cart

1. Copy `snos-sa1-beacon-fastrom.sfc` to the SD card. Boot it and note,
   or film, the colours and the final screen. Expected: through orange,
   then black, as on 2026-11-23. `snos-sa1-beacon.sfc` is the control: it
   should reach the prompt, as beacon-safe did.
2. If either stops at `beacon: con` with no `nmi`, or at the colours, try
   once more with the sd2snes in-game hooks off: the FPGA hooks the NMI
   vector of every game (`cheat.v`, vector `$2A10`), the SA-1 core
   included, and the probe never enabled NMI.

## SlowROM (2026-11-23)

After the cart run above, the SA-1 image is SlowROM: with `SA1=1` the
Makefile defines `SLOWROM`, `main.c` never sets MEMSEL (only
`BEACON_FASTROM` does), and `sa1_hold` no longer writes CXB-FXB. Every
constant that was measured or tuned at FastROM was measured again on the
SlowROM image under Mesen, which charges the S-CPU 8 master cycles for a
ROM fetch here, and given a SlowROM value behind `SLOWROM`. The stock
images (`make`, `make LINK_HW=1`) are byte-identical to `main`'s; every app
`.raw` is byte-identical between the images, and to `main`'s.

### The instrument

`make test-pace` (any image; `PACE="adapter=false"` leaves port 2 empty):
frames from power-on to the prompt on screen, `help` and `cat NET.TXT`
painted, ten keys typed to echoed; behind them the NMI's length, how many
NMIs ran a cable exchange while the console owed VRAM, the row DMAs per
vblank (landed, started in active display and lost, and in vblanks that
ended with rows still owed), `pad_latch`'s entry and length, and
`con_init`'s length. On `LINK_HW` the adapter model of `test/hwport.lua`
answers on port 2 from power-on, as the owner's Pico did.

### Stock FastROM against SA-1 SlowROM

`LINK_HW` images, 32 columns, adapter answering, unless stated. Cable
figures from `test-hwspeed` with `HWPROF=1`, `test-hwresponse`,
`test-hwpanel`.

| | stock FastROM | SA-1 SlowROM |
|---|---|---|
| speedtest down / up / both (defaults) | 3650 / 3375 / 2971 + 2148 B/s | 2482 / 2250 / 2336 + 1636 B/s |
| exchanges per frame in the speedtest | 1.24 | 0.93 |
| NMI, idle prompt (one exchange) | 117.1 lines | 136.7 lines |
| NMI in the speedtest, mean / worst | 120.9 / 162.0 lines | 141.5 / 183.8 lines |
| a frame's clocks, first to last | 76.9 lines (205 mc a bit) | 90.6 lines (242 mc a bit) |
| check to unmask, HDMA off / on | 89.0 / 92.0 lines | 104.1 / 107.5 lines |
| gates, and worst-case unmask before vblank | 131 / 128: 4.0 / 4.1 lines | 115 / 112: 4.9 / 4.5 lines |
| rows per full flush, 32 columns | 8 or 9 (budget 8) | 6 or 7 (budget 6) |
| transfers per full flush, 64 columns | 8 to 10 (budget 4 rows) | 7 or 8 (budget 3 rows) |
| `con_init` (forced blank) | 2979 lines, 11.4 frames | 3497 lines, 13.3 frames |
| power-on to prompt on screen | 93 frames | 94 frames |
| `help` painted, adapter attached | 21 frames | 25 frames |
| `cat NET.TXT` painted (3 pages), adapter attached | 148 frames | 180 frames |
| the same with no adapter | 129 frames | 152 frames |
| `help` / `cat` painted, emulator ROM, 64 columns | 26 / 128 frames | 32 / 155 frames |
| key to echo: mailbox / adapter keyboard | 3 / 6.9 frames | 3 / 5.0 frames |
| hwresponse key / START+SELECT stop | 21 / 47 frames | 15 / 46 frames |
| hwpanel SELECT to panel / RIGHT to highlight | 18 / 4 frames | 16-17 / 4-6 frames |

Text is 19-23% slower, `con_init` 17%: the honest cost of SlowROM for
-O0 code. Boot to prompt does not move, because the C startup's 43 KB RAM
clear (69 of the 93 frames) runs before MEMSEL is set on either image.
The cable loses a third, and that is not the per-exchange cost: at one
exchange a frame stock does 2628 / 2352 / 2482 + 1739
(docs/link-throughput.md), 6% above SlowROM. It is the second exchange,
which no longer fits: the NMI with its exchange (141 lines) and a
foreground poll (128) are more than a frame. `net tune exch 1` and the
default 2 measure the same at SlowROM; 2 stays, because a frame whose NMI
left the link to the console still gets an exchange from the foreground
(the hwresponse key, 15 frames). Every NMI ends by line 152, so none is
still running when the next vblank begins; `pad_latch` starts 0.44 lines
into the NMI and spins 3.5 lines on both images, nowhere near its guards
(which, being pass counts, only cover more time at SlowROM).

### What was retuned

| constant | where | stock | SlowROM | from |
|---|---|---|---|---|
| `CON_FLUSH_ROWS` | console.h | 4 | 3 | full flushes landed 7 or 8 transfers |
| `CON_FLUSH_ROWS_NARROW` | console.h | 8 | 6 | full flushes landed 6 or 7 rows |
| `XFER_LAST` | link_serial.c | 131 | 115 | check to unmask 104.1 lines |
| `XFER_LAST_HDMA` | link_serial.c | 128 | 112 | 107.5 under HDMA |
| `FG_LAST` | link_serial.c | 100 | 79 | 35.6 lines of C before the clocks |
| `QUIET` (lead-in turns) | linkxfer.s | 220 | 183 | 36 master cycles a turn, ~307 us |
| `POLL_LAST` (mailbox poll) | input.c | 134 | 122 | longest mask 94.8 lines (82.0 stock) |

Each was measured with the gate or budget raised, then set to leave the
margin stock has (four lines before vblank for the cable, the least full
flush for the console). Checked and left alone: the keyboard panel's row
DMAs (one row a call, already checked against vblank: 16 frames to paint
the panel), the HDMA split (no CPU timing), the kick (eight clocks ~6.9 us
apart, still well inside the Pico's 64 us gap timer), the exchange budget
`EXCH_MAX` 3 and default 2, `net tune window` 4, `pad_latch` and `win_open`
guards, and the apps: `vnc` closes its write window by line 213 on both
images (`test-vnc`, 0 blanks outside 192-224), `view` works in forced
blank, `speedtest` and `mc` count frames, not cycles. `apps/wire` links
its own copy of `linkxfer.s` without `SLOWROM` and keeps 220 turns, ~368
us at SlowROM, which the adapter takes as it takes the shorter one. The
Pico firmware is unchanged: its gap timer is 64 us and the longest gap
between clocks at SlowROM is ~11 us.

One bug, not a constant: after spawning a background job the shell waits
a second without flushing, and the NMI leaves the link alone while a row is
owed, so a dirty row held the job's first datagram for that second. At
SlowROM the echoed command was still dirty when the wait began, and
`test-netreset`'s background `get` sent its DNS query after 70 frames
instead of 11 (the test allows 30). `kcon_sync` before the wait fixes it.
The race is the stock image's too; the fix sits under `SLOWROM` only to
keep that image byte-identical, and should lose the `#ifdef` when stock
may change.

One test value differs on the SlowROM image and is spliced from the build,
like `@SRAM_KB@`: `test-hwspeed` asks for at least one foreground exchange
on stock and none at SlowROM (`@SPEED_FG_MIN@`); the gate checks still
judge any there are. `@XFER_LAST@` and `@XFER_LAST_HDMA@` are read from the
build's side of `link_serial.c`'s `#ifdef`.

### Why it was "super slow"

The owner's run was `snos-sa1-beacon-safe.sfc` with the Pico attached.
Under Mesen, the same image with the adapter model answering:

- **The beacon itself.** Prompt on screen at frame 399 (6.7 s), against
  94 for the plain SlowROM image. Eight colours held 30 frames each (4 s),
  the 30-frame NMI check, and each word checkpoint flushed alone: the
  console line came out a word at a time over 73 frames (1.2 s), where the
  plain image paints everything at once at frame 94. That is 305 frames,
  5.1 s, the whole difference to the plain image.
- **Shell text, ~20% slower than stock**: `help` 25 frames against 21,
  `cat NET.TXT` 181 against 148. Part of it was the FastROM budget of 8
  rows a flush at 32 columns: at SlowROM only 6 or 7 fit, and 10 of 103
  row DMAs in `cat` started in active display and were redrawn a flush
  later. With 6, none is lost and `cat` takes 180; the rest is SlowROM.
- **The cable timing mismatch** did not slow text: the NMI hands the vblank
  to the console whenever a row is owed, and in every run no NMI ran an
  exchange while one was. It mattered during transfers: with the FastROM
  gates a foreground exchange could still hold NMI masked when vblank
  began (2-3 times in test-hwresponse and test-hwpanel), the hazard that
  can cascade into nested NMIs. The SlowROM gates remove it.

So Mesen explains about 5 s of beacon and a fifth on top of stock for
text, and nothing slower. If the plain SlowROM image still feels much
slower than that on the cart (`help` should take ~0.4 s), the cart is
doing something Mesen does not model, and timing `help` and `cat NET.TXT`
there against the figures above is the next measurement.

### Results (2026-11-23)

`make -k SA1=1 test`: 33 RESULT PASS, no make errors. `make SA1=1
test-vnc`: PASS, latest blank close line 213. `SA1=1 LINK_HW=1`:
`test-hwspeed`, `test-hwresponse`, `test-hwpanel` PASS; `test-hwspeed` on
the stock `LINK_HW` image PASS after the test change. `test-beacon` PASS
on `BEACON=1` and `BEACON=1 BEACON_FASTROM=1`, with and without `LINK_HW`;
`test-beacon`, `test-boot` and `test-shell` PASS on both `BEACON_HOLD=1`
`LINK_HW` builds. Stock `make` and `make LINK_HW=1` images byte-identical
to `main`'s, every app identical across all four images and to `main`'s.

### For the next cart run

- `snes/build-hw-sa1/snos-sa1.sfc`: the SlowROM image, Pico on port 2.
  Time `help` (Mesen: 25 frames, ~0.4 s) and `cat NET.TXT` (180 frames
  with its two page stops), then `net speedtest` (Mesen: 2482 / 2250 /
  2336 + 1636).
- `snes/build-hw-sa1-beacon-fastrom/snos-sa1-beacon-fastrom.sfc`: should
  stop after orange. If it boots, FastROM was not the cause.
- `snes/build-hw-sa1-beacon/snos-sa1-beacon.sfc`: the control, should
  reach the prompt.

## The cable on the cart (2026-11-23)

The SlowROM image boots on the owner's sd2snes (firmware 1.11.0b1) and
shows `192K SRAM`, but the Pico on port 2 never gets a frame: its `stats`
show one 16-clock burst a frame (the auto-joypad read), `frames=0`, no
512-clock exchange and no 8-clock kick since power-on, and the console's
`net status` says "no link". The stock `LINK_HW` image on the same cart,
cable and Pico links at once. Under Mesen with the adapter model the SA-1
image links too. So on the cart `link_serial_xfer` is never reached.

### Every way the cable can stay silent

At the idle prompt with the link down, two callers move the probe counter
(`probe_wait`, one probe every 60 calls): the NMI (`net_service` →
`net_up` → `link_serial_present`) whenever the console owes no rows, and
the foreground every frame (`kbd_idle` → `ip_service` → `net_up`). Two
calls a frame means the 60th always falls in the same context: on the
plain SA-1 image under Mesen every probe is the foreground's, at line 3
(of the next frame); on stock at 259; an odd number of skipped NMIs (a
row printed, a key typed) moves them to the other context. A refused
probe waits another 60 calls and lands on the same line: the trap is
real and deterministic. What it needs is a gate reading inside 116-228.

| condition | under Mesen, idle, link down | can the cart differ? |
|---|---|---|
| `net_ready` 0 | set by `shell_run` before the prompt | no: no timing or hardware in it |
| `con_dirty_pending()` at every NMI | 10-12% of NMIs skip (boot text, the diagnostic's repaints) | only with the foreground blocked too, which still probes every second; text that paints says rows land |
| the WRAM backend (magic at `$7EE000`) | zeroed at boot, nothing writes it | then `net status` says "link up, no answer", not "no link" |
| the gate refuses the probe | NMI probe at line 238 (diagnostic image), foreground at line 3: 9 and 112 lines inside the allowed zones | only if the cart ran the NMI's C 10 lines *faster* than Mesen, or the foreground 110 lines slower, or `beam_line` lies |
| `beam_line` wrong | V reads 225 at NMI entry, gate and vblank flag agree | `$2137` latches only with `$4201` bit 7 set: nothing in SNOS writes `$4201`, the sd2snes menu leaves `$FF` (`snes/main.a65` `snes_init`), pin 6 is not in the cable; neither FPGA core decodes `$2137`/`$213C`/`$213D` (`address.v`) |
| `pad_latch` after the auto-read | entered at 225.44, read over at ~228.4: 3 lines to spare | needs the NMI 3 lines late; the idle loop masks NMI only right after the NMI (`pad_pressed`, `ip_service`). A scratch build delaying every NMI by 4 lines does not silence the link: the machine hangs (each burnt guard is 334 lines at SlowROM, 273 at FastROM, longer than a frame, and the nested NMIs cascade) |
| the sd2snes in-game hook | not modelled | ~0.25 line before our NMI, no pad wait (below); a scratch build with that delay links normally |
| the SA-1 core's S-CPU bus timing | not modelled | 1.11.0 lacks "fix: SA-1 bus sampling" (`bea50dd`, 2026-04-27, released in v1.11.1): "too late ROMSEL sampling leading to setup violations for SNES CPU reads". It is what killed FastROM; how much margin SlowROM has on that core is unknown |

None survives the analysis as a cause on its own. The one hardware-only
actor left is the SA-1 core of firmware 1.11.0 itself, and no Mesen
setting models it. The diagnostic below tells the candidates apart on a
photo.

### The sd2snes in-game hook (v1.11.0 sources)

- Off by default: `src/cfg.c` `enable_ingame_hook = 0`; `enable_ingame_buttons
  = 1`, `enable_hook_holdoff = 1` (hooks wait 10 s after reset). Menu:
  *In-game Settings* → "In-game hook", " In-game buttons", " Initial
  holdoff" (`snes/const.a65`, `snes/menudata.a65`). `src/cheat.c`
  programs the FPGA from them on every game start, SA-1 included.
- When on, the FPGA watches for the CPU's four stack pushes followed by the
  `$00:FFEA` fetch and answers `$2A10` (`cheat.v`, `cpu_push_cnt == 4`,
  `nmi_match_bits`). `snes/nmihook.a65` there, from FPGA block RAM at
  `$2A00-$2BFF` (6-cycle region): `php; rep; pha; lda $4218` (the last
  frame's pad 1, no `$4212` wait), `sta $2BF0`, a branch the FPGA fills
  in. With buttons on, auto-read on (the FPGA snoops `$4200` bit 0:
  `main.v` `snes_ajr`, set by our `$81`) and no combination held, it goes
  straight to `nmi_exit` (or the WRAM cheats first, if any): `rep; pla;
  plp; sta $2BFD` (unmaps itself), `jmp ($FFEA)`. About 40 CPU cycles,
  0.2-0.3 line. The manual pad read (latch on `$4016`, 16 reads of
  `$4016`) runs only if the FPGA saw auto-read off.
- SA-1 core against base core: the same hook code; the SA-1 core's
  `cheat.v` lacks the save-state and USB-exe branches and the IRQ hook.
- Where the beam is (Mesen, SA-1 / stock): C entry 225.28 / 225.28,
  `pad_latch` 225.44-228.95 / 225.42-228.79. The hook moves both entries
  ~0.25 line later; the auto-read starts at 225.1-225.3 and ends ~228.4,
  so `pad_latch` still finds it running, at either speed. It cannot burn.

One more difference the cores have, harmless to the link: "Auto region
patch" (`CFG_R213F_OVERRIDE`, on by default) on the SA-1 core keeps its
`$213F` state in a 2-bit register against 4-bit one-hot states
(`verilog/sd2snes_sa1/main.v`, `r213f_state`), so after the first read
every `$213F` read is driven by the FPGA with a constant (`$45` on NTSC)
over STAT78. SNOS reads `$213F` only to reset the counter read toggle,
which the PPU still does.

### The link diagnostic

    make SA1=1 LINK_HW=1 LINKDIAG=1   # build-hw-sa1-linkdiag/snos-sa1-linkdiag.sfc
    make LINK_HW=1 LINKDIAG=1         # build-hw-linkdiag/snos-linkdiag.sfc (stock)

Boot it and wait; nothing to type. Nine rows from screen row 13 repaint
about once a second (only the cells that changed), counters since power-on:

    LINKDIAG sa1 slowrom  t 28        t: seconds shown
    nmi 1680 net 1503 skip 177        NMIs; ran net_service; skipped, rows owed
    be - rdy1 up0 pw7 clk 51          backend -/W/S; net_ready; up; probe_wait; exchanges clocked
    prb n2 f49 ref 0 @0f              probes from the NMI / foreground; refused; last refused line+context
    gate n238-238 f3-10 bl0           lines read at the gate, NMI / foreground; latch vs vblank-flag mismatches
    in v225-225 h184-198 s81 fE3      NMI entry: V, H (dots), $4212, STAT78 after the latch
    pad229-229 g12-13 n6-122 b0       V after pad_latch; its guard; NMI length in lines; guards burnt
    ok 0 bad 0 rx 0                   replies that checksummed; net bad sums; frames received
    hdr 00 00 00 00 00 00             last good reply: seq ack flags sum, first record bytes

`kernel/linkdiag.c`; the NMI only stores (the entry sample, before
`pad_latch`, is `kernel/ldentry.s`), the foreground formats. Cost under
Mesen on the SA-1 image: `pad_latch` starts 0.6 line later (0.5 on
stock), the gate 0.9 later, and the NMI ends 5.9 lines later with the
link down, 10.3 with an exchange. Its repaints dirty rows, so they move
the probe parity: the diagnostic image may link where the plain one does
not, and that is itself an answer. Every other image is byte-identical
with and without the change.

`make LINK_HW=1 LINKDIAG=1 test-linkdiag` (also `SA1=1`, `LDIAG="adapter=false"`)
prints the panel under Mesen. After 30 s:

| | SA-1, adapter | SA-1, none | stock, adapter | stock, none |
|---|---|---|---|---|
| nmi / net / skip | 1680 / 1483 / 198 | 1680 / 1503 / 177 | 1680 / 1504 / 177 | 1680 / 1529 / 151 |
| be, up, clk | S, 1, 1455 | -, 0, 51 | S, 1, 1480 | -, 0, 51 |
| prb n / f, ref | 0 / 2, 0 | 2 / 49, 0 | 0 / 2, 0 | 49 / 2, 0 |
| gate n / f, bl | 0-254 / 3-10, 0 | 238 / 3-10, 0 | 1-254 / 3-255, 0 | 236 / 3-255, 0 |
| in v, h, s | 225, 184-197, 81 | 225, 184-198, 81 | 225, 172-185, 81 | 225, 172-184, 81 |
| pad, g, n, b | 229, 12-13, 6-172, 0 | 229, 12-13, 6-122, 0 | 228-229, 15-17, 6-147, 0 | 228-229, 15-17, 6-104, 0 |
| ok / bad / rx | 1454 / 0 / 1453 | 0 / 0 / 0 | 1479 / 0 / 1478 | 0 / 0 / 0 |

How to read a photo against those:

- `in v` not 225, or `bl` above 0: the beam latch lies on the cart.
- `ref` above 0: the gate refused probes, at the line after `@`, in the
  NMI (`n`) or the foreground (`f`); `gate` shows where they read.
- `net` near 0 with `skip` near `nmi`: the console owes rows at every NMI.
- `prb` both 0 while `net` grows: the probe never comes due.
- `clk` growing while the Pico shows `frames=0`: the clocks start and do
  not reach the Pico.
- `g` up to 2000 or more, `b` above 0: `pad_latch` burnt its guard (a
  late NMI). `n` far above the reference: a long NMI.
- `h` some 60-110 dots above the reference: the in-game hook is running.
- `f45` (constant) on the SA-1 image against a changing byte on stock: the
  SA-1 core's region patch (harmless, above).
- `be W`: the mailbox backend; `rdy0`: the network never initialised.

### For the cart

Photograph the panel of `snos-sa1-linkdiag.sfc` after about 30 seconds,
with the Pico's `stats` line at the same moment; then the same for
`snos-linkdiag.sfc`. The difference between the two photos is the answer.
Delete any `*-linkdiag.srm` first.

Without a new ROM, on the plain `snos-sa1.sfc`, watching `frames=` in the
Pico's `stats` (rising ~60 a second and `kbd=` draining means linked):

1. *In-game Settings* → "In-game hook" Off (if it was on). Links: the hook
   was it, against the analysis above.
2. *In-game Settings* → "Auto region patch" Off. Links: the FPGA's drive
   on `$213F` matters after all.
3. *In-game Settings* → "Reset patch for clock phase" Off, and
   "1CHIP transient fixes" Off. Links: one of the reset-loop or `$2100`
   patches.
4. Firmware v1.11.1 or later (the SA-1 bus-sampling fix and the SA-1
   speed fix). Links: the 1.11.0 SA-1 core's S-CPU timing; then
   `snos-sa1-beacon-fastrom.sfc` is worth one more try.

Still `frames=0` and `clocks=16` after all four: only the diagnostic's
photo can say more.
