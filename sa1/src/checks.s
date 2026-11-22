;;; Checks 1-5 and 7: one routine per screen line or group of lines. Each
;;; measures, judges against the documented behaviour, prints, and moves on;
;;; every wait is bounded, so a dead feature prints FAIL instead of hanging.
;;; S-CPU conventions as in main.s.

#include "regs.inc"
#include "layout.inc"
#include "assets.inc"

              .section code, root
              .public check_iram, check_start, check_vc, check_signal, check_arith
              .public check_bwram, check_rom, check_cc, rom_mark
              .extern ln_begin, ln_char, ln_str, ln_col, ln_hex2, ln_hex4, ln_hex6
              .extern ln_dec, ln_status, ln_end, ln_yn, udiv32, wait_vblank
              .extern wait_eq, wait_ne, sa1_cmd, dma_vram
              .extern sa1_reset, sa1_nmi, sa1_irq, s_irq_alt
              .extern arith_tbl, img8, img4, exp8, exp4
              .extern STATUS_OK, STATUS_FAIL, STATUS_INFO

;;; X = name string, A = STATUS_*: start a check line.
line_start:
              pha
              phx
              jsr     ln_begin
              plx
              jsr     ln_str
              pla
              jmp     ln_status

;;; ===================================================== 1. I-RAM, start
;;; SIWP resets to 0, which per the documentation write-protects all of
;;; I-RAM from the S-CPU; the first write below is made before touching it.
check_iram:
              sep     #0x20
              lda     abs:IRAM+0x700
              eor     #0xff
              sta     dp:V_T2
              sta     abs:IRAM+0x700
              lda     abs:IRAM+0x700
              cmp     dp:V_T2
              rep     #0x20
              jsr     z_to_a                  ; 1: the reset-state write landed
              sta     dp:V_T3
              sep     #0x20
              lda     #0xff
              sta     abs:SIWP
              rep     #0x20

              stz     dp:V_T4                 ; mismatches
              lda     ##0xace1                ; pass 1: LFSR bytes
              jsr     iram_pass
              lda     ##0x1d2b                ; pass 2: another sequence
              jsr     iram_pass

              sep     #0x20                   ; protect $3300-$33FF only
              lda     #0xf7
              sta     abs:SIWP
              lda     abs:IRAM+0x300
              eor     #0xff
              sta     dp:V_T2
              sta     abs:IRAM+0x300
              lda     abs:IRAM+0x300
              cmp     dp:V_T2
              rep     #0x20
              jsr     z_to_a
              sta     dp:V_T5
              sep     #0x20
              lda     #0xff
              sta     abs:SIWP
              rep     #0x20

              ldx     ##0x7fe                 ; leave I-RAM zeroed: mailbox idle
ci_clr$:      stz     abs:IRAM,x
              dex
              dex
              bpl     ci_clr$

              lda     ##STATUS_OK
              ldx     dp:V_T4
              beq     ci_ok$
              lda     ##STATUS_FAIL
ci_ok$:       ldx     ##nm_iram
              jsr     line_start
              lda     dp:V_T4
              bne     ci_err$
              ldx     ##s_iram_ok
              jsr     ln_str
              bra     ci_end$
ci_err$:      sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              ldx     ##s_bad
              jsr     ln_str
ci_end$:      jsr     ln_end

              lda     dp:V_T3                 ; both writes blocked = OK
              ora     dp:V_T5
              beq     ci_wpok$
              lda     ##STATUS_FAIL
              bra     ci_wp$
ci_wpok$:     lda     ##STATUS_OK
ci_wp$:       ldx     ##nm_iramwp
              jsr     line_start
              ldx     ##s_rst
              jsr     ln_str
              lda     dp:V_T3
              cmp     ##1
              jsr     ln_yn
              ldx     ##s_b3
              jsr     ln_str
              lda     dp:V_T5
              cmp     ##1
              jsr     ln_yn
              jmp     ln_end

;;; A = 1 if Z was set, else 0. Flags are not preserved.
z_to_a:
              beq     za1$
              lda     ##0
              rts
za1$:         lda     ##1
              rts

;;; A = LFSR seed. Fills all 2 KB, then compares; adds mismatches to V_T4.
iram_pass:
              sta     dp:V_T0
              sta     dp:V_T1
              ldx     ##0
ip_fill$:     lda     dp:V_T0
              lsr     a
              bcc     ip_f1$
              eor     ##0xb400
ip_f1$:       sta     dp:V_T0
              sep     #0x20
              sta     abs:IRAM,x
              rep     #0x20
              inx
              cpx     ##0x800
              bne     ip_fill$
              lda     dp:V_T1
              sta     dp:V_T0
              ldx     ##0
ip_cmp$:      lda     dp:V_T0
              lsr     a
              bcc     ip_c1$
              eor     ##0xb400
ip_c1$:       sta     dp:V_T0
              sep     #0x20
              cmp     abs:IRAM,x
              rep     #0x20
              beq     ip_c2$
              inc     dp:V_T4
ip_c2$:       inx
              cpx     ##0x800
              bne     ip_cmp$
              rts

;;; Vectors first, then CCNT bit 5 low releases the SA-1; it writes IM_SIG.
check_start:
              lda     ##sa1_reset
              sta     abs:CRV
              lda     ##sa1_nmi
              sta     abs:CNV
              lda     ##sa1_irq
              sta     abs:CIV
              sep     #0x20
              stz     abs:CCNT
              rep     #0x20
              lda     ##IRAM+IM_SIG
              sta     dp:V_WPTR
              lda     ##SIG_LO
              sta     dp:V_WVAL
              lda     ##30
              sta     dp:V_WMAX
              jsr     wait_eq
              sta     dp:V_T4
              bcs     cs_fail$
              lda     abs:IRAM+IM_SIG+2
              cmp     ##SIG_HI
              bne     cs_fail$
              lda     ##1
              sta     dp:V_SA1OK
              lda     ##STATUS_OK
              bra     cs_line$
