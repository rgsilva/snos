;;; SNOS cartridge header, SA-1 (map $23). Built by `make SA1=1`; see
;;; docs/sa1-port-plan.md and docs/sa1-notes.md.
;;;
;;; The SA-1 maps ROM $0000-$7FFF at $00:8000, so this header sits at ROM
;;; $7FB0-$7FDF and the vectors at $7FE0 (linker-sa1.scm). tools/raw2sfc.py
;;; --sa1 patches the ROM size byte ($7FD7) and the checksum. The vectors and
;;; trap handlers are traps.inc, the same as on the HiROM cartridge.

              .rtmodel version, "1"
              .rtmodel core, "*"

;;; ---------------------------------------------------------------- header

;;; Extended header $7FB0-$7FBF (licensee byte $33 => present)
              .section snesheaderextended, root
              .ascii  "  "           ; maker code
              .ascii  "SNOS"         ; game code
              .byte   0,0,0,0,0,0,0  ; reserved
              .byte   0              ; expansion RAM size
              .byte   0              ; special version
              .byte   0              ; cartridge sub-type

;;; Header $7FC0-$7FDF
              .section snesheader, root
              .ascii  "SNOS                 "   ; 21-byte title
              .byte   0x23           ; map mode: SA-1, SlowROM (kernel/main.c)
              .byte   0x35           ; cartridge type: SA-1+ROM+RAM+battery
              .byte   0x0c           ; ROM size (patched by raw2sfc.py)
              .byte   0x08           ; RAM size: 256 KB BW-RAM
              .byte   0x01           ; country: USA (NTSC)
              .byte   0x33           ; licensee: extended header present
              .byte   0x00           ; version
              .word   0xaaaa         ; checksum complement (patched)
              .word   0x5555         ; checksum (patched)

#include "traps.inc"

#ifdef BEACON
#include "beacon.inc"
#endif
