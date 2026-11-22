;;; S-CPU side: boot, screen, the check sequence, the verdict and the page
;;; loop. NMI stays disabled throughout: frames are counted by polling HVBJOY,
;;; and the S-CPU's own benchmarks are timed by the V-timer IRQ.
;;;
;;; Conventions for every S-CPU routine unless it says otherwise: enter and
;;; leave with A/X/Y 16-bit, D=0, DB=$00, so abs $0000-$1FFF is WRAM, $3000
;;; I-RAM, $6000 the BW-RAM window and $8000+ this ROM bank.

#include "regs.inc"
#include "layout.inc"
#include "assets.inc"

              .section code, root
              .public s_reset, s_irq, s_irq_alt, s_rti
              .public wait_eq, wait_ne, sa1_cmd, dma_vram
              .extern ln_begin, ln_char, ln_str, ln_col, ln_dec, ln_end, scr_flush
              .extern wait_vblank, mb_init, STATUS_OK, STATUS_FAIL
              .extern check_iram, check_start, check_vc, check_signal, check_arith
              .extern check_bwram, check_rom, check_cc, check_speed
              .extern font4, font4_end, palette, palette_end

s_rti:        rti

s_reset:
              sei
              clc
              xce
              rep     #0x38
              ldx     ##S_STACK
              txs
              lda     ##0
              tcd
              phk
              plb
              sep     #0x20
              lda     #0x80
              sta     abs:INIDISP             ; forced blank
              stz     abs:NMITIMEN
              stz     abs:HDMAEN
              stz     abs:MDMAEN
              stz     abs:MEMSEL              ; SlowROM, as the header says
              rep     #0x20
              ldx     ##0xfe                  ; clear the direct page
rs_dp$:       stz     dp:0,x
              dex
              dex
              bpl     rs_dp$
              ldx     ##0x0dfe                ; and $0200-$0FFF: buffers, results
rs_ram$:      stz     abs:0x0200,x
              dex
              dex
              bpl     rs_ram$
              lda     ##TOP_ROW
              sta     dp:V_ROWS
              sta     dp:V_ROWS+2
              sta     dp:V_ROWS+4
              jsr     mb_init
              jsr     ppu_init
              jsr     shadow_clear
              sep     #0x20
              lda     #0x0f
              sta     abs:INIDISP
              rep     #0x20
              cli

              stz     dp:V_PAGE
              ldx     ##s_title1
              jsr     title_line
              jsr     check_iram
              jsr     check_start
              jsr     check_vc
              jsr     check_signal
              jsr     check_arith
              jsr     check_bwram

              lda     ##1
              sta     dp:V_PAGE
              ldx     ##s_title2
              jsr     title_line
              jsr     check_rom
              jsr     check_cc

              lda     ##2
              sta     dp:V_PAGE
              ldx     ##s_title3
              jsr     title_line
              ldx     ##s_legend1
              jsr     title_line
              ldx     ##s_legend2
              jsr     title_line
              jsr     check_speed

              jsr     verdict
              lda     ##1
              sta     long:MBX_STATUS
              jmp     page_loop

;;; X = string; a plain line (no status) on page V_PAGE.
title_line:
              phx
              jsr     ln_begin
              plx
              jsr     ln_str
              jmp     ln_end

;;; The verdict, two lines on every page, in the transcript once.
verdict:
              lda     ##2
              sta     dp:V_PAGE
vd_page$:     jsr     ln_begin
              ldx     ##s_verdict
              jsr     ln_str
              lda     ##8
              jsr     ln_col
              ldx     ##s_vok
              lda     dp:V_NFAIL
              beq     vd_st$
              ldx     ##s_vfail
vd_st$:       jsr     ln_str
              lda     ##13
              jsr     ln_col
              lda     dp:V_NOK
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              ldx     ##s_vok2
              jsr     ln_str
              lda     dp:V_NFAIL
              sta     dp:V_NUM
              stz     dp:V_NUM+2
              jsr     ln_dec
              ldx     ##s_vfail2
              jsr     ln_str
              jsr     ln_end
              jsr     ln_begin
              ldx     ##s_fails
              jsr     ln_str
              ldy     ##0
vd_fl$:       cpy     dp:V_FLEN
              beq     vd_fl_end$
              lda     abs:FAILLIST,y
              and     ##0xff
              jsr     ln_char
              iny
              bra     vd_fl$
