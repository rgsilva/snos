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

## 5. Link: the SNES → gateway direction is still stop-and-wait

The gateway → SNES direction was fixed (~2.5×) by pipelining the gateway. The
reverse direction carries only commands today, so it is invisible — but IRC
sends as much as it receives, and `OPEN` will make this the first thing anyone
notices.

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
