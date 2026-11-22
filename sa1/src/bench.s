;;; Check 6: the same routines timed on both CPUs.
;;;
;;; bench_a/b/d are byte-for-byte the code both CPUs run, from this ROM bank.
;;; They keep their state in a direct page (B_*) and stop when abs IM_STOP
;;; turns nonzero; with DB=$00 that address is I-RAM on the SA-1 and WRAM on
;;; the S-CPU, so each CPU's stop flag is local to it. Moving D moves the
;;; data: $0300 (I-RAM on the SA-1, WRAM on the S-CPU) or $6000 (BW-RAM on
;;; both). bench_c differs by CPU only because the multipliers do.
;;;
;;; Entry for every bench_*: A/X/Y 16-bit, D = the benchmark page, DB = $00,
;;; B_CNT zeroed, B_SRC/B_DST set. Exit with B_CNT = iterations completed.

#include "regs.inc"
#include "layout.inc"

              .section code, root
              .public bench_a, bench_b, bench_c_sa1, bench_d, check_speed
              .extern ln_begin, ln_char, ln_str, ln_col, ln_hex4, ln_dec, ln_ratio
              .extern ln_status, ln_end, ln_hex2, wait_vblank, wait_eq, wait_ne
              .extern STATUS_OK, STATUS_FAIL, STATUS_INFO

;;; (a) 16 rounds of add / compare / branch in registers.
bench_a:
ba_iter$:     lda     ##0
              ldx     ##16
ba_in$:       clc
              adc     ##0x1234
              cmp     ##0xf000
              bcc     ba_sk$
              sbc     ##0xe000
ba_sk$:       dex
              bne     ba_in$
              inc     dp:B_CNT
              bne     ba_nc$
              inc     dp:B_CNT+2
ba_nc$:       lda     abs:IM_STOP
              beq     ba_iter$
              rts

;;; (b) 16 x 16 -> 32 shift-and-add multiply, operands and product in DP.
;;; M1 = iteration & $7FFF, M2 = $1234.
bench_b:
bb_iter$:     lda     dp:B_CNT
              and     ##0x7fff
              sta     dp:B_M1
              sta     dp:B_T1
              stz     dp:B_T1+2
              lda     ##0x1234
              sta     dp:B_M2
              stz     dp:B_PROD
              stz     dp:B_PROD+2
              ldx     ##16
bb_bit$:      lsr     dp:B_M2
              bcc     bb_sk$
              clc
              lda     dp:B_PROD
              adc     dp:B_T1
              sta     dp:B_PROD
              lda     dp:B_PROD+2
              adc     dp:B_T1+2
              sta     dp:B_PROD+2
bb_sk$:       asl     dp:B_T1
              rol     dp:B_T1+2
              dex
              bne     bb_bit$
              inc     dp:B_CNT
              bne     bb_nc$
              inc     dp:B_CNT+2
bb_nc$:       lda     abs:IM_STOP
              beq     bb_iter$
              rts

;;; (c) the same product on the SA-1's multiplier: one signed 16 x 16
;;; (operands below $8000, so signed = unsigned). 5 cycles until MR is valid.
bench_c_sa1:
              sep     #0x20
              stz     abs:MCNT
              rep     #0x20
bcs_iter$:    lda     dp:B_CNT
              and     ##0x7fff
              sta     dp:B_M1
              sta     abs:MA
              lda     ##0x1234
              sta     abs:MB                  ; the high byte starts it
              nop
              nop
              lda     abs:MR
              sta     dp:B_PROD
              lda     abs:MR+2
              sta     dp:B_PROD+2
              inc     dp:B_CNT
              bne     bcs_nc$
              inc     dp:B_CNT+2
bcs_nc$:      lda     abs:IM_STOP
              beq     bcs_iter$
              rts

