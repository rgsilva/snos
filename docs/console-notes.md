# Console: 64-col Mode 5 hires — working — 2026-07-22

`docs/assets/v6-shell.png`: 64 columns of readable green text on the
SNES. This is the PLAN's preferred console (64 col @ 8px), replacing the
interim 32-col Mode 1.

## How 64 cols works (Mode 5 two-layer, emu8086 technique)

Mode 5 is 512-dot hires; each tilemap cell is 16px and fetches a tile PAIR
(N, N+1). We split each cell into two text columns across two layers:
- **BG2 (2bpp), tilemap $0000, chars $1000**: EVEN text columns. Font per char
  = `[glyph][blank]` -> glyph in the cell's LEFT 8px.
- **BG1 (4bpp), tilemap $0400, chars $3000**: ODD text columns. Font per char
  = `[blank][glyph]` -> glyph in the RIGHT 8px.
Transparency + priority composite them into 64 full 8px columns. A map entry
for char c is `(2c) | $2000` (priority). Registers (from emu8086/dosboot.asm):
`$2105=$05, $2107=$04(BG1SC), $2108=$00(BG2SC), $210B=$13, $212C=$212D=$03`.
Both fonts are built in `con_init` from font8x8_basic (no external tool).

## The two bugs that cost the most (both about vblank timing)

1. **Only even columns showed** at first: a single BG on the main screen gets
   only the even hires dots (main=even, sub=odd), so each glyph was half-width
   ticks. Fixed by the BG1+BG2 pair above.
2. **Odd columns (BG1) blank** even after the pair was set up: writing 64
   tilemap words per row via C `$2118` stores overran vblank, so the 2nd half
   (BG1) landed in active display and was dropped. The chain of red herrings:
   truncation looked like "BG1 not rendering", then "2nd DMA fails", then
   "channel reuse". **Real cause: the slow -O0 buffer *refill* between DMAs ate
   the vblank budget.** Fix: build both row buffers OUTSIDE vblank, then do
   only the two fast DMAs (channels 0 and 1) INSIDE vblank, one row per
   con_flush call. VRAM dumps (`test/vmap.lua`) were what finally pinned it.

## Layout recap

32x32 tilemap per layer used as a 28-row visible ring; scroll bumps BG1/BG2
VOFS by 8 and clears the wrapped row (one dirty row per scroll, not 28).
Own module; keyboard/ and basic-rom/ untouched.