vd_fl_end$:   jsr     ln_end
              lda     ##1                     ; transcript only once
              sta     dp:V_LNMODE
              dec     dp:V_PAGE
              bpl     vd_page$
              stz     dp:V_LNMODE
              stz     dp:V_PAGE
              rts

;;; A (or any button but the D-pad) shows the next page.
page_loop:
              sep     #0x20
              lda     #0x01                   ; auto-joypad, still no NMI
              sta     abs:NMITIMEN
              rep     #0x20
pl_frame$:    jsr     wait_vblank
pl_joy$:      lda     abs:HVBJOY
              and     ##1
              bne     pl_joy$
              lda     abs:JOY1L
              and     ##0xc0f0                ; B Y A X L R
              tax
              eor     dp:V_PAD
              stx     dp:V_PAD
              and     dp:V_PAD                ; newly pressed
              beq     pl_frame$
              lda     dp:V_SHOWN
              inc     a
              cmp     ##3
              bcc     pl_set$
              lda     ##0
pl_set$:      sta     dp:V_SHOWN
              jsr     scr_flush
              bra     pl_frame$

;;; ------------------------------------------------------------ waits
;;; Poll the word at V_WPTR (bank $00) until it equals / differs from V_WVAL,
;;; for at most V_WMAX vertical blanks. Returns A = blanks seen, C=1 timeout.
;;; Touches only V_W*, V_FRAMES and A.
wait_eq:      lda     ##0xffff
              bra     wait_go
wait_ne:      lda     ##0
wait_go:
              sta     dp:V_WMODE
              stz     dp:V_FRAMES
              lda     abs:HVBJOY
              and     ##0x80
              sta     dp:V_WPREV
wg_poll$:     lda     (dp:V_WPTR)
              cmp     dp:V_WVAL
              beq     wg_same$
              lda     dp:V_WMODE
              beq     wg_hit$
              bra     wg_tick$
wg_same$:     lda     dp:V_WMODE
              bne     wg_hit$
wg_tick$:     lda     abs:HVBJOY
              and     ##0x80
              cmp     dp:V_WPREV
              beq     wg_poll$
              sta     dp:V_WPREV
              cmp     ##0
              beq     wg_poll$
              inc     dp:V_FRAMES
              lda     dp:V_FRAMES
              cmp     dp:V_WMAX
              bcc     wg_poll$
              sec
              rts
wg_hit$:      lda     dp:V_FRAMES
              clc
              rts

;;; A = command. Runs it on the SA-1 and waits up to 60 frames for IM_CMD to
;;; return to 0. C=1: timed out, or the SA-1 never started.
sa1_cmd:
              ldx     dp:V_SA1OK
              beq     sc_dead$
              sta     abs:IRAM+IM_CMD
              lda     ##IRAM+IM_CMD
              sta     dp:V_WPTR
              stz     dp:V_WVAL
              lda     ##60
              sta     dp:V_WMAX
              jmp     wait_eq
sc_dead$:     sec
              rts

;;; ---------------------------------------------------------- interrupts
;;; IRQ: the V-timer (S-CPU benchmarks) and the SA-1's IRQ to the S-CPU.
;;; abs addressing with DB=$00 throughout: D is the benchmark's when it lands.
s_irq:
              rep     #0x30
              pha
              sep     #0x20
              lda     abs:TIMEUP
              bpl     si_sa1$
              rep     #0x20
              inc     abs:V_TICKS
              lda     abs:V_LEFT
              beq     si_sa1$
              dec     a
              sta     abs:V_LEFT
              bne     si_sa1$
              inc     abs:IM_STOP             ; WRAM $0200 on this CPU
si_sa1$:      sep     #0x20
              lda     abs:SFR
              bpl     si_out$
              and     #0x0f
              sta     abs:V_IRQM
              stz     abs:V_IRQM+1
              lda     #0x80
              sta     abs:SIC
              rep     #0x20
              inc     abs:V_IRQN
si_out$:      rep     #0x30
              pla
              rti

;;; Reached only through the SIV override ($2209 bit 6).
s_irq_alt:
              rep     #0x30
              pha
              sep     #0x20
              lda     abs:SFR
              and     #0x0f
              sta     abs:V_IRQM
              stz     abs:V_IRQM+1
              lda     #0x80
              sta     abs:SIC
              rep     #0x20
              inc     abs:V_IRQALT
              pla
              rti

