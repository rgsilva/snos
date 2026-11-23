;;; The assembly half of the boot beacon (kernel/beacon.h, docs/sa1-notes.md),
;;; included by header-sa1.s under BEACON.

#define BEACON_ASM
#include "../kernel/beacon.h"

;;; Backdrop to `colour`, display on, then BEACON_HOLD frames. 8-bit A and X;
;;; touches no RAM and no stack.
beacon_paint  .macro  colour
              lda     #0x80
              sta     long:0x002100         ; forced blank for the CGRAM write
              lda     #0
              sta     long:0x002121
              lda     #(\colour & 0xff)
              sta     long:0x002122
              lda     #(\colour >> 8)
              sta     long:0x002122
              lda     #0x0f
              sta     long:0x002100         ; display on, full brightness
              ldx     #BEACON_HOLD
10$:          lda     long:0x004212
              bmi     10$                   ; out of any vblank
20$:          lda     long:0x004212
              bpl     20$                   ; into the next one
              dex
              bne     10$
              .endm

;;; The reset vector. The CPU is in emulation mode with DB=D=0 and nothing
;;; else is known: the PPU keeps whatever it had, so every register that can
;;; hide the backdrop is set here. Then the normal C startup.
              .section reset, root
              .public __program_root_section
__program_root_section:
              .word   beacon_reset

              .section code, root
              .public beacon_reset
              .extern __program_start
beacon_reset:
              sei
              lda     #0x80
              sta     long:0x002100
              lda     #0
              sta     long:0x004200         ; NMI, IRQ, auto-joypad off
              sta     long:0x00420c         ; HDMA off
              sta     long:0x002105         ; mode 0
              sta     long:0x002106         ; no mosaic
              sta     long:0x002123         ; windows off
              sta     long:0x002124
              sta     long:0x002125
              sta     long:0x00212a
              sta     long:0x00212b
              sta     long:0x00212c         ; main screen: no layers
              sta     long:0x00212d         ; sub screen: none
              sta     long:0x00212e         ; no window masking
              sta     long:0x00212f
              sta     long:0x002131         ; no colour math on any layer
              sta     long:0x002133         ; no interlace, hires, overscan
              lda     #0x30
              sta     long:0x002130         ; never force black, never math
              lda     #0xe0
              sta     long:0x002132         ; fixed colour black
              beacon_paint BEACON_RED
              jmp     long:__program_start

;;; Called by the C startup before it initialises RAM: the first code the
;;; S-CPU runs from a bank $C0 or above, still at SlowROM.
              .section farcode
              .public __low_level_init
__low_level_init:
              php
              sep     #0x30
              beacon_paint BEACON_GREEN
              plp
              rtl

;;; hw_init calls this after the FastROM switch: 4096 passes of a loop that
;;; also reads ROM, at $C0:FFE0 (linker-sa1-beacon.scm, made by the Makefile),
;;; a bank away from main; then rtl.
              .section beaconfar, text
              .public beacon_far
beacon_far:
              php
              rep     #0x30
              phx
              ldx     ##0
bf_loop:      lda     long:beacon_far
              inx
              cpx     ##4096
              bne     bf_loop
              plx
              plp
              rtl