cs_fail$:     lda     ##STATUS_FAIL
cs_line$:     ldx     ##nm_start
              jsr     line_start
              lda     abs:IRAM+IM_SIG
              jsr     ln_hex4
              ldx     ##s_f
              jsr     ln_str
              lda     dp:V_T4
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              jmp     ln_end

;;; $230E three ways: "a=" lda abs, "00=" lda long $00:230E, "80=" lda long
;;; $80:230E. If it is open bus, each returns the last byte the CPU fetched:
;;; the address high byte ($23), or the bank byte of the long read.
check_vc:
              sep     #0x20
              lda     abs:VC
              sta     dp:V_T0
              lda     long:0x00230e
              sta     dp:V_T1
              lda     long:0x80230e
              sta     dp:V_T2
              rep     #0x20
              lda     ##STATUS_INFO
              ldx     ##nm_vc
              jsr     line_start
              ldx     ##s_vc1
              jsr     ln_str
              lda     dp:V_T0
              jsr     ln_hex2
              ldx     ##s_vc2
              jsr     ln_str
              lda     dp:V_T1
              jsr     ln_hex2
              ldx     ##s_vc3
              jsr     ln_str
              lda     dp:V_T2
              jsr     ln_hex2
              jmp     ln_end

;;; ======================================================= 2. signalling
;;; V_T0..V_T3: frames / message for the two directions of each kind.
;;; A frame count of $FF means the handler never ran.
check_signal:
              ;; S-CPU -> SA-1 IRQ, message 5
              lda     abs:IRAM+IM_IRQN
              sta     dp:V_WVAL
              lda     ##IRAM+IM_IRQN
              sta     dp:V_WPTR
              sep     #0x20
              lda     #0x85
              sta     abs:CCNT
              rep     #0x20
              jsr     sig_wait
              sta     dp:V_T0
              sep     #0x20
              stz     abs:CCNT
              rep     #0x20
              lda     abs:IRAM+IM_IRQM
              sta     dp:V_T1
              ;; S-CPU -> SA-1 NMI, message 6
              lda     abs:IRAM+IM_NMIN
              sta     dp:V_WVAL
              lda     ##IRAM+IM_NMIN
              sta     dp:V_WPTR
              sep     #0x20
              lda     #0x16
              sta     abs:CCNT
              rep     #0x20
              jsr     sig_wait
              sta     dp:V_T2
              sep     #0x20
              stz     abs:CCNT
              rep     #0x20
              lda     abs:IRAM+IM_NMIM
              sta     dp:V_T3

              ldx     ##STATUS_FAIL
              lda     dp:V_T1
              cmp     ##5
              bne     sg1_line$
              lda     dp:V_T3
              cmp     ##6
              bne     sg1_line$
              lda     dp:V_T0
              ora     dp:V_T2
              cmp     ##0xff
              beq     sg1_line$
              ldx     ##STATUS_OK
sg1_line$:    txa
              ldx     ##nm_s2a
              jsr     line_start
              ldx     ##s_irq
              lda     dp:V_T0
              ldy     dp:V_T1
              jsr     sig_pair
              ldx     ##s_nmi
              lda     dp:V_T2
              ldy     dp:V_T3
              jsr     sig_pair
              jsr     ln_end

              ;; SA-1 -> S-CPU IRQ through $FFEE, message $A
              sep     #0x20
              lda     #0x80
              sta     abs:SIE
              rep     #0x20
              stz     dp:V_IRQM
              lda     dp:V_IRQN
              sta     dp:V_T4
              lda     ##0x8a
              sta     abs:IRAM+IM_ARG0
              stz     abs:IRAM+IM_ARG1
              lda     ##CMD_SIGNAL
              jsr     sa1_cmd
              lda     dp:V_T4
              sta     dp:V_WVAL
              lda     ##V_IRQN
              sta     dp:V_WPTR
              jsr     sig_wait
              sta     dp:V_T0
              lda     dp:V_IRQM
              sta     dp:V_T1
              ;; ... and through the SIV override, message $C
              stz     dp:V_IRQM
              lda     dp:V_IRQALT
              sta     dp:V_T4
              lda     ##0xcc
              sta     abs:IRAM+IM_ARG0
              lda     ##s_irq_alt
              sta     abs:IRAM+IM_ARG1
              lda     ##CMD_SIGNAL
              jsr     sa1_cmd
              lda     dp:V_T4
              sta     dp:V_WVAL
              lda     ##V_IRQALT
              sta     dp:V_WPTR
              jsr     sig_wait
              sta     dp:V_T2
              lda     dp:V_IRQM
              sta     dp:V_T3
              stz     abs:IRAM+IM_ARG0        ; SCNT back to 0: ROM vectors
              stz     abs:IRAM+IM_ARG1
              lda     ##CMD_SIGNAL
              jsr     sa1_cmd

              ldx     ##STATUS_FAIL
              lda     dp:V_T1
              cmp     ##0x0a
              bne     sg2_line$
              lda     dp:V_T3
              cmp     ##0x0c
              bne     sg2_line$
              lda     dp:V_T0
              ora     dp:V_T2
              cmp     ##0xff
              beq     sg2_line$
              ldx     ##STATUS_OK