;;; (c) on the S-CPU: four 8 x 8 products from $4202/$4203, 8 cycles each.
;;; NMI and auto-joypad are off while this runs (both disturb $4216).
bench_c_scpu:
bcc_iter$:    lda     dp:B_CNT
              and     ##0x7fff
              sta     dp:B_M1
              sep     #0x20
              sta     abs:WRMPYA              ; xl * $34
              lda     #0x34
              sta     abs:WRMPYB
              nop
              nop
              nop
              nop
              rep     #0x20
              lda     abs:RDMPYL
              sta     dp:B_PROD
              stz     dp:B_PROD+2
              sep     #0x20
              lda     #0x12                   ; xl * $12, << 8
              sta     abs:WRMPYB
              nop
              nop
              nop
              nop
              rep     #0x20
              lda     abs:RDMPYL
              clc
              adc     dp:B_PROD+1
              sta     dp:B_PROD+1
              sep     #0x20
              lda     #0
              adc     dp:B_PROD+3
              sta     dp:B_PROD+3
              lda     dp:B_M1+1               ; xh * $34, << 8
              sta     abs:WRMPYA
              lda     #0x34
              sta     abs:WRMPYB
              nop
              nop
              nop
              nop
              rep     #0x20
              lda     abs:RDMPYL
              clc
              adc     dp:B_PROD+1
              sta     dp:B_PROD+1
              sep     #0x20
              lda     #0
              adc     dp:B_PROD+3
              sta     dp:B_PROD+3
              lda     #0x12                   ; xh * $12, << 16
              sta     abs:WRMPYB
              nop
              nop
              nop
              nop
              rep     #0x20
              lda     abs:RDMPYL
              clc
              adc     dp:B_PROD+2
              sta     dp:B_PROD+2
              inc     dp:B_CNT
              bne     bcc_nc$
              inc     dp:B_CNT+2
bcc_nc$:      lda     abs:IM_STOP
              beq     bcc_iter$
              rts

;;; (d) copy 256 bytes B_SRC -> B_DST, a word at a time.
bench_d:
bd_iter$:     ldy     ##0
bd_cp$:       lda     (dp:B_SRC),y
              sta     (dp:B_DST),y
              iny
              iny
              cpy     ##256
              bne     bd_cp$
              inc     dp:B_CNT
              bne     bd_nc$
              inc     dp:B_CNT+2
bd_nc$:       lda     abs:IM_STOP
              beq     bd_iter$
              rts

;;; X = frames. Polls HVBJOY for X vertical blanks, then sets the SA-1's
;;; stop flag. Position independent: it also runs from WAIT_WRAM. The inner
;;; delay keeps the CPU fetching code between polls (ROM when run in place).
wait_stop:
              sep     #0x20
ws_out$:      lda     abs:HVBJOY
              bmi     ws_out$
ws_in$:       ldy     ##24
ws_dl$:       dey
              bne     ws_dl$
              lda     abs:HVBJOY
              bpl     ws_in$
              dex
              bne     ws_out$
              lda     #1
              sta     abs:IRAM+IM_STOP
              rep     #0x20
              rts
wait_stop_end:

;;; ---------------------------------------------------------------- driver
;;; Configs, in screen order: S = S-CPU, data in WRAM; then the SA-1 with
;;; data in I-RAM (I) or BW-RAM (B) while the S-CPU idles in WRAM (i) or
;;; polls from ROM (b).
NCFG          .equ    5

;;; Per bench: 5 lines. Results in RESULTS[(bench * 5 + cfg) * 4]; for the
;;; multiplies, a "chk" line (transcript only) with the last M1 and product.
check_speed:
              ldx     ##0                     ; wait_stop -> WRAM
csp_cp$:      lda     abs:wait_stop,x
              sta     abs:WAIT_WRAM,x
              inx
              inx
              cpx     ##wait_stop_end-wait_stop+1
              bcc     csp_cp$
              stz     dp:V_T6                 ; bench
csp_bench$:   stz     dp:V_T7                 ; config
csp_cfg$:     lda     dp:V_T7
              bne     csp_sa1$
              jsr     run_s
              bra     csp_line$
csp_sa1$:     jsr     run_a
csp_line$:    jsr     speed_line
              inc     dp:V_T7
              lda     dp:V_T7
              cmp     ##NCFG
              bne     csp_cfg$
              inc     dp:V_T6
              lda     dp:V_T6
              cmp     ##4
              bne     csp_bench$
              jmp     fast_line

;;; Index of the current result slot * 4 -> X.
slot:
              lda     dp:V_T6
              asl     a
              asl     a
              clc
              adc     dp:V_T6                 ; bench * 5
              clc
              adc     dp:V_T7
              asl     a
              asl     a
              tax
              rts

