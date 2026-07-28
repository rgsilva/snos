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

Also unexplained and worth a look: even allowing for stop-and-wait, a data frame
carries ~20 bytes of a possible ~58. Something upstream of `build()` is feeding
it in small pieces.

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