sg2_line$:    txa
              ldx     ##nm_a2s
              jsr     line_start
              ldx     ##s_irq
              lda     dp:V_T0
              ldy     dp:V_T1
              jsr     sig_pair
              ldx     ##s_siv
              lda     dp:V_T2
              ldy     dp:V_T3
              jsr     sig_pair
              jsr     ln_end

              ;; With SIE off: is the request still visible in SFR, and does
              ;; SIC clear it? The documentation is unsure; reported only.
              sep     #0x20
              stz     abs:SIE
              rep     #0x20
              lda     ##0x83
              sta     abs:IRAM+IM_ARG0
              lda     ##CMD_SIGNAL
              jsr     sa1_cmd
              sep     #0x20
              lda     abs:SFR
              sta     dp:V_T0
              lda     #0x80
              sta     abs:SIC
              lda     abs:SFR
              sta     dp:V_T1
              rep     #0x20
              stz     abs:IRAM+IM_ARG0
              lda     ##CMD_SIGNAL
              jsr     sa1_cmd
              lda     ##STATUS_INFO
              ldx     ##nm_sfr
              jsr     line_start
              ldx     ##s_pend
              jsr     ln_str
              lda     dp:V_T0
              and     ##0x80
              eor     ##0x80
              jsr     ln_yn01
              ldx     ##s_clr
              jsr     ln_str
              lda     dp:V_T1
              and     ##0x80
              jsr     ln_yn01
              ldx     ##s_msg
              jsr     ln_str
              lda     dp:V_T0
              and     ##0x0f
              jsr     ln_hex2
              jmp     ln_end

;;; Z set: '1', else '0'.
ln_yn01:
              beq     yn1$
              lda     ##0x30
              jmp     ln_char
yn1$:         lda     ##0x31
              jmp     ln_char

;;; wait_ne for 10 frames on V_WPTR/V_WVAL. A = frames, or $FF on timeout.
sig_wait:
              lda     ##10
              sta     dp:V_WMAX
              jsr     wait_ne
              bcc     sw_ok$
              lda     ##0xff
sw_ok$:       rts

;;; X = label, A = frames, Y = message: " irq f0 m5"
sig_pair:
              pha
              phy
              jsr     ln_str
              lda     ##0x66                  ; 'f'
              jsr     ln_char
              ply
              pla
              phy
              cmp     ##0xff
              bne     sp_num$
              lda     ##0x2d                  ; '-'
              jsr     ln_char
              bra     sp_msg$
sp_num$:      sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
sp_msg$:      lda     ##0x20
              jsr     ln_char
              lda     ##0x6d                  ; 'm'
              jsr     ln_char
              pla
              and     ##0x0f
              tax
              lda     abs:hexdig1,x
              and     ##0xff
              jmp     ln_char

;;; ================================================== 3. arithmetic unit
;;; The SA-1 runs arith_tbl (16-byte records, see tools/mkassets.py) and
;;; leaves MR0-MR4 and OF per record in IM_RES; judged here against the
;;; values computed at build time. Per group (mul, div, sum): V_T0+g*2 total,
;;; V_T3+g*2 passed, first failing record and its low word in GFIRST/GGOT.
GFIRST        .equ    0x0900
GGOT          .equ    0x0910
check_arith:
              ldx     ##10
ca_zero$:     stz     dp:V_T0,x
              dex
              dex
              bpl     ca_zero$
              lda     ##0xffff
              sta     abs:GFIRST
              sta     abs:GFIRST+2
              sta     abs:GFIRST+4
              lda     ##0x3030                ; OF string, '0's
              ldx     ##14
ca_of0$:      sta     abs:OFSTR,x
              dex
              dex
              bpl     ca_of0$
              stz     dp:V_T6                 ; sum records seen

              lda     ##CMD_ARITH
              jsr     sa1_cmd
              bcc     ca_ran$
              lda     ##STATUS_FAIL           ; no SA-1: one line says so
              ldx     ##nm_mul
              jsr     line_start
              ldx     ##s_nosa1
              jsr     ln_str
              jmp     ln_end

ca_ran$:      ldx     ##0                     ; record offset
              ldy     ##0                     ; result offset
              stz     dp:V_T7                 ; record index
ca_rec$:      lda     dp:V_T7
              cmp     ##ARITH_N
              bne     ca_go$
              jmp     ca_print$
ca_go$:       stz     dp:V_PTR                ; mismatch flag
              lda     abs:arith_tbl+14,x      ; judge bits
              and     ##1
              beq     ca_j2$
              lda     abs:IRAM+IM_RES,y
              cmp     abs:arith_tbl+8,x
              bne     ca_bad$
              lda     abs:IRAM+IM_RES+2,y
              cmp     abs:arith_tbl+10,x
              bne     ca_bad$
ca_j2$:       lda     abs:arith_tbl+14,x
              and     ##2
              beq     ca_j4$
              lda     abs:IRAM+IM_RES+4,y
              eor     abs:arith_tbl+12,x
              and     ##0xff
              bne     ca_bad$
ca_j4$:       lda     abs:arith_tbl+14,x
              and     ##4
              beq     ca_tally$
              lda     abs:IRAM+IM_RES+5,y
              eor     abs:arith_tbl+13,x
              and     ##0x80
              beq     ca_tally$
ca_bad$:      inc     dp:V_PTR
ca_tally$:    lda     abs:arith_tbl+15,x      ; group
              and     ##0xff
              asl     a
              phx
              tax
              inc     dp:V_T0,x
              lda     dp:V_PTR
              bne     ca_fail$
              inc     dp:V_T3,x
              bra     ca_of$
ca_fail$:     lda     abs:GFIRST,x
              bpl     ca_of$
              lda     dp:V_T7
              sta     abs:GFIRST,x
              lda     abs:IRAM+IM_RES,y
              sta     abs:GGOT,x
ca_of$:       cpx     ##4                     ; sum group: record its OF
              bne     ca_next$
              lda     abs:IRAM+IM_RES+5,y
              and     ##0x80
              beq     ca_of1$
              ldx     dp:V_T6
              sep     #0x20
              lda     #0x31
              sta     abs:OFSTR,x
              rep     #0x20
ca_of1$:      inc     dp:V_T6
ca_next$:     plx
              txa
              clc
              adc     ##16
              tax
              tya
              clc
              adc     ##6
              tay
              inc     dp:V_T7
              jmp     ca_rec$

ca_print$:    ldy     ##0
              ldx     ##nm_mul
              jsr     arith_line
              ldy     ##2
              ldx     ##nm_div
              jsr     arith_line
              ldy     ##4
              ldx     ##nm_sum
              jsr     arith_line_of
              rts

