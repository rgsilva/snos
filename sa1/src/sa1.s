;;; SA-1 side: reset, interrupt handlers, and a command loop driven through
;;; I-RAM (IM_CMD, see layout.inc). The S-CPU points CRV/CNV/CIV here before
;;; releasing the SA-1, so these entry points are ordinary bank-$00 code.
;;;
;;; The SA-1 has no WRAM: its direct page ($0000) and stack ($01FF) are in
;;; I-RAM, and so are the V_* scratch words shared code (rom_mark) uses.
;;; I-RAM and BW-RAM writes are disabled at reset (CIWP = 0, CBWE = 0), so
;;; enabling them comes before the first push.

#include "regs.inc"
#include "layout.inc"
#include "assets.inc"

              .section code, root
              .public sa1_reset, sa1_nmi, sa1_irq
              .extern bench_a, bench_b, bench_c_sa1, bench_d, rom_mark
              .extern arith_tbl, rom_probes_sa1, img8

sa1_reset:
              sei
              sep     #0x20
              lda     #0xff
              sta     abs:CIWP
              lda     #0x80
              sta     abs:CBWE
              lda     #SA1_BMAP
              sta     abs:BMAP
              clc
              xce
              rep     #0x38
              lda     ##SA1_STACK
              tcs
              lda     ##0
              tcd
              phk
              plb
              lda     ##SIG_HI
              sta     abs:IM_SIG+2
              lda     ##SIG_LO                ; the S-CPU waits for this word
              sta     abs:IM_SIG
              sep     #0x20
              lda     #0x90                   ; IRQ and NMI from the S-CPU
              sta     abs:CIE
              rep     #0x20
              cli
sa1_loop:     lda     abs:IM_CMD
              beq     sa1_loop
              cmp     ##CMD_LAST+1
              bcs     sa1_done
              asl     a
              tax
              jsr     (abs:sa1_cmd_tbl,x)
sa1_done:     lda     ##0
              sta     abs:IM_CMD
              bra     sa1_loop

;;; IRQ from the S-CPU (CCNT bit 7): count it, keep CFR's message, clear it.
sa1_irq:
              rep     #0x30
              pha
              sep     #0x20
              lda     abs:CFR
              bpl     sai_out$
              and     #0x0f
              sta     abs:IM_IRQM
              stz     abs:IM_IRQM+1
              lda     #0x80
              sta     abs:CIC
              rep     #0x20
              inc     abs:IM_IRQN
sai_out$:     rep     #0x30
              pla
              rti

;;; NMI from the S-CPU (CCNT bit 4).
sa1_nmi:
              rep     #0x30
              pha
              sep     #0x20
              lda     abs:CFR
              and     #0x0f
              sta     abs:IM_NMIM
              stz     abs:IM_NMIM+1
              lda     #0x10
              sta     abs:CIC
              rep     #0x20
              inc     abs:IM_NMIN
              pla
              rti

;;; ---------------------------------------------------------- commands
;;; Each: A/X/Y 16-bit, D = 0, returns with rts.

;;; ARG0 bench, ARG1 D, ARG2 src, ARG3 dst. IM_RES: count (4), M1, product (4).
cmd_bench:
              ldx     abs:IM_ARG2
              ldy     abs:IM_ARG3
              lda     abs:IM_ARG1
              tcd
              stz     dp:B_CNT
              stz     dp:B_CNT+2
              stx     dp:B_SRC
              sty     dp:B_DST
              lda     abs:IM_ARG0
              asl     a
              tax
              jsr     (abs:sa1_bench_tbl,x)
              lda     dp:B_CNT
              sta     abs:IM_RES
              lda     dp:B_CNT+2
              sta     abs:IM_RES+2
              lda     dp:B_M1
              sta     abs:IM_RES+4
              lda     dp:B_PROD
              sta     abs:IM_RES+6
              lda     dp:B_PROD+2
              sta     abs:IM_RES+8
              lda     ##0
              tcd
              rts

;;; Run arith_tbl (16-byte records, tools/mkassets.py): optional MCNT and MA
;;; writes, then MB written `rep` times. IM_RES + 6n: MR0-MR4, OF.
cmd_arith:
              ldx     ##0
              ldy     ##0
car_rec$:     cpx     ##ARITH_N * 16
              bne     car_go$
              rts
car_go$:      sep     #0x20
              lda     abs:arith_tbl,x
              and     #1
              beq     car_noc$
              lda     abs:arith_tbl+1,x
              sta     abs:MCNT
car_noc$:     lda     abs:arith_tbl,x
              and     #2
              rep     #0x20
              beq     car_noa$
              lda     abs:arith_tbl+2,x
              sta     abs:MA
