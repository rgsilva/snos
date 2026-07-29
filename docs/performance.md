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

## 1. Console output — the big one

`kcon_putc` calls `con_flush` on every newline, and `con_flush` waits for a
*fresh* vblank edge. So every line costs at least one frame, and 1.75 in
practice. Meanwhile `con_flush` is capable of **four rows per vblank** — so
flushing per newline wastes three quarters of its capacity.

**Batch it.** Flush when four rows are dirty, and unconditionally before
blocking for input. Output would then appear in bursts of four lines, and the
user would never see a stale screen, because they only ever look at it when the
program pauses for input or the screen fills.

Estimated: 175 frames → ~50 for the same 100 lines. **Roughly 3.5×**, on every
text operation in the system — `help`, `cat`, BASIC, the download dots, and the
browser when it exists.

Lowest risk on this list: `con_flush` already handles multiple rows; only the
decision of *when* to call it changes.

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