;;; Y = group * 2, X = name. "5/5", or "4/5 #3=xxxx" (record, got low word).
arith_line_of:
              lda     ##1
              bra     al_go
arith_line:
              lda     ##0
al_go:        sta     dp:V_T7
              lda     abs:V_T3,y
              cmp     abs:V_T0,y
              beq     al_ok$
              lda     ##STATUS_FAIL
              bra     al_st$
al_ok$:       lda     ##STATUS_OK
al_st$:       jsr     line_start
              lda     abs:V_T3,y
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              lda     ##0x2f                  ; '/'
              jsr     ln_char
              lda     abs:V_T0,y
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              lda     abs:GFIRST,y
              bmi     al_of$
              ldx     ##s_hash
              jsr     ln_str
              lda     abs:GFIRST,y
              jsr     ln_hex2
              lda     ##0x3d                  ; '='
              jsr     ln_char
              lda     abs:GGOT,y
              jsr     ln_hex4
al_of$:       lda     dp:V_T7
              beq     al_end$
              ldx     ##s_of
              jsr     ln_str
              ldy     ##0
al_ofc$:      cpy     dp:V_T6
              beq     al_end$
              lda     abs:OFSTR,y
              and     ##0xff
              jsr     ln_char
              iny
              bra     al_ofc$
al_end$:      jmp     ln_end

;;; ============================================================ 4. BW-RAM
;;; Leaves SBWE = CBWE = $80, BWPA = 0, both windows at their idle blocks.
check_bwram:
              sep     #0x20
              lda     #0x80
              sta     abs:SBWE
              rep     #0x20

              ;; boot counter, before anything else writes BW-RAM
              lda     long:0x400000+BW_BOOT
              cmp     ##0x4153                ; "SA1P"
              bne     cb_fresh$
              lda     long:0x400000+BW_BOOT+2
              cmp     ##0x5031
              beq     cb_count$
cb_fresh$:    lda     ##0x4153
              sta     long:0x400000+BW_BOOT
              lda     ##0x5031
              sta     long:0x400000+BW_BOOT+2
              lda     ##0
              sta     long:0x400000+BW_BOOT+4
cb_count$:    lda     long:0x400000+BW_BOOT+4
              inc     a
              sta     long:0x400000+BW_BOOT+4

              ;; size: tag the first word of all 128 8 KB windows of $40-$4F,
              ;; count those that kept their own tag, put everything back
              ldy     ##0
cb_save$:     tya
              jsr     bw_win_ptr
              lda     [dp:V_PTR]
              tyx
              sta     abs:SAVEBUF,x
              iny
              iny
              cpy     ##256
              bne     cb_save$
              ldy     ##0
cb_tag$:      tya
              jsr     bw_win_ptr
              tya
              eor     ##0xa500
              sta     [dp:V_PTR]
              iny
              iny
              cpy     ##256
              bne     cb_tag$
              stz     dp:V_T4                 ; distinct windows
              ldy     ##0
cb_cnt$:      tya
              jsr     bw_win_ptr
              tya
              eor     ##0xa500
              cmp     [dp:V_PTR]
              bne     cb_cnt1$
              inc     dp:V_T4
cb_cnt1$:     iny
              iny
              cpy     ##256
              bne     cb_cnt$
              ldy     ##254                   ; descending: window 0 wins
cb_rest$:     tya
              jsr     bw_win_ptr
              tyx
              lda     abs:SAVEBUF,x
              sta     [dp:V_PTR]
              dey
              dey
              bpl     cb_rest$

              lda     ##STATUS_FAIL
              ldx     dp:V_T4
              cpx     ##32                    ; header claims 256 KB
              bne     cb_sz$
              lda     ##STATUS_OK
cb_sz$:       ldx     ##nm_bwram
              jsr     line_start
              lda     dp:V_T4                 ; * 8 KB
              asl     a
              asl     a
              asl     a
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              ldx     ##s_kb
              jsr     ln_str
              jsr     ln_end

              ;; windows: block k at $6000 on each CPU, k = 0, 1, 31
              stz     dp:V_T4                 ; bit per k: S-CPU ok
              stz     dp:V_T5                 ; bit per k: SA-1 ok
              ldy     ##0
cb_win$:      lda     abs:win_blocks,y
              and     ##0xff
              sta     dp:V_T6
              ora     ##0xb000
              sta     dp:V_T7                 ; tag
              lda     dp:V_T6                 ; -> $40:0010 + k * $2000
              asl     a
              jsr     bw_win_ptr
              lda     dp:V_PTR
              ora     ##0x0010
              sta     dp:V_PTR
              lda     dp:V_T7
              sta     [dp:V_PTR]
              sep     #0x20
              lda     dp:V_T6
              sta     abs:BMAPS
              rep     #0x20
              lda     abs:0x6010
              cmp     dp:V_T7
              bne     cb_ws$
              lda     abs:win_bits,y
              and     ##0xff
              tsb     dp:V_T4
cb_ws$:       lda     dp:V_T6
              sta     abs:IRAM+IM_ARG0
              lda     ##CMD_BWMAP
              jsr     sa1_cmd
              bcs     cb_wa$
              lda     abs:IRAM+IM_RES
              cmp     dp:V_T7
              bne     cb_wa$
              lda     abs:win_bits,y
              and     ##0xff
              tsb     dp:V_T5
cb_wa$:       iny
              cpy     ##3
              bne     cb_win$
              sep     #0x20
              stz     abs:BMAPS
              rep     #0x20
              lda     dp:V_T4
              and     dp:V_T5
              cmp     ##7
              beq     cb_wok$
              lda     ##STATUS_FAIL
              bra     cb_wst$
