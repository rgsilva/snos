;;; The Mode 3 bitplane transpose, by hand.
;;;
;;; WHY THIS FILE EXISTS. RFB delivers linear pixel bytes and the SNES stores
;;; 8bpp as eight bitplanes, so every tile `vnc` paints has to be turned inside
;;; out: 64 pixel bytes in, 64 plane bytes out, 512 bit extractions. At -O0 that
;;; measured 333k master clocks a tile -- 0.93 of a FRAME each, seventeen seconds
;;; for a 768-tile screen -- which made it the largest single cost in the app
;;; once the link work of M7 had taken the wire below it.
;;;
;;; It stays on the console deliberately. tools/gateway.py is a dumb pipe for
;;; `vnc` -- pump() is sock.recv(512) straight through -- so the SNES does its own
;;; RFB handshake, its own 3:3:2 negotiation and its own tile building, and is a
;;; real VNC client rather than a display for tiles somebody else interleaved.
;;; Sending pre-interleaved tiles from the Mac would have made the gateway the
;;; client. This file is what buys the speed without giving that up.
;;;
;;; ------------------------------------------------------ what it costs, and why
;;;
;;; The shape is shift-and-rotate, fully unrolled: ASL a pixel byte so its top
;;; bit falls into carry, ROL that carry into the plane byte being accumulated,
;;; eight times for the eight pixels of a row, eight times over for the eight
;;; planes. Seven cycles a bit and nothing else touched, because the pixel bytes
;;; live in DIRECT PAGE for the duration -- `asl dp` is 5 cycles where `asl abs`
;;; is 6, and the app's direct page starts at $1900 so DL is zero and there is no
;;; extra cycle for that either.
;;;
;;; Two other shapes were costed on paper before this one was written, and both
;;; lost. They are recorded because "why not a table?" is the obvious question:
;;;
;;;   A 512-BYTE TABLE PER PLANE PAIR, giving two planes at once in a 16-bit
;;;   accumulator: T01[v] = bit0(v) | (bit1(v) << 8), and out = (out << 1) | T01[v]
;;;   per pixel -- which works, because the low byte's bit 7 is only reached on
;;;   the last shift and so never crosses into bit 8. That is `asl a` (2) + `ldx`
;;;   (4) + `ora abs,x` (6) = 12 cycles for two bits, against 14 here. But the
;;;   index has to be 2v, not v, and doubling a pixel byte into a 16-bit direct
;;;   page slot costs more per pixel than the 2 cycles a plane pair saves. Net
;;;   loss, plus 2 KB of tables that would have to be built in RAM at startup
;;;   (see "initialised statics" in docs/calypsi-asm-syntax.md).
;;;
;;;   THE HACKER'S DELIGHT BUTTERFLY -- three delta-swap passes over the 8x8 bit
;;;   matrix. Each pass is ~47 cycles per 16-bit word and there are four words,
;;;   so ~190 a pass and ~570 a row before the repacking between passes, against
;;;   the 488 a row this does. A barrel shifter is what makes that algorithm win,
;;;   and the 65816 has not got one.
;;;
;;; MEASURED, not estimated: 40.2k master clocks a tile against 333.4k, which is
;;; 8.3x, or 0.112 of a frame against 0.93. Both ends of that come from the same
;;; place -- an exec callback on build_tile's entry and its RTL under
;;; `make test-vnc`, timing in master clocks, so the C wrapper's addressing is
;;; inside both figures and so is any NMI that lands mid-call. NMI is not this
;;; routine's to give back: it is most of a frame per frame while an application
;;; owns the PPU, and it scales with elapsed time, so it does not flatter the
;;; ratio.
;;;
;;; A 768-tile screen is therefore 1.4 seconds of transpose rather than 12, and a
;;; 96-tile pan a quarter of a second rather than two.
;;;
;;; WHAT IS NOW THE LARGER HALF OF PAINTING IS THE WRITE WINDOW. Measured in the
;;; same run: 141 frames of win_open-plus-DMA against 100 of transpose. A batch
;;; of eight tiles is built in 0.9 of a frame and then waits over a frame for the
;;; beam to come back round to scanline 192, so the wait no longer hides behind
;;; the work. Raising BATCH is the lever there, not more assembly -- and it is a
;;; different bug's worth of care, because BATCH is also what bounds how much a
;;; window may be asked to swallow.
;;;
;;; What is left HERE is almost all the 3584 cycles of ASL/ROL, which is the
;;; floor for this shape. Getting materially past it needs a different algorithm,
;;; and the two candidates are above.
;;;
;;; ------------------------------------------------- the C calling convention
;;;
;;; Same as kernel/netasm.s, and derived the same way -- out of a compiler
;;; listing, not vendor documentation:
;;;
;;;   first far pointer    _Dp   (offset), _Dp+2 (bank)
;;;   second far pointer   _Dp+4 (offset), _Dp+6 (bank)
;;;
;;; Entered with A/X/Y 16 bits wide, Calypsi's normal state, and hands them back
;;; that way because the caller does its own `sep` afterwards.
;;;
;;; This is NOT interrupt code, so the hazard in docs/calypsi-interrupt-hazard.md
;;; applies only in its mild form: `_Dp` is one shared 20-byte block addressed
;;; relative to D, and this routine merely reads the argument slots the caller
;;; has just written and calls nothing, so nothing of the caller's can be
;;; clobbered. NMI cannot collide either -- it runs on its own direct page
;;; ($1C00) while this runs on the application's ($1900).
;;;
;;; --------------------------------------------------------- the bank, exactly
;;;
;;; Both pointers are dereferenced with absolute-long-indexed addressing, whose
;;; bank is an immediate, so this file hard-codes $7F the way netasm.s hard-codes
;;; $7E. That is a linker invariant, not a hope: apps/app.scm.in gives an
;;; application exactly two data regions, AppNear $7F0000-$7F00FF and AppFar
;;; $7F0100-$7FFFFF, so with --data-model=large every static an app has is in
;;; bank $7F by construction and running out of room is a link error. Only the
;;; OFFSET of each pointer is taken from _Dp; the bank halves are ignored.
;;;
;;; The scratch is 64 bytes of `ztiny`, i.e. of the application's direct page.
;;; $1900-$19FF is 256 bytes and the only other thing in it is the 20-byte _Dp
;;; block, so there is room several times over -- the link map is where to check
;;; that if it ever stops being true.

              .rtmodel version, "1"
              .rtmodel core, "*"

              .extern _Dp

              .section farcode, root
              .public tile_transpose