car_noa$:     lda     abs:arith_tbl+6,x
              sta     dp:V_T0
car_rep$:     lda     abs:arith_tbl+4,x
              sta     abs:MB                  ; high byte starts the operation
              nop                             ; >= 6 cycles before the next
              nop
              nop
              dec     dp:V_T0
              bne     car_rep$
              sep     #0x20
              lda     abs:MR
              sta     abs:IM_RES,y
              lda     abs:MR+1
              sta     abs:IM_RES+1,y
              lda     abs:MR+2
              sta     abs:IM_RES+2,y
              lda     abs:MR+3
              sta     abs:IM_RES+3,y
              lda     abs:MR+4
              sta     abs:IM_RES+4,y
              lda     abs:OF
              sta     abs:IM_RES+5,y
              rep     #0x20
              tya
              clc
              adc     ##6
              tay
              txa
              clc
              adc     ##16
              tax
              bra     car_rec$

;;; ARG1 nonzero -> SIV; then ARG0 -> SCNT.
cmd_signal:
              lda     abs:IM_ARG1
              beq     csg_scnt$
              sta     abs:SIV
csg_scnt$:    sep     #0x20
              lda     abs:IM_ARG0
              sta     abs:SCNT
              rep     #0x20
              rts

;;; ARG0 -> BMAP; the word at $6010 -> IM_RES; BMAP back to its idle block.
cmd_bwmap:
              sep     #0x20
              lda     abs:IM_ARG0
              sta     abs:BMAP
              rep     #0x20
              lda     abs:0x6010
              sta     abs:IM_RES
              sep     #0x20
              lda     #SA1_BMAP
              sta     abs:BMAP
              rep     #0x20
              rts

cmd_cbwe:
              sep     #0x20
              lda     abs:IM_ARG0
              sta     abs:CBWE
              rep     #0x20
              rts

;;; ARG1 nonzero -> CDMA first (fullsnes ends type 1 from this side), then
;;; ARG0 -> DCNT.
cmd_dcnt:
              sep     #0x20
              lda     abs:IM_ARG1
              beq     cdc_dcnt$
              sta     abs:CDMA
cdc_dcnt$:    lda     abs:IM_ARG0
              sta     abs:DCNT
              rep     #0x20
              rts

;;; The markers at rom_probes_sa1, through the SA-1's bus. IM_RES + 4n.
cmd_romread:
              ldx     ##0
              stz     dp:V_T1
crr$:         lda     abs:rom_probes_sa1,x
              sta     dp:V_PTR
              lda     abs:rom_probes_sa1+2,x
              and     ##0xff
              sta     dp:V_PTR+2
              phx
              jsr     rom_mark
              ldx     dp:V_T1
              lda     dp:V_NUM
              sta     abs:IM_RES,x
              lda     dp:V_NUM+2
              sta     abs:IM_RES+2,x
              inx
              inx
              inx
              inx
              stx     dp:V_T1
              plx
              txa
              clc
              adc     ##8
              tax
              cpx     ##ROM_PROBES_SA1 * 8
              bne     crr$
              rts

;;; Type 2 conversion: the first 8 x 8 tile of img8 written pixel by pixel
;;; into BRF, rows alternating between its two halves; each completed row
;;; lands as planar bytes in the I-RAM buffer at DDA (CC_BUF).
cmd_cc2:
              sep     #0x20
              stz     abs:CDMA                ; 8bpp
              lda     #0xa0                   ; enable, char conv, type 2
              sta     abs:DCNT
              stz     abs:DDA
              lda     #CC_BUF >> 8
              sta     abs:DDA+1
              rep     #0x20
              stz     dp:V_T2                 ; row
cc2_row$:     lda     dp:V_T2
              xba
              lsr     a
              lsr     a
              tax                             ; row * 64: source
              lda     dp:V_T2
              and     ##1
              asl     a
              asl     a
              asl     a
              tay                             ; BRF half
              lda     ##8
              sta     dp:V_T3
              sep     #0x20
cc2_px$:      lda     abs:img8,x
              sta     abs:BRF,y
              inx
              iny
              dec     dp:V_T3
              bne     cc2_px$
              rep     #0x20
              inc     dp:V_T2
              lda     dp:V_T2
              cmp     ##8
              bne     cc2_row$
              sep     #0x20
              stz     abs:DCNT
              rep     #0x20
              rts

sa1_cmd_tbl:  .word   0, cmd_bench, cmd_arith, cmd_signal, cmd_bwmap, cmd_cbwe
              .word   cmd_dcnt, cmd_romread, cmd_cc2
sa1_bench_tbl:
              .word   bench_a, bench_b, bench_c_sa1, bench_d