;;; The S-CPU run: D = $0300 in WRAM, timed by the V-timer IRQ at line 225.
;;; Leaves V_NUM = count (0 if the IRQ never came), V_T4/V_T5 = M1/product lo,
;;; V_PTR = product hi.
run_s:
              stz     abs:B_IDP+B_CNT
              stz     abs:B_IDP+B_CNT+2
              lda     ##B_ISRC
              sta     abs:B_IDP+B_SRC
              lda     ##B_IDST
              sta     abs:B_IDP+B_DST
              stz     abs:IM_STOP
              stz     dp:V_LEFT
              lda     ##225
              sta     abs:VTIMEL
              sep     #0x20
              lda     #0x20                   ; V-IRQ only: no NMI, no joypad
              sta     abs:NMITIMEN
              rep     #0x20
              lda     dp:V_TICKS              ; start on a tick
              sta     dp:V_WVAL
              lda     ##V_TICKS
              sta     dp:V_WPTR
              lda     ##5
              sta     dp:V_WMAX
              jsr     wait_ne
              bcs     rs_dead$
              lda     ##BENCH_FRAMES
              sta     dp:V_LEFT
              lda     dp:V_T6
              asl     a
              tax
              phd
              lda     ##B_IDP
              tcd
              lda     abs:V_FAST
              bne     rs_fast$
              jsr     (abs:s_bench_tbl,x)
              bra     rs_ran$
rs_fast$:     jsl     long:fast_a + 0x800000
rs_ran$:      pld
              lda     abs:B_IDP+B_CNT
              sta     dp:V_NUM
              lda     abs:B_IDP+B_CNT+2
              sta     dp:V_NUM+2
              lda     abs:B_IDP+B_M1
              sta     dp:V_T4
              lda     abs:B_IDP+B_PROD
              sta     dp:V_T5
              lda     abs:B_IDP+B_PROD+2
              sta     dp:V_PTR
              bra     rs_out$
rs_dead$:     stz     dp:V_NUM
              stz     dp:V_NUM+2
rs_out$:      sep     #0x20
              stz     abs:NMITIMEN
              rep     #0x20
              rts

;;; Bank $80 entry for the FastROM run: with EXB = $80, $80:8000-$FFFF is
;;; this ROM bank, so the same bench_a runs from there at 3.58 MHz.
fast_a:       jsr     bench_a
              rtl

;;; "a:F     OK   3263     x1.2": bench_a on the S-CPU in FastROM. SNOS runs
;;; FastROM today; fullsnes doubts it is usable on an SA-1 cart.
fast_line:
              sep     #0x20
              lda     #0x80
              sta     abs:EXB
              lda     #1
              sta     abs:MEMSEL
              rep     #0x20
              lda     ##1
              sta     dp:V_FAST
              stz     dp:V_T6                 ; bench a
              jsr     run_s
              stz     dp:V_FAST
              sep     #0x20
              stz     abs:MEMSEL
              lda     #0x02
              sta     abs:EXB
              rep     #0x20
              jsr     ln_begin
              ldx     ##s_fast
              jsr     ln_str
              ldx     ##STATUS_OK
              lda     dp:V_NUM
              ora     dp:V_NUM+2
              bne     fl_st$
              ldx     ##STATUS_FAIL
fl_st$:       txa
              jsr     ln_status
              lda     dp:V_NUM
              sta     dp:V_T4
              lda     dp:V_NUM+2
              sta     dp:V_T5
              jsr     ln_dec
              lda     ##22
              jsr     ln_col
              lda     dp:V_T4
              sta     dp:V_NUM
              lda     dp:V_T5
              sta     dp:V_NUM+2
              lda     abs:RESULTS
              sta     dp:V_DEN
              lda     abs:RESULTS+2
              sta     dp:V_DEN+2
              jsr     ln_ratio
              jmp     ln_end

;;; An SA-1 run with config V_T7 (1-4). Same results as run_s.
run_a:
              stz     dp:V_NUM
              stz     dp:V_NUM+2
              lda     dp:V_SA1OK
              bne     ra_go$
              rts
ra_go$:       lda     dp:V_T6
              sta     abs:IRAM+IM_ARG0
              lda     dp:V_T7
              dec     a
              asl     a
              asl     a
              asl     a                       ; (cfg - 1) * 8
              tax
              lda     abs:cfg_tbl,x
              sta     abs:IRAM+IM_ARG1
              lda     abs:cfg_tbl+2,x
              sta     abs:IRAM+IM_ARG2
              lda     abs:cfg_tbl+4,x
              sta     abs:IRAM+IM_ARG3
              lda     abs:cfg_tbl+6,x
              sta     dp:V_T5                 ; 0 idle in WRAM, 1 busy in ROM
              stz     abs:IRAM+IM_STOP
              jsr     wait_vblank
              lda     ##CMD_BENCH
              sta     abs:IRAM+IM_CMD
              ldx     ##BENCH_FRAMES
              lda     dp:V_T5
              bne     ra_busy$
              jsr     abs:WAIT_WRAM
              bra     ra_wait$