;;; The application arena's bank, as the base of a 24-bit address. See above.
ARENA         .equ    0x7f0000

;;; The shadow is FB_W = 256 bytes wide, which is the one number that makes the
;;; addressing cheap: consecutive rows of a tile are exactly 256 bytes apart, so
;;; every one of the 64 source bytes is a CONSTANT displacement from the tile's
;;; first byte. Nothing is added per row and no index is ever advanced.
STRIDE        .equ    256

;;; ---------------------------------------------------------------------------
;;; One bit. ASL drops the pixel byte's top bit into carry and ROL takes it into
;;; the plane byte, so after eight of these A holds one plane with pixel 0 in bit
;;; 7 -- which is the order the PPU wants -- and each pixel byte has moved on to
;;; the next plane down. Eight passes consume the row exactly.
;;;
;;; A must be 8 bits here. `asl dp` is 5 cycles with M=1 and 7 with M=0, and the
;;; assembler tracks neither (docs/calypsi-asm-syntax.md), so the width is set
;;; once by the `sep` in the body and never disturbed.
;;; ---------------------------------------------------------------------------
rbit          .macro  px
              asl     dp:.tiny (\px)
              rol     a
              .endm

;;; One plane byte, from the eight pixels at direct-page offset \base.
plane         .macro  base
              lda     #0
              rbit    \base+0
              rbit    \base+1
              rbit    \base+2
              rbit    \base+3
              rbit    \base+4
              rbit    \base+5
              rbit    \base+6
              rbit    \base+7
              .endm

