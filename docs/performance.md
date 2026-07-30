# Where the time actually goes

Measured on 2026-07-28 against `snos.sfc` at `4105de1`, by timing frames from
"command submitted" to "prompt returned". Numbers first, opinions after.

| operation | frames | seconds | note |
|---|---|---|---|
| any command, fixed overhead | 16 | 0.27 | before any work happens |
| `apps`, `ls` | 21 | 0.35 | ~5 lines |
| `help` | 32 | 0.53 | ~20 lines |
| `debug fstest` | 95 | 1.58 | 40 000 SRAM byte ops |
| BASIC printing 100 lines | 191 | 3.18 | 175 frames of output |

Two figures fall out of that, and they are the ones worth designing against:

- **Console output costs ~1.75 frames per line** (175 frames for 100 lines).
  That is ~34 lines/second, and it dominates everything textual.
- **SRAM moves ~30 KB/s** (40 000 byte operations in 79 frames of work).

## 1. Console output — done, and the 3.5x estimate was optimistic

Batching landed. Measured A/B on the same binary:

| | before | after |
|---|---|---|
| `help` (13 lines) | 30 frames | **21** |
| `cat NET.TXT` | 57 frames | **47** |

**1.2-1.4x, not 3.5x.** The estimate assumed the display was the only cost. For
`cat` it no longer is — SRAM reads at ~30 KB/s now dominate, so making the
screen faster stopped helping.

Two things were wrong in the old note. `con_flush` moves **eight** rows, not
four — and the four was never a hardware limit at all: `dma_vram` writes $420B
and the CPU stalls until that one transfer finishes, so the transfers were
always sequential and the per-row DMA channel bought nothing. The channel tables
rationed a resource that was not scarce, and rationed the keyboard split down to
*two* rows a flush for no reason. One channel does all of them now.

Raising the budget alone changed nothing measurable, which is the useful part:
nothing accumulated. `kcon_putc` flushed on EVERY newline, so exactly one row
was ever dirty when the flush ran — a flush capable of eight moved one and paid
a whole vblank for it. The budget only started to matter once flushing waited
for a batch.

**What is left here is the cadence, not the batch size.** Flushes land about
four frames apart: `kbd_idle` waits a frame, then `con_flush` deliberately waits
for the *next* vblank rather than using the one it is standing in. That wait is
a guard, not an oversight — NMI fires at the top of the same window and a DMA
that overruns lands during active display where VRAM writes are ignored, which
presents as a frozen screen and no other symptom. Removing it means making the
flush know how much vblank remains. That is the next real win and the riskiest
change in the console.

## 2. SRAM block I/O

`fs_getc`/`fs_putc` recompute a far pointer, a bank and a window offset per
byte. At ~30 KB/s a 43 KB image takes 1.4 seconds of pure copying.

A block API that computes the pointer once per 8 KB window and walks it would
cut most of that. The filesystem is the only caller, so the change is contained.

Not urgent for downloads — the link runs at 3.4 KB/s, so SRAM is nowhere near
the bottleneck there. It matters for images, and for anything that reads a whole
file at once.

## 3. SRAM → VRAM directly, for the image viewer

Worth stating explicitly because it is easy to miss: **cartridge SRAM is on the
A-bus and VRAM is reachable through the B-bus at $2118, so DMA can go straight
from one to the other.** An image does not need to pass through WRAM or through
C at all.

A 43 KB image is six 8 KB windows — a handful of DMAs, a few frames, instead of
~1.4 seconds of per-byte copying. If the viewer is loading through `fs_getc`,
this is the difference between "instant" and "watch it happen".

## 4. Fixed 16-frame command overhead

0.27 s before any command begins, from `kbd_idle` waiting a whole frame per
poll of the mailbox. Barely perceptible interactively, and it would matter only
for scripted use. Low priority, listed so the 16 frames in every measurement
above are accounted for rather than mysterious.

## 5. Link throughput — measured, and the earlier figures were wrong

I had been quoting ~1.7 KB/s and ~3.4 KB/s. Both were wrong. Measured
end to end, fetching a 3215-byte file through the live gateway:

| | |
|---|---|
| throughput | **~590 B/s** |
| link exchanges | **58.2/s** — essentially the full 60 Hz frame rate |
| payload per exchange | **~10 bytes** of a 64-byte frame |

So the link is not stalling at all. It runs at frame rate and the frames are
about 85% empty. Two separate causes, and the second is the interesting one:

**Stop-and-wait.** `gateway.py` keeps a single in-flight frame (`awaiting` is a
boolean), so a *data* frame can only leave on every other exchange. That alone
halves it.

**A window does not fix it, and I proved that the hard way.** I implemented one,
measured a 3.2x speedup, and it was false — my benchmark detected completion by
looking for `"> "`, which also matches the `"> "` inside `"-> "`, so it was
timing how fast the download *started*. With the detection corrected, the
windowed gateway stalls after 2 KB and the file lands at zero bytes.

The reason is structural: `net.c` accepts a frame only when `seq == rx_next`,
strictly in order, and **the WRAM link mailbox holds exactly one frame per
direction**. Send two frames before the SNES polls and the second overwrites the
first; the SNES then sees a sequence gap, refuses to ack, and the sender wedges
with the window full. Stop-and-wait was masking a bridge that has no flow
control of its own.

So the order is: **fix the bridge handshake first** — the host must not write a
new frame until the SNES has consumed the previous one — *then* a window becomes
possible. Doing it the other way round is what produced the 2 KB stall.

Reverted. A correct 590 B/s beats a broken 1820.