cb_wok$:      lda     ##STATUS_OK
cb_wst$:      ldx     ##nm_bwwin
              jsr     line_start
              ldx     ##s_wins
              jsr     ln_str
              lda     dp:V_T4
              jsr     ln_bits3
              ldx     ##s_wina
              jsr     ln_str
              lda     dp:V_T5
              jsr     ln_bits3
              ldx     ##s_winl
              jsr     ln_str
              jsr     ln_end

              ;; protection. p0: inside BWPA's area, both enables off; u0:
              ;; outside it; p1: inside, SBWE on; pc: inside, only CBWE on.
              stz     abs:IRAM+IM_ARG0
              lda     ##CMD_CBWE
              jsr     sa1_cmd
              sep     #0x20
              stz     abs:SBWE
              stz     abs:BWPA                ; protect $40:0000-$40:00FF
              rep     #0x20
              lda     ##0x0080
              jsr     bw_try_at
              sta     dp:V_T0
              lda     ##0x0400
              jsr     bw_try_at
              sta     dp:V_T1
              sep     #0x20
              lda     #0x80
              sta     abs:SBWE
              rep     #0x20
              lda     ##0x0080
              jsr     bw_try_at
              sta     dp:V_T2
              sep     #0x20
              stz     abs:SBWE
              rep     #0x20
              lda     ##0x80
              sta     abs:IRAM+IM_ARG0
              lda     ##CMD_CBWE
              jsr     sa1_cmd
              lda     ##0x0080
              jsr     bw_try_at
              sta     dp:V_T3
              sep     #0x20
              lda     #0x80
              sta     abs:SBWE
              rep     #0x20

              lda     ##STATUS_FAIL
              ldx     dp:V_T0
              cpx     ##0x50                  ; 'P'
              bne     cb_pst$
              ldx     dp:V_T2
              cpx     ##0x57                  ; 'W'
              bne     cb_pst$
              lda     ##STATUS_OK
cb_pst$:      ldx     ##nm_bwprot
              jsr     line_start
              ldx     ##s_p0
              lda     dp:V_T0
              jsr     prot_item
              ldx     ##s_u0
              lda     dp:V_T1
              jsr     prot_item
              ldx     ##s_p1
              lda     dp:V_T2
              jsr     prot_item
              ldx     ##s_pc
              lda     dp:V_T3
              jsr     prot_item
              jsr     ln_end

              lda     ##STATUS_INFO
              ldx     ##nm_bwboot
              jsr     line_start
              ldx     ##s_boots
              jsr     ln_str
              lda     long:0x400000+BW_BOOT+4
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              jmp     ln_end

;;; X = label, A = letter.
prot_item:
              pha
              jsr     ln_str
              pla
              jmp     ln_char

;;; A = 3 bits -> "YYY"/"YNY", bit 0 first.
ln_bits3:
              sta     dp:V_T6
              ldy     ##3
lb3$:         lda     ##0x4e                  ; 'N'
              lsr     dp:V_T6
              bcc     lb3_put$
              lda     ##0x59                  ; 'Y'
lb3_put$:     jsr     ln_char
              dey
              bne     lb3$
              rts

;;; A = window * 2 (0-254) -> V_PTR = $40:0000 + window * 8 KB.
bw_win_ptr:
              pha
              and     ##0x0e
              xba
              asl     a
              asl     a
              asl     a
              asl     a
              sta     dp:V_PTR
              pla
              lsr     a
              lsr     a
              lsr     a
              lsr     a
              clc
              adc     ##0x40
              sta     dp:V_PTR+2
              rts

;;; A = BW-RAM offset in bank $40. Tries to flip the byte; puts it back if
;;; that worked. Returns A = 'W' (landed) or 'P'.
bw_try_at:
              sta     dp:V_PTR
              lda     ##0x40
              sta     dp:V_PTR+2
              sep     #0x20
              lda     [dp:V_PTR]
              sta     dp:V_T5
              eor     #0xff
              sta     [dp:V_PTR]
              lda     [dp:V_PTR]
              cmp     dp:V_T5
              beq     bt_p$
              lda     dp:V_T5
              sta     [dp:V_PTR]
              rep     #0x20
              lda     ##0x57
              rts
bt_p$:        rep     #0x20
              lda     ##0x50
              rts

;;; ========================================================= 5. ROM map
;;; V_PTR = a 32 KB-aligned address. Every 32 KB block of the image carries a
;;; marker at +$7F80: "MK", its own ROM offset (24-bit), and that offset
;;; complemented. Returns V_NUM = the offset, or $FFFFFF if no valid marker.
;;; Shared with the SA-1 (it uses its own DP), so: no S-CPU-only state.
rom_mark:
              ldy     ##0x7f80
              lda     [dp:V_PTR],y
              cmp     ##0x4b4d
              bne     rm_bad$
              ldy     ##0x7f82
              lda     [dp:V_PTR],y
              sta     dp:V_NUM
              ldy     ##0x7f84
              lda     [dp:V_PTR],y
              and     ##0xff
              sta     dp:V_NUM+2
              ldy     ##0x7f85
              lda     [dp:V_PTR],y
              eor     ##0xffff
              cmp     dp:V_NUM
              bne     rm_bad$
              ldy     ##0x7f87
              lda     [dp:V_PTR],y
              eor     ##0xff
              and     ##0xff
              cmp     dp:V_NUM+2
              bne     rm_bad$
              rts
rm_bad$:      lda     ##0xffff
              sta     dp:V_NUM
              lda     ##0x00ff
              sta     dp:V_NUM+2
              rts

;;; One line per address in rom_probes, judged against the documented map;
;;; then FXB (banks $A0-$BF / $F0-$FF) moved, and the SA-1's view.
check_rom:
              ldy     ##0
cr_next$:     cpy     ##ROM_PROBES * 8
              bne     cr_go$
              jmp     cr_fxb$
cr_go$:       lda     abs:rom_probes,y
              sta     dp:V_PTR
              lda     abs:rom_probes+2,y
              and     ##0xff
              sta     dp:V_PTR+2
              phy
              jsr     rom_mark
              ply
              ldx     ##STATUS_FAIL
              lda     dp:V_NUM
              cmp     abs:rom_probes+4,y
              bne     cr_st$
              lda     dp:V_NUM+2
              cmp     abs:rom_probes+6,y
              bne     cr_st$
              ldx     ##STATUS_OK