;;; ---------------------------------------------------------------- PPU
ppu_init:
              sep     #0x20
              ldx     ##0x2101                ; $2101-$210C
pi_zero$:     stz     abs:0,x
              inx
              cpx     ##0x210d
              bne     pi_zero$
              ldx     ##0x210d                ; scroll registers, written twice
pi_scroll$:   stz     abs:0,x
              stz     abs:0,x
              inx
              cpx     ##0x2115
              bne     pi_scroll$
              lda     #0xff                   ; V scroll -1: map row 0 at line 0
              sta     abs:BG1VOFS
              lda     #0x03
              sta     abs:BG1VOFS
              lda     #0xff
              sta     abs:BG2VOFS
              lda     #0x03
              sta     abs:BG2VOFS
              ldx     ##0x2123                ; windows, colour math, SETINI
pi_misc$:     stz     abs:0,x
              inx
              cpx     ##0x2134
              bne     pi_misc$
              lda     #0x03
              sta     abs:BGMODE              ; Mode 3: BG1 8bpp, BG2 4bpp
              lda     #(VR_PICMAP >> 10) << 2
              sta     abs:BG1SC
              lda     #(VR_TEXTMAP >> 10) << 2
              sta     abs:BG2SC
              lda     #(VR_PIC >> 12) | ((VR_FONT >> 12) << 4)
              sta     abs:BG12NBA
              lda     #0x02
              sta     abs:TM                  ; BG2 text; scr_flush adds BG1
              lda     #0x80
              sta     abs:VMAIN
              rep     #0x20

              stz     abs:VMADDL              ; clear all of VRAM
              lda     ##0x1809                ; mode 1, fixed source
              sta     abs:DMAP0
              lda     ##zero_word
              sta     abs:A1T0L
              stz     abs:DAS0L               ; 0 = 64 KB
              sep     #0x20
              stz     abs:A1B0
              lda     #1
              sta     abs:MDMAEN
              rep     #0x20

              lda     ##VR_FONT
              ldx     ##font4
              ldy     ##FONT4_SIZE
              jsr     dma_vram

              sep     #0x20
              stz     abs:CGADD
              rep     #0x20
              lda     ##0x2200                ; mode 0 to $2122
              sta     abs:DMAP0
              lda     ##palette
              sta     abs:A1T0L
              lda     ##PALETTE_SIZE
              sta     abs:DAS0L
              sep     #0x20
              stz     abs:A1B0
              lda     #1
              sta     abs:MDMAEN
              rep     #0x20

              ;; BG1 map: the 8 x 4 picture, tiles 1-32; tile 0 stays blank.
              lda     ##1
              sta     dp:V_T2
              ldy     ##0
pi_row$:      tya
              asl     a
              asl     a
              asl     a
              asl     a
              asl     a
              clc
              adc     ##VR_PICMAP + PIC_ROW * 32 + PIC_COL
              sta     abs:VMADDL
              ldx     ##8
pi_cell$:     lda     dp:V_T2
              sta     abs:VMDATAL
              inc     dp:V_T2
              dex
              bne     pi_cell$
              iny
              cpy     ##4
              bne     pi_row$
              rts

;;; A = VRAM word address, X = source in bank $00, Y = bytes. Call in blank.
dma_vram:
              sta     abs:VMADDL
              stx     abs:A1T0L
              sty     abs:DAS0L
              lda     ##0x1801
              sta     abs:DMAP0
              sep     #0x20
              stz     abs:A1B0
              lda     #1
              sta     abs:MDMAEN
              rep     #0x20
              rts

shadow_clear:
              ldx     ##3 * SHADOW_PAGE - 2
              lda     ##0x0020
sc_loop$:     sta     long:SHADOW,x
              dex
              dex
              bpl     sc_loop$
              rts

zero_word:    .word   0
s_title1:     .asciz  "SA1PROBE p1/3 A:next"
s_title2:     .asciz  "ROM MAP p2/3 A:next"
s_title3:     .asciz  "SPEED 16 fr/run p3/3 A:next"
s_legend1:    .asciz  "S S-CPU; I/B SA-1 IRAM/BWRAM"
s_legend2:    .asciz  "i S idle in WRAM, b busy in ROM"
s_verdict:    .asciz  "verdict"
s_vok:        .asciz  "OK"
s_vfail:      .asciz  "FAIL"
s_vok2:       .asciz  " ok "
s_vfail2:     .asciz  " fail"
s_fails:      .asciz  "fails: "