### Measured properly, and the earlier guess about packing was wrong

A repeatable benchmark now exists: `get file:///tmp/bench.bin` fetches a fixed
8192 bytes with no network in the path, so it times the link and nothing else.
Three runs: 799, 800, 789 frames — **615 B/s, reproducible to about 1%**.

Frame accounting over that transfer, counted in the gateway:

| | |
|---|---|
| polls | 600 |
| frames carrying new data | 120 |
| payload in those frames | 7198 bytes — **60 of a possible 60 each** |
| retransmissions | **355** |

So the frames are already completely full, and the earlier note guessing that
"something upstream of build() is feeding it in small pieces" was wrong. The
link is **round-trip-bound**: a frame takes about five emulated frames to be
acknowledged through two files and two processes, and stop-and-wait sends
nothing while it waits. 59% of all polls are retransmissions.

### The obvious fix does not work, and it is worth knowing why

A round-trip-bound link is fixed by carrying more per round trip, so: raise
`LINK_FRAME` from 64 to 256. One constant on each of four sides, no protocol
change. It builds and links cleanly — the frame buffers are in far WRAM, not the
bank $00 budget — and then **the kernel dies the moment the link is claimed**.

    *** BRK ***
    PB:0001 PC:0051  D:1B00  DB:00AB

Executing at $01:0051 is the WRAM mirror — the CPU jumped into the kernel's own
direct page, which means a return address was smashed. `boot` and `shell` pass
throughout, because neither claims the link; only the paths that actually
exchange frames crash. 128 fails the same way, so it is not a marginal budget
that one more byte tips over.

The likely cause is NMI re-entrancy: `net_service` runs in NMI and copies the
frame in and out with a C loop at `-O0`, so quadrupling the frame quadruples
that copy, the handler overruns the frame, and the next NMI re-enters it on the
same direct page and stack. That is a hypothesis, not a measurement — what is
established is that the change is not the one-line win it looks like.

**Doing it properly means making the copy cheap first**: `MVN` moves a block at
~7 cycles/byte against something like 40-100 for a `-O0` far loop, which is the
same "hand-written 65816 for the hot loop" pattern the rest of this family uses.
With that in place a bigger frame — or a real window, which additionally needs a
mailbox deeper than one frame per direction — becomes affordable.

Reverted to 64 rather than left half-done. A correct 615 B/s beats a 2400 B/s
that panics.

**RESOLVED 2026-07-30 — and the re-entrancy hypothesis above was right.** With the
copy in `MVN` and the checksum hand-written (`kernel/netasm.s`, `91ec6ab`), the
NMI handler went from a mean of 144.8 scanlines to 41.3, and `LINK_FRAME 128`
went from instant death to a clean 12/12 (`9a68863`). The frame buffers also sat
at mailbox `+0x10` and `+0x50` — exactly `LINK_FRAME` apart at 64 — with that
literal repeated in four files, so any bump silently overlapped TX and RX in all
of them; the RX offset is now derived (`c07e8e0`).

**256 does not panic. It is a data bug.** At 256 the handler is mean 54.4
scanlines, worst 108.3, re-entry 0 — comfortably inside a frame, nothing smashed.
What fails is `net` (downloads never complete, "2 copies of the body seen") and
`pager` never reporting at all, and the failing sub-tests are specifically the
ones that deliberately drop frames mid-window, so the retransmission path is
where to look. Already ruled out: the record length field (max payload 250, fits
a byte), `CMSG_MAX` (288, bounds-checked), the rings (1024/512/512 — one frame
fits), and any hardcoded payload size in the test gateway. So the sentence above
about 2400 B/s "that panics" is no longer true, and neither is the folklore it
became.

**But bigger frames buy nothing end-to-end, which is the more useful result.** A
48 KB `vnc` screen through the real bridge, real `gateway.py` and a real VNC
server takes 44.33 s / 2660 frames at `LINK_FRAME` 64 **and** 44.33 s / 2660
frames at 128 — bit-identical. The hermetic `make test-vnc` harness shows 1.66×
for the same change because it has no file hop and no second process; it is
frame-size-bound where the real path is round-trip-bound. **Never quote a
`test-vnc` speed-up as a user-visible one.** The conclusion of the section above
stands and is now the whole story: this link is round-trip-bound, only about one
poll in seven carries data (`get` 5.0 polls per data frame, `vnc` 7.3), and
`gateway.py`'s `next_out()` is stop-and-wait on its own send side. A window, not
a bigger frame, is the lever.

**Real hardware is a different question.** None of this constrains the cable: a
synchronous exchange has no file bridge, no 1-deep mailbox and no 60 Hz ceiling.
These numbers measure the emulator path, which is the one being developed
against — worth remembering before optimising for a bottleneck that will not
exist at M7.

## On `-O0`

It stays. Calypsi miscompiles at `-O2` with setjmp-heavy code and `--speed`
miscompiles outright, both documented in the sibling projects. The established
pattern in this family is **correct C first, then hand-written 65816 for the
hot loop** — that is exactly what `snespdp11` did for its dispatch loop and
`snesmac` for its interpreter.

If anything here justifies that treatment it is the console row-build in
`con_flush` and the SRAM window walk. Neither is worth doing before the
structural wins above, which are larger and cheaper.

## Suggested order

1. Console batching — biggest effect, smallest change, touches everything.
2. SRAM→VRAM DMA in the viewer — only if the viewer needs it; check first.
3. SRAM block I/O — after 1 and 2 show whether it still matters.
4. Everything else — measure again before believing any of it.