cr_st$:       stx     dp:V_T6
              phy
              jsr     ln_begin
              lda     dp:V_PTR+2              ; name: "bb:aaaa"
              jsr     ln_hex2
              lda     ##0x3a
              jsr     ln_char
              lda     dp:V_PTR
              jsr     ln_hex4
              lda     dp:V_T6
              jsr     ln_status
              jsr     ln_hex6
              jsr     ln_end
              ply
              tya
              clc
              adc     ##8
              tay
              bra     cr_next$

cr_fxb$:      lda     ##0x00                  ; FXB=$00: LoROM $A0 stays put
              ldx     ##nm_fxb00
              ldy     ##0x30
              jsr     fxb_line
              lda     ##0x80                  ; FXB=$80: both follow
              ldx     ##nm_fxb80
              ldy     ##0
              jsr     fxb_line
              sep     #0x20
              lda     #0x03
              sta     abs:FXB
              rep     #0x20

              ;; the SA-1 reads the same five markers through its own bus
              lda     ##CMD_ROMREAD
              jsr     sa1_cmd
              stz     dp:V_T4                 ; matches
              bcs     cr_sa1$
              ldy     ##0
              ldx     ##0
cr_sa1c$:     lda     abs:IRAM+IM_RES,x
              cmp     abs:rom_probes_sa1+4,y
              bne     cr_sa1n$
              lda     abs:IRAM+IM_RES+2,x
              cmp     abs:rom_probes_sa1+6,y
              bne     cr_sa1n$
              inc     dp:V_T4
cr_sa1n$:     inx
              inx
              inx
              inx
              tya
              clc
              adc     ##8
              tay
              cpy     ##ROM_PROBES_SA1 * 8
              bne     cr_sa1c$
cr_sa1$:      lda     ##STATUS_FAIL
              ldx     dp:V_T4
              cpx     ##ROM_PROBES_SA1
              bne     cr_sa1st$
              lda     ##STATUS_OK
cr_sa1st$:    ldx     ##nm_sa1rom
              jsr     line_start
              lda     dp:V_T4
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              ldx     ##s_of5
              jsr     ln_str
              jmp     ln_end

;;; A = FXB value, X = name, Y = expected $A0:8000 offset >> 16 ($30 or 0);
;;; $F0:0000 must read offset 0 either way.
;;; "fxb=80 OK   a0=000000 f0=000000"
fxb_line:
              sty     dp:V_T3
              stx     dp:V_T2
              sep     #0x20
              sta     abs:FXB
              rep     #0x20
              lda     ##0x8000
              sta     dp:V_PTR
              lda     ##0xa0
              sta     dp:V_PTR+2
              jsr     rom_mark
              lda     dp:V_NUM
              sta     dp:V_T4
              lda     dp:V_NUM+2
              sta     dp:V_T5
              stz     dp:V_PTR
              lda     ##0xf0
              sta     dp:V_PTR+2
              jsr     rom_mark
              ldx     ##STATUS_FAIL           ; $F0:0000 must read offset 0
              lda     dp:V_NUM
              ora     dp:V_NUM+2
              bne     fl_st$
              lda     dp:V_T4                 ; $A0:8000
              bne     fl_st$
              lda     dp:V_T5
              cmp     dp:V_T3
              bne     fl_st$
              ldx     ##STATUS_OK
fl_st$:       txa
              ldx     dp:V_T2
              jsr     line_start
              ldx     ##s_a0
              jsr     ln_str
              lda     dp:V_NUM                ; keep $F0's offset for later
              sta     dp:V_T6
              lda     dp:V_NUM+2
              sta     dp:V_T7
              lda     dp:V_T4
              sta     dp:V_NUM
              lda     dp:V_T5
              sta     dp:V_NUM+2
              jsr     ln_hex6
              ldx     ##s_f0
              jsr     ln_str
              lda     dp:V_T6
              sta     dp:V_NUM
              lda     dp:V_T7
              sta     dp:V_NUM+2
              jsr     ln_hex6
              jmp     ln_end

;;; ============================================== 7. character conversion
;;; Type 1: the SA-1 sets DCNT, the S-CPU writes SDA, CDMA, then DDA (the
;;; $2236 write starts it), waits for SFR bit 5, and DMAs from the bitmap's
;;; BW-RAM address; what it reads there is planar tile data.
check_cc:
              ;; the source images, ROM -> BW-RAM
              ldx     ##0
cc_cp8$:      lda     abs:img8,x
              sta     long:0x400000+BW_IMG8,x
              inx
              inx
              cpx     ##2048
              bne     cc_cp8$
              ldx     ##0
cc_cp4$:      lda     abs:img4,x
              sta     long:0x400000+BW_IMG4,x
              inx
              inx
              cpx     ##1024
              bne     cc_cp4$

              ;; 8bpp, 8 chars per line: once read back to WRAM, once to VRAM
              lda     ##BW_IMG8
              ldx     ##0x0c                  ; 8bpp, width 2^3
              jsr     cc_begin
              sta     dp:V_T6                 ; ready? (0 = timed out)
              lda     ##CCREAD & 0xffff
              ldx     ##BW_IMG8
              ldy     ##2048
              jsr     dma_bw_wram
              lda     ##0x0c
              jsr     cc_end
              lda     ##BW_IMG8
              ldx     ##0x0c
              jsr     cc_begin
              jsr     wait_vblank
              sep     #0x20
              lda     #0x80
              sta     abs:INIDISP
              rep     #0x20
              lda     ##VR_PIC + 32           ; tile 1
              sta     abs:VMADDL
              lda     ##BW_IMG8
              sta     abs:A1T0L
              lda     ##2048
              sta     abs:DAS0L
              lda     ##0x1801
              sta     abs:DMAP0
              sep     #0x20
              lda     #0x40
              sta     abs:A1B0
              lda     #1
              sta     abs:MDMAEN
              lda     #0x0f
              sta     abs:INIDISP
              rep     #0x20
              lda     ##0x0c
              jsr     cc_end
              ldx     ##exp8
              ldy     ##2048
              lda     ##0
              jsr     cc_compare
              ldx     ##nm_cc8
              jsr     cc_line

              ;; 4bpp, same layout
              lda     ##BW_IMG4
              ldx     ##0x0d                  ; 4bpp, width 2^3
              jsr     cc_begin
              sta     dp:V_T6
              lda     ##(CCREAD & 0xffff) + 2048
              ldx     ##BW_IMG4
              ldy     ##1024
              jsr     dma_bw_wram
              lda     ##0x0d
              jsr     cc_end
              ldx     ##exp4
              ldy     ##1024
              lda     ##2048
              jsr     cc_compare
              ldx     ##nm_cc4
              jsr     cc_line

              ;; type 2: the SA-1 converts one tile into I-RAM
              stz     dp:V_T6
              stz     dp:V_T4
              lda     ##CMD_CC2
              jsr     sa1_cmd
              bcs     cc2_line$
              inc     dp:V_T6
              ldx     ##0
