;;; SNOS cartridge header, HiROM. The vectors and trap handlers are in traps.inc;
;;; header-sa1.s is the SA-1 cartridge's header around the same file.
;;;
;;; HiROM ($21) + FastROM => map mode $31. MEMSEL ($420D bit 0) is set by
;;; kernel_init(); reset always begins in slow mode with PB=$00, so the first
;;; instructions run out of the $00:8000 mirror regardless.
;;;
;;; ROM size byte ($FFD7) is PATCHED BY tools/raw2sfc.py to match the actual
;;; padded image size — do not hand-maintain it here, a mismatch degrades
;;; Mesen's header-scoring and shows up as "cartridge not detected".
;;;
;;; Vector rules that bit people before (PLAN.md §3): COP and BRK force PB=$00,
;;; so their handlers must live in bank $00 — i.e. in the `code` section, which
;;; linker.scm scatters to $C08000 and the CPU sees mirrored at $00:8000.
;;; Handlers indirect through the RAM vector table so M3 apps can hook them
;;; without rewriting ROM.

              .rtmodel version, "1"
              .rtmodel core, "*"

;;; ---------------------------------------------------------------- header

;;; Extended header $FFB0-$FFBF (licensee byte $33 => present)
              .section snesheaderextended, root
              .ascii  "  "           ; maker code
              .ascii  "SNOS"         ; game code
              .byte   0,0,0,0,0,0,0  ; reserved
              .byte   0              ; expansion RAM size
              .byte   0              ; special version
              .byte   0              ; cartridge sub-type

;;; Header $FFC0-$FFDF
              .section snesheader, root
              .ascii  "SNOS                 "   ; 21-byte title
              .byte   0x31           ; map mode: HiROM + FastROM
              .byte   0x02           ; cartridge type: ROM+RAM+battery
              .byte   0x0c           ; ROM size (patched by raw2sfc.py)
              .byte   0x08           ; RAM size: 256 KB cart SRAM
              .byte   0x01           ; country: USA (NTSC)
              .byte   0x33           ; licensee: extended header present
              .byte   0x00           ; version
              .word   0xaaaa         ; checksum complement (patched)
              .word   0x5555         ; checksum (patched)

#include "traps.inc"