;;; One row of the tile: eight planes, MOST SIGNIFICANT FIRST because that is
;;; the order shifting produces them in, each stored as it appears.
;;;
;;; The 64-byte tile is four groups of eight words, group g holding planes 2g and
;;; 2g+1, so row r's word for planes (2g, 2g+1) is at byte 16g + 2r with the ODD
;;; plane in the high byte. Hence the constants: plane 7 at 49+2r, plane 6 at
;;; 48+2r, and so on down. X holds the destination offset throughout.
xrow          .macro  r
              plane   row+(\r)*8
              sta     long:ARENA+49+(\r)*2,x
              plane   row+(\r)*8
              sta     long:ARENA+48+(\r)*2,x
              plane   row+(\r)*8
              sta     long:ARENA+33+(\r)*2,x
              plane   row+(\r)*8
              sta     long:ARENA+32+(\r)*2,x
              plane   row+(\r)*8
              sta     long:ARENA+17+(\r)*2,x
              plane   row+(\r)*8
              sta     long:ARENA+16+(\r)*2,x
              plane   row+(\r)*8
              sta     long:ARENA+1+(\r)*2,x
              plane   row+(\r)*8
              sta     long:ARENA+0+(\r)*2,x
              .endm

;;; Eight source bytes into direct page, as four 16-bit moves. A is 16 bits for
;;; the whole load phase; X is the tile's first byte and does not move.
;;;
;;; The row is READ ONCE. The C this replaces read every pixel byte eight times
;;; -- once per plane -- out of far memory at -O0; copying it into direct page
;;; first is most of why the shift loop can afford to be dumb.
loadrow       .macro  r
              lda     long:ARENA+(\r)*STRIDE+0,x
              sta     dp:.tiny (row+(\r)*8+0)
              lda     long:ARENA+(\r)*STRIDE+2,x
              sta     dp:.tiny (row+(\r)*8+2)
              lda     long:ARENA+(\r)*STRIDE+4,x
              sta     dp:.tiny (row+(\r)*8+4)
              lda     long:ARENA+(\r)*STRIDE+6,x
              sta     dp:.tiny (row+(\r)*8+6)
              .endm

;;; ---------------------------------------------------------------------------
;;; void tile_transpose(const uint8_t *src, uint16_t *out)
;;;
;;; src points at the tile's top-left pixel in the linear shadow; out at the 32
;;; words of one staging slot. Both are in bank $7F. Nothing about the torus is
;;; visible here: a tile never straddles the shadow's seam, because the seam
;;; falls on a tile boundary in both axes (256 and 192 are multiples of 8) and
;;; `t` is a shadow tile index rather than a screen position. Panning is the
;;; tilemap's job.
;;; ---------------------------------------------------------------------------
tile_transpose:
              rep     #0x30            ; 16-bit A for the load, 16-bit X for the
                                       ; whole routine
              ldx     dp:.tiny _Dp     ; X = src offset within bank $7F
              loadrow 0
              loadrow 1
              loadrow 2
              loadrow 3
              loadrow 4
              loadrow 5
              loadrow 6
              loadrow 7

              ldx     dp:.tiny (_Dp+4) ; X = out offset within bank $7F
              sep     #0x20            ; 8-bit A: plane bytes, and `asl dp` at 5
                                       ; cycles instead of 7
              xrow    0
              xrow    1
              xrow    2
              xrow    3
              xrow    4
              xrow    5
              xrow    6
              xrow    7

              rep     #0x20            ; hand A back 16 bits wide, as the caller
                                       ; assumes
              rtl

;;; The row scratch: the 64 pixel bytes of one tile, destroyed in place by the
;;; shifting above. In the application's direct page, which is what makes the
;;; inner loop 7 cycles a bit.
              .section ztiny, bss
row:          .space  64