cc2_cmp$:     sep     #0x20
              lda     abs:IRAM+CC_BUF,x
              cmp     abs:exp8,x
              rep     #0x20
              beq     cc2_ok$
              inc     dp:V_T4
cc2_ok$:      inx
              cpx     ##64
              bne     cc2_cmp$
cc2_line$:    ldx     ##nm_cc2
              jsr     cc_line1

              ;; throughput: 256 tiles at 8bpp (128 x 128, 16 chars wide)
              lda     ##BW_RATE
              ldx     ##0x10
              jsr     cc_begin
              sta     dp:V_T6
              jsr     rate_dma
              sta     dp:V_T4                 ; lines
              lda     ##0x10
              jsr     cc_end
              jsr     rate_dma                ; the same DMA, conversion off
              sta     dp:V_T5

              ldx     ##STATUS_FAIL
              lda     dp:V_T6
              beq     cr1$
              lda     dp:V_T4
              beq     cr1$
              ldx     ##STATUS_OK
cr1$:         txa
              ldx     ##nm_ccrate
              jsr     line_start
              lda     dp:V_T4
              jsr     rate_print
              jsr     ln_end
              lda     ##STATUS_INFO
              ldx     ##nm_ccplain
              jsr     line_start
              lda     dp:V_T5
              jsr     rate_print
              jmp     ln_end

;;; A = lines for 256 tiles. "698/frm 98/vbl" (262 lines, 37 of them blank).
rate_print:
              sta     dp:V_T7
              cmp     ##0
              bne     rp_ok$
              ldx     ##s_dash
              jmp     ln_str
rp_ok$:       lda     ##256 * 262 & 0xffff
              sta     dp:V_NUM
              lda     ##256 * 262 >> 16
              sta     dp:V_NUM+2
              lda     dp:V_T7
              sta     dp:V_DEN
              stz     dp:V_DEN+2
              jsr     udiv32
              jsr     ln_dec
              ldx     ##s_frm
              jsr     ln_str
              lda     ##256 * 37
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              lda     dp:V_T7
              sta     dp:V_DEN
              stz     dp:V_DEN+2
              jsr     udiv32
              jsr     ln_dec
              ldx     ##s_vbl
              jmp     ln_str

;;; 16 KB from $40:8000 to the WRAM sink. Returns A = scanlines it took.
rate_dma:
              jsr     wait_vblank
              jsr     vcount
              sta     dp:V_T2
              sep     #0x20
              stz     abs:WMADDL
              stz     abs:WMADDL+1
              lda     #SINK >> 16
              and     #1
              sta     abs:WMADDH
              rep     #0x20
              lda     ##0x8000                ; mode 0 to $2180
              sta     abs:DMAP0
              lda     ##BW_RATE
              sta     abs:A1T0L
              lda     ##16384
              sta     abs:DAS0L
              sep     #0x20
              lda     #0x40
              sta     abs:A1B0
              lda     #1
              sta     abs:MDMAEN
              rep     #0x20
              jsr     vcount
              sec
              sbc     dp:V_T2
              bpl     rd_pos$
              clc
              adc     ##262
rd_pos$:      rts

;;; A = the PPU's V counter, latched now.
vcount:
              sep     #0x20
              lda     abs:STAT78              ; resets the OPVCT byte flip-flop
              lda     abs:SLHV
              lda     abs:OPVCT
              xba
              lda     abs:OPVCT
              xba
              rep     #0x20
              and     ##0x1ff
              rts

;;; A = BW-RAM offset of the bitmap, X = CDMA value. Starts type 1 and waits
;;; (bounded) for SFR bit 5. Returns A = nonzero if it came.
cc_begin:
              sta     dp:V_T0
              stx     dp:V_T1
              lda     ##0xb0                  ; enable, char conv, type 1
              sta     abs:IRAM+IM_ARG0
              stz     abs:IRAM+IM_ARG1
              lda     ##CMD_DCNT
              jsr     sa1_cmd
              sep     #0x20
              lda     #0x20
              sta     abs:SIC
              rep     #0x20
              lda     dp:V_T0
              sta     abs:SDA
              sep     #0x20
              stz     abs:SDA+2
              lda     dp:V_T1
              sta     abs:CDMA
              stz     abs:DDA
              lda     #CC_BUF >> 8
              sta     abs:DDA+1               ; starts the conversion
              rep     #0x20
              ldx     ##0
cb_wait$:     sep     #0x20
              lda     abs:SFR
              rep     #0x20
              and     ##0x20
              bne     cb_got$
              dex
              bne     cb_wait$
              lda     ##0
cb_got$:      rts