ra_busy$:     jsr     wait_stop
ra_wait$:     lda     ##IRAM+IM_CMD
              sta     dp:V_WPTR
              stz     dp:V_WVAL
              lda     ##10
              sta     dp:V_WMAX
              jsr     wait_eq
              bcs     ra_dead$
              lda     abs:IRAM+IM_RES
              sta     dp:V_NUM
              lda     abs:IRAM+IM_RES+2
              sta     dp:V_NUM+2
              lda     abs:IRAM+IM_RES+4
              sta     dp:V_T4
              lda     abs:IRAM+IM_RES+6
              sta     dp:V_T5
              lda     abs:IRAM+IM_RES+8
              sta     dp:V_PTR
ra_dead$:     rts

;;; "a:Ii    OK   10412 x4.0", then the transcript-only check line.
speed_line:
              jsr     slot
              lda     dp:V_NUM
              sta     abs:RESULTS,x
              lda     dp:V_NUM+2
              sta     abs:RESULTS+2,x
              lda     dp:V_T4                 ; keep the check values
              pha
              lda     dp:V_T5
              pha
              lda     dp:V_PTR
              pha
              jsr     ln_begin
              lda     dp:V_T6
              clc
              adc     ##0x61                  ; 'a'
              jsr     ln_char
              lda     ##0x3a
              jsr     ln_char
              lda     dp:V_T7
              asl     a
              tax
              lda     abs:cfg_names,x
              tax
              jsr     ln_str
              ldx     ##STATUS_OK
              lda     dp:V_NUM
              ora     dp:V_NUM+2
              bne     sl_st$
              ldx     ##STATUS_FAIL
sl_st$:       txa
              jsr     ln_status
              jsr     ln_dec
              lda     dp:V_T7
              beq     sl_end$
              lda     ##22
              jsr     ln_col
              jsr     slot
              lda     abs:RESULTS,x
              sta     dp:V_NUM
              lda     abs:RESULTS+2,x
              sta     dp:V_NUM+2
              txa                             ; the S-CPU's slot: bench * 20
              sec
              sbc     dp:V_T7
              sbc     dp:V_T7
              sbc     dp:V_T7
              sbc     dp:V_T7
              tax
              lda     abs:RESULTS,x
              sta     dp:V_DEN
              lda     abs:RESULTS+2,x
              sta     dp:V_DEN+2
              jsr     ln_ratio
sl_end$:      jsr     ln_end
              pla
              sta     dp:V_PTR
              pla
              sta     dp:V_T5
              pla
              sta     dp:V_T4
              lda     dp:V_T6                 ; b and c: check line
              cmp     ##1
              beq     sl_chk$
              cmp     ##2
              bne     sl_out$
sl_chk$:      lda     ##2                     ; transcript only
              sta     dp:V_LNMODE
              jsr     ln_begin
              ldx     ##s_chk
              jsr     ln_str
              lda     dp:V_T6
              clc
              adc     ##0x61
              jsr     ln_char
              lda     dp:V_T7
              asl     a
              tax
              lda     abs:cfg_names,x
              tax
              jsr     ln_str
              lda     ##0x20
              jsr     ln_char
              lda     dp:V_T4
              jsr     ln_hex4
              lda     ##0x20
              jsr     ln_char
              lda     dp:V_PTR
              jsr     ln_hex4
              lda     dp:V_T5
              jsr     ln_hex4
              jsr     ln_end
              stz     dp:V_LNMODE
sl_out$:      rts

s_bench_tbl:  .word   bench_a, bench_b, bench_c_scpu, bench_d
;;; D, src, dst, S-CPU busy
cfg_tbl:      .word   B_IDP, B_ISRC, B_IDST, 0
              .word   B_BDP, B_BSRC, B_BDST, 0
              .word   B_IDP, B_ISRC, B_IDST, 1
              .word   B_BDP, B_BSRC, B_BDST, 1
cfg_names:    .word   s_cs, s_cii, s_cbi, s_cib, s_cbb
s_cs:         .asciz  "S"
s_cii:        .asciz  "Ii"
s_cbi:        .asciz  "Bi"
s_cib:        .asciz  "Ib"
s_cbb:        .asciz  "Bb"
s_chk:        .asciz  "chk "
s_fast:       .asciz  "a:F"
