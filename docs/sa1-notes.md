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
