;;; SA-1 cartridge header and S-CPU vectors, all in LoROM bank $00.
;;;
;;; An SA-1 cart keeps its header at ROM offset $7FB0-$7FFF ($00:FFB0) even
;;; though banks $C0+ are HiROM-style: the chip maps the first 32 KB of ROM to
;;; $00:8000. tools/mksfc.py patches the ROM size byte and the checksum.
;;; The SA-1's own reset/NMI/IRQ vectors are NOT here: they come from
;;; CRV/CNV/CIV, which the S-CPU writes before releasing it (its BRK and COP
;;; would come from this table, and are never used).

              .rtmodel version, "1"
              .rtmodel core, "*"

              .extern s_reset, s_irq, s_rti

;;; Marker for ROM offset $007F80; mksfc.py writes every 32 KB block's.
              .section romtag, root
              .space  48, 0xff

              .section sneshdrext, root
              .ascii  "  "                  ; maker code
              .ascii  "SA1P"                ; game code
              .byte   0,0,0,0,0,0,0         ; reserved
              .byte   0                     ; expansion RAM: none
              .byte   0                     ; special version
              .byte   0                     ; cartridge sub-type

              .section sneshdr, root
              .ascii  "SA1PROBE             " ; 21 bytes
              .byte   0x23                  ; map mode: SA-1, SlowROM
              .byte   0x35                  ; cart type: SA-1 + ROM + RAM + battery
              .byte   0x0c                  ; ROM size: 4 MB (patched)
              .byte   0x08                  ; RAM size: 256 KB BW-RAM
              .byte   0x01                  ; country: USA (NTSC)
              .byte   0x33                  ; licensee: extended header
              .byte   0x00                  ; version
              .word   0xaaaa                ; checksum complement (patched)
              .word   0x5555                ; checksum (patched)

              .section vectors, root
              .word   0, 0                  ; $FFE0
              .word   s_rti                 ; $FFE4 native COP
              .word   s_rti                 ; $FFE6 native BRK
              .word   s_rti                 ; $FFE8 native ABORT
              .word   s_rti                 ; $FFEA native NMI (never enabled)
              .word   0                     ; $FFEC
              .word   s_irq                 ; $FFEE native IRQ
              .word   0, 0                  ; $FFF0
              .word   s_rti                 ; $FFF4 emulation COP
              .word   0                     ; $FFF6
              .word   s_rti                 ; $FFF8 emulation ABORT
              .word   s_rti                 ; $FFFA emulation NMI
              .word   s_reset               ; $FFFC reset
              .word   s_rti                 ; $FFFE emulation IRQ/BRK