;;; A = the CDMA value used. Sets CDMA bit 7 from both sides (the register
;;; is shared; fullsnes has the SA-1 do it), clears the flag, DCNT off.
cc_end:
              ora     ##0x80
              sta     abs:IRAM+IM_ARG1
              sep     #0x20
              sta     abs:CDMA
              lda     #0x20
              sta     abs:SIC
              rep     #0x20
              stz     abs:IRAM+IM_ARG0
              lda     ##CMD_DCNT
              jmp     sa1_cmd

;;; A = WRAM $7E offset, X = BW-RAM offset, Y = bytes.
dma_bw_wram:
              sta     abs:WMADDL
              sep     #0x20
              stz     abs:WMADDH
              rep     #0x20
              stx     abs:A1T0L
              sty     abs:DAS0L
              lda     ##0x8000
              sta     abs:DMAP0
              sep     #0x20
              lda     #0x40
              sta     abs:A1B0
              lda     #1
              sta     abs:MDMAEN
              rep     #0x20
              rts

;;; A = offset into CCREAD, X = expected data, Y = bytes. V_T4 = bad bytes,
;;; V_T5 = bytes compared.
cc_compare:
              sta     dp:V_T2
              stx     dp:V_T3
              sty     dp:V_T5
              stz     dp:V_T4
              lda     dp:V_T2
              clc
              adc     dp:V_T5
              sta     dp:V_T7                 ; end
              ldx     dp:V_T2
              ldy     ##0
ccc$:         sep     #0x20
              lda     long:CCREAD,x
              cmp     (dp:V_T3),y
              rep     #0x20
              beq     ccc_ok$
              inc     dp:V_T4
ccc_ok$:      inx
              iny
              cpx     dp:V_T7
              bne     ccc$
              rts

;;; X = name. Uses V_T4 (bad bytes), V_T6 (ready flag).
cc_line:
              lda     ##s_32t
              bra     cl_go
cc_line1:     lda     ##s_1t
cl_go:        sta     dp:V_T7
              lda     ##STATUS_FAIL
              ldy     dp:V_T4
              bne     cl_st$
              ldy     dp:V_T6
              beq     cl_st$
              lda     ##STATUS_OK
cl_st$:       jsr     line_start
              lda     dp:V_T6
              bne     cl_rdy$
              ldx     ##s_noirq
              jsr     ln_str
cl_rdy$:      ldx     dp:V_T7
              jsr     ln_str
              lda     dp:V_T4
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              ldx     ##s_bad
              jsr     ln_str
              jmp     ln_end

;;; -------------------------------------------------------------- tables
;;; address (24-bit + pad), expected ROM offset (32-bit): the documented map
;;; with the reset MMC values $00-$03 and a 4 MB image.
rom_probes:   .long   0x008000, 0x000000
              .long   0x018000, 0x008000
              .long   0x208000, 0x100000
              .long   0x808000, 0x200000
              .long   0xa08000, 0x300000
              .long   0xc00000, 0x000000
              .long   0xc08000, 0x008000
              .long   0xd00000, 0x100000
              .long   0xf00000, 0x300000
              .long   0xff8000, 0x3f8000
;;; What the SA-1 reads in cmd_romread; same layout.
              .public rom_probes_sa1
rom_probes_sa1:
              .long   0x008000, 0x000000
              .long   0xc00000, 0x000000
              .long   0xc08000, 0x008000
              .long   0xa08000, 0x300000
              .long   0xf00000, 0x300000

win_blocks:   .byte   0, 1, 31
win_bits:     .byte   1, 2, 4

hexdig1:      .ascii  "0123456789ABCDEF"
nm_iram:      .asciz  "iram"
nm_iramwp:    .asciz  "iramwp"
nm_start:     .asciz  "start"
nm_vc:        .asciz  "vc"
nm_s2a:       .asciz  "s>a"
nm_a2s:       .asciz  "a>s"
nm_sfr:       .asciz  "sfr"
nm_mul:       .asciz  "mul"
nm_div:       .asciz  "div"
nm_sum:       .asciz  "sum"
nm_bwram:     .asciz  "bwram"
nm_bwwin:     .asciz  "bwwin"
nm_bwprot:    .asciz  "bwprot"
nm_bwboot:    .asciz  "bwboot"
nm_fxb00:     .asciz  "fxb=00"
nm_fxb80:     .asciz  "fxb=80"
nm_sa1rom:    .asciz  "sa1rom"
nm_cc8:       .asciz  "cc8"
nm_cc4:       .asciz  "cc4"
nm_cc2:       .asciz  "cc2"
nm_ccrate:    .asciz  "ccrate"
nm_ccplain:   .asciz  "ccplain"
s_iram_ok:    .asciz  "2048 B"
s_bad:        .asciz  " bad"
s_rst:        .asciz  "rst="
s_b3:         .asciz  " b3="
s_f:          .asciz  " f"
s_vc1:        .asciz  "a="
s_vc2:        .asciz  " 00="
s_vc3:        .asciz  " 80="
s_irq:        .asciz  "irq "
s_nmi:        .asciz  " nmi "
s_siv:        .asciz  " siv "
s_pend:       .asciz  "pend="
s_clr:        .asciz  " clr="
s_msg:        .asciz  " msg="
s_nosa1:      .asciz  "no SA-1"
s_hash:       .asciz  " #"
s_of:         .asciz  " of="
s_kb:         .asciz  "K $40-$4F"
s_wins:       .asciz  "S="
s_wina:       .asciz  " A="
s_winl:       .asciz  " 0,1,31"
s_p0:         .asciz  "p0="
s_u0:         .asciz  " u0="
s_p1:         .asciz  " p1="
s_pc:         .asciz  " pc="
s_boots:      .asciz  "boots="
s_a0:         .asciz  "a0="
s_f0:         .asciz  " f0="
s_of5:        .asciz  "/5 same"
s_32t:        .asciz  "32 tiles "
s_1t:         .asciz  "1 tile "
s_noirq:      .asciz  "no flag "
s_frm:        .asciz  "/frm "
s_vbl:        .asciz  "/vbl"
s_dash:       .asciz  "-"
