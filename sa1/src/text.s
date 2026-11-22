;;; Report output: a 32-column line buffer that lands both in a screen shadow
;;; page (WRAM, DMA'd to the BG2 map) and in the test mailbox transcript.
;;;
;;; S-CPU only. All routines: enter and leave with A/X/Y 16-bit, D=0, DB=0.
;;; ln_* clobber A and X; Y survives unless stated. Scratch is V_L* only, so
;;; values a caller keeps in V_T* survive building a line.

#include "regs.inc"
#include "layout.inc"

STATUS_COL    .equ    8
VALUE_COL     .equ    13

              .section code, root
              .public ln_begin, ln_char, ln_str, ln_col, ln_hex2, ln_hex4, ln_hex6
              .public ln_dec, ln_ratio, ln_status, ln_end, ln_yn
              .public udiv32, wait_vblank, scr_flush, mb_init
              .public STATUS_OK, STATUS_FAIL, STATUS_INFO

STATUS_OK     .equ    0
STATUS_FAIL   .equ    1
STATUS_INFO   .equ    2

ln_begin:
              lda     ##0x2020
              ldx     ##30
lb_fill$:     sta     abs:LINEBUF,x
              dex
              dex
              bpl     lb_fill$
              stz     dp:V_COL
              rts

;;; A low byte. A full line drops the character.
ln_char:
              ldx     dp:V_COL
              cpx     ##32
              bcs     lc_full$
              sep     #0x20
              sta     abs:LINEBUF,x
              rep     #0x20
              inx
              stx     dp:V_COL
lc_full$:     rts

;;; X = address of a NUL-terminated string in bank $00.
ln_str:
              stx     dp:V_L0
ls_next$:     lda     (dp:V_L0)
              and     ##0xff
              beq     ls_done$
              jsr     ln_char
              inc     dp:V_L0
              bra     ls_next$
ls_done$:     rts

;;; Pad with spaces up to column A.
ln_col:
              sta     dp:V_L1
lcl_next$:    lda     dp:V_COL
              cmp     dp:V_L1
              bcs     lcl_done$
              lda     ##0x20
              jsr     ln_char
              bra     lcl_next$
lcl_done$:    rts

;;; Z set: 'W' (the write landed), else 'P'.
ln_yn:
              beq     lyn_w$
              lda     ##0x50
              jmp     ln_char
lyn_w$:       lda     ##0x57
              jmp     ln_char

ln_hex4:
              pha
              xba
              jsr     ln_hex2
              pla
ln_hex2:
              and     ##0xff
              pha
              lsr     a
              lsr     a
              lsr     a
              lsr     a
              jsr     ln_nib
              pla
              and     ##0x0f
ln_nib:
              tax
              lda     abs:hexdig,x
              and     ##0xff
              jmp     ln_char

;;; V_NUM low 24 bits.
ln_hex6:
              lda     dp:V_NUM+2
              jsr     ln_hex2
              lda     dp:V_NUM
              jmp     ln_hex4

;;; V_NUM as unsigned decimal. Destroys V_NUM, V_DEN, V_REM; Y survives.
ln_dec:
              phy
              ldy     ##0
ld_div$:      lda     ##10
              sta     dp:V_DEN
              stz     dp:V_DEN+2
              jsr     udiv32
              lda     dp:V_REM
              pha
              iny
              lda     dp:V_NUM
              ora     dp:V_NUM+2
              bne     ld_div$
ld_out$:      pla
              clc
              adc     ##0x30
              jsr     ln_char
              dey
              bne     ld_out$
              ply
              rts

;;; V_NUM / V_DEN as "x12.3", rounded. Destroys V_NUM, V_DEN, V_REM.
ln_ratio:
              lda     dp:V_DEN
              ora     dp:V_DEN+2
              bne     lr_ok$
              ldx     ##s_noratio
              jmp     ln_str
lr_ok$:       lda     dp:V_NUM                ; V_NUM *= 10: (x*4 + x) * 2
              sta     dp:V_L2
              lda     dp:V_NUM+2
              sta     dp:V_L3
              asl     dp:V_NUM
              rol     dp:V_NUM+2
              asl     dp:V_NUM
              rol     dp:V_NUM+2
              clc
              lda     dp:V_NUM
              adc     dp:V_L2
              sta     dp:V_NUM
              lda     dp:V_NUM+2
              adc     dp:V_L3
              sta     dp:V_NUM+2
              asl     dp:V_NUM
              rol     dp:V_NUM+2
              lda     dp:V_DEN+2              ; + V_DEN / 2
              lsr     a
              sta     dp:V_L3
              lda     dp:V_DEN
              ror     a
              clc
              adc     dp:V_NUM
              sta     dp:V_NUM
              lda     dp:V_L3
              adc     dp:V_NUM+2
              sta     dp:V_NUM+2
              jsr     udiv32
              lda     ##0x78                  ; 'x'
              jsr     ln_char
              lda     ##10
              sta     dp:V_DEN
              stz     dp:V_DEN+2
              jsr     udiv32
              lda     dp:V_REM
              pha
              jsr     ln_dec
              lda     ##0x2e                  ; '.'
              jsr     ln_char
              pla
              clc
              adc     ##0x30
              jmp     ln_char

;;; V_NUM = V_NUM / V_DEN, V_REM = remainder; 32-bit unsigned. Uses X.
udiv32:
              stz     dp:V_REM
              stz     dp:V_REM+2
              ldx     ##32
ud_bit$:      asl     dp:V_NUM
              rol     dp:V_NUM+2
              rol     dp:V_REM
              rol     dp:V_REM+2
              lda     dp:V_REM
              sec
              sbc     dp:V_DEN
              sta     dp:V_L4
              lda     dp:V_REM+2
              sbc     dp:V_DEN+2
              bcc     ud_skip$
              sta     dp:V_REM+2
              lda     dp:V_L4
              sta     dp:V_REM
              inc     dp:V_NUM
ud_skip$:     dex
              bne     ud_bit$
              rts

;;; The name is already at column 0. A = STATUS_*; tallies it, writes the
;;; status column and leaves the cursor at the value column.
ln_status:
              pha
              lda     ##STATUS_COL
              jsr     ln_col
              pla
              cmp     ##STATUS_FAIL
              beq     lst_fail$
              bcs     lst_info$
              inc     dp:V_NOK
              ldx     ##s_ok
              bra     lst_put$
lst_fail$:    inc     dp:V_NFAIL
              jsr     fail_add
              ldx     ##s_fail
              bra     lst_put$
lst_info$:    ldx     ##s_info
lst_put$:     jsr     ln_str
              lda     ##VALUE_COL
              jmp     ln_col

;;; Append the line's name (up to the first space) to FAILLIST.
fail_add:
              phy
              ldy     ##0
              ldx     dp:V_FLEN
fa_next$:     cpx     ##56
              bcs     fa_done$
              lda     abs:LINEBUF,y
              and     ##0xff
              cmp     ##0x20
              beq     fa_sep$
              sep     #0x20
              sta     abs:FAILLIST,x
              rep     #0x20
              inx
              iny
              cpy     ##STATUS_COL
              bne     fa_next$
fa_sep$:      sep     #0x20
              lda     #0x20
              sta     abs:FAILLIST,x
              rep     #0x20
              inx
fa_done$:     stx     dp:V_FLEN
              ply
              rts

;;; Finish the line: into the shadow of page V_PAGE at its next row (rows past
;;; 27 are dropped), then into the transcript, trailing spaces trimmed.
;;; V_LNMODE bit0 skips the transcript, bit1 the screen. Flushes the screen if
;;; V_PAGE is the page shown.
ln_end:
              phy
              lda     dp:V_LNMODE
              and     ##2
              bne     le_mail$
              lda     dp:V_PAGE
              asl     a
              tax
              lda     dp:V_ROWS,x
              cmp     ##28
              bcs     le_mail$
              inc     dp:V_ROWS,x
              asl     a                       ; row * 64 bytes
              asl     a
              asl     a
              asl     a
              asl     a
              asl     a
              sta     dp:V_L0
              lda     dp:V_PAGE               ; + page * $800
              xba
              asl     a
              asl     a
              asl     a
              clc
              adc     dp:V_L0
              tax
              ldy     ##0
le_copy$:     lda     abs:LINEBUF,y
              and     ##0xff
              sta     long:SHADOW,x
              inx
              inx
              iny
              cpy     ##32
              bne     le_copy$
le_mail$:     lda     dp:V_LNMODE
              and     ##1
              bne     le_flush$
              ldy     ##32
le_trim$:     dey
              bmi     le_nl$
              lda     abs:LINEBUF,y
              and     ##0xff
              cmp     ##0x20
              beq     le_trim$
              iny
              sty     dp:V_L1
              ldy     ##0
le_put$:      lda     abs:LINEBUF,y
              jsr     mb_putc
              iny
              cpy     dp:V_L1
              bne     le_put$
le_nl$:       lda     ##10
              jsr     mb_putc
le_flush$:    lda     dp:V_PAGE
              cmp     dp:V_SHOWN
              bne     le_done$
              jsr     scr_flush
le_done$:     ply
              rts

;;; A low byte into the transcript ring. Keeps X and Y.
mb_putc:
              phx
              ldx     dp:V_MBPOS
              sep     #0x20
              sta     long:MBX_RING,x
              rep     #0x20
              inx
              cpx     ##MBX_RINGSIZE
              bcc     mp_nowrap$
              ldx     ##0
mp_nowrap$:   stx     dp:V_MBPOS
              lda     long:MBX_WINDEX
              inc     a
              sta     long:MBX_WINDEX
              plx
              rts

mb_init:
              lda     ##0
              sta     long:MBX_WINDEX
              sta     long:MBX_STATUS
              stz     dp:V_MBPOS
              lda     ##0xcafe
              sta     long:MBX_MAGIC
              rts

;;; Returns at the start of the next vertical blank.
wait_vblank:
              sep     #0x20
wv_out$:      lda     abs:HVBJOY
              bmi     wv_out$
wv_in$:       lda     abs:HVBJOY
              bpl     wv_in$
              rep     #0x20
              rts

;;; Page V_SHOWN's shadow -> BG2 map, at the next vertical blank (2 KB takes
;;; about 12 of its 37 lines). The picture layer is on for page 2 only.
scr_flush:
              jsr     wait_vblank
              sep     #0x20
              lda     #0x02
              ldx     dp:V_SHOWN
              cpx     ##1
              bne     sf_tm$
              lda     #0x03
sf_tm$:       sta     abs:TM
              lda     #0x80
              sta     abs:VMAIN
              rep     #0x20
              lda     ##VR_TEXTMAP
              sta     abs:VMADDL
              lda     ##0x1801                ; mode 1 to $2118/$2119
              sta     abs:DMAP0
              lda     dp:V_SHOWN
              xba
              asl     a
              asl     a
              asl     a
              clc
              adc     ##SHADOW & 0xffff
              sta     abs:A1T0L
              lda     ##SHADOW_PAGE
              sta     abs:DAS0L
              sep     #0x20
              lda     #SHADOW >> 16
              sta     abs:A1B0
              lda     #1
              sta     abs:MDMAEN
              rep     #0x20
              rts

hexdig:       .ascii  "0123456789ABCDEF"
s_ok:         .asciz  "OK"
s_fail:       .asciz  "FAIL"
s_info:       .asciz  "--"
s_noratio:    .asciz  "x-"
