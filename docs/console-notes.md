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
Both fonts are baked at build time by `snes/tools/mkfont.py` from
font8x8_basic, and `con_init` DMAs them from ROM. (They were once computed
in `con_init`, which cost about half a second of blank screen on every app
exit.)

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

## 32 columns: Mode 0, BG2 alone (2026-10-02)

On the real CRT, over composite and a cheap AV-to-HDMI adapter, the 64
columns were hard to read: the one-hires-dot gaps inside m, w and e are finer
than composite carries, so they fill in and shimmer. Pure green ($03E0) made
it worse, being the lowest-luma, highest-chroma colour on offer.

The fix needed no new font and no new VRAM. In Mode 0 with 8x8 tiles a cell
fetches only tile N, and BG2's tile 2c is already the `[glyph]` half of its
pair. So the same font, the same tilemaps ($0000 text, $0800 panel), the same
scroll ring and the same keyboard HDMA split all carry over; 32 columns by 28
rows. What differs:

- `$2105=$00`, `$212C=$02` (BG2 only), `$212D=$00`.
- BG2's palettes start at CGRAM 32 in Mode 0, so the text colour lives in
  CGRAM 33 as well as 1. Both are always written; forgetting 33 is black on
  black, which a VRAM-reading test cannot see.
- One DMA per row instead of two, so a vblank moves 8 rows instead of 4
  (measured; see `CON_FLUSH_ROWS_NARROW` in `kernel/console.h`).

The shadow buffer stays 64 wide with a live `cols` governing wrap and bounds;
columns 32-63 just stay blank. That keeps `SYS_CONROW`'s 64-byte contract
(telnetd sees blank right halves), keeps the ring index a shift, and needed no
ABI bump, because `SYS_SCREEN` already reported the live width.

`mode` in the shell switches width (`32`/`64`) and colour (`white`, `green`,
`mint`, `amber`). A width change clears the screen: the ring holds no record
of where lines broke, so scrollback cannot be re-wrapped honestly. Apps
flagged `APP_MODE_<name> := 64` in `snes/Makefile` (edit; irc lays itself out for
either width) get 64
columns while they hold the screen and give the shell its mode back when they
exit, are killed, or leave `fg`. The hardware ROM boots 32 columns in white;
the emulator ROM keeps 64 green for the tests (`make CON_COLS=32
CON_COLOUR=white` previews). The mode is published at `$7EFDF0` for the
harness, which decodes one layer or two to match.

## Layout recap

32x32 tilemap per layer used as a 28-row visible ring; scroll bumps BG1/BG2
VOFS by 8 and clears the wrapped row (one dirty row per scroll, not 28).
Own module; keyboard/ and basic-rom/ untouched.
