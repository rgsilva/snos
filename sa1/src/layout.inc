;;; Fixed RAM layout. Nothing here is linker-allocated: both CPUs and the Lua
;;; test reach these addresses directly.
;;;
;;; I-RAM is $3000-$37FF on the S-CPU and ALSO $0000-$07FF on the SA-1, so an
;;; abs address below $0800 means I-RAM to the SA-1 and WRAM to the S-CPU.
;;; The benchmark routines rely on that: the same bytes run on either CPU.

IRAM          .equ    0x3000          ; S-CPU view of I-RAM offset 0

;;; ------------------------------------------- I-RAM, offsets (SA-1 view)
SA1_STACK     .equ    0x01ff          ; $0100-$01FF; $0000-$00FF is the SA-1's DP
IM_STOP       .equ    0x0200          ; nonzero: benchmark loop exits
IM_CMD        .equ    0x0204          ; command for the SA-1; it writes 0 when done
IM_ARG0       .equ    0x0206
IM_ARG1       .equ    0x0208
IM_ARG2       .equ    0x020a
IM_ARG3       .equ    0x020c
IM_SIG        .equ    0x0210          ; SA-1 writes SIG_LO, SIG_HI after reset
IM_IRQN       .equ    0x0214          ; SA-1 IRQ handler: count, last CFR message
IM_IRQM       .equ    0x0216
IM_NMIN       .equ    0x0218          ; SA-1 NMI handler: count, last CFR message
IM_NMIM       .equ    0x021a
IM_RES        .equ    0x0220          ; command results, up to $02FF
B_IDP         .equ    0x0300          ; benchmark DP, I-RAM config (WRAM on S-CPU)
B_ISRC        .equ    0x0400          ; benchmark copy source / dest, 256 B each
B_IDST        .equ    0x0500
CC_BUF        .equ    0x0600          ; char conversion buffer, 2 x 64 B

SIG_LO        .equ    0x5a1a
SIG_HI        .equ    0xa5e1

;;; SA-1 commands (IM_CMD)
CMD_BENCH     .equ    1               ; ARG0 bench, ARG1 D, ARG2 src, ARG3 dst
CMD_ARITH     .equ    2               ; run arith_tbl into IM_RES
CMD_SIGNAL    .equ    3               ; ARG1 -> SIV, then ARG0 -> SCNT
CMD_BWMAP     .equ    4               ; ARG0 -> BMAP, word at $6010 -> IM_RES
CMD_CBWE      .equ    5               ; ARG0 -> CBWE
CMD_DCNT      .equ    6               ; ARG1 (if nonzero) -> CDMA, ARG0 -> DCNT
CMD_ROMREAD   .equ    7               ; markers seen by the SA-1 -> IM_RES
CMD_LAST      .equ    7

;;; Benchmark direct page, relative to D (I-RAM, BW-RAM or WRAM).
B_CNT         .equ    0x00            ; 32-bit completed iterations
B_SRC         .equ    0x04
B_DST         .equ    0x06
B_M1          .equ    0x08
B_M2          .equ    0x0a
B_PROD        .equ    0x0c            ; 32-bit
B_T1          .equ    0x10            ; 32-bit
B_BDP         .equ    0x6000          ; BW-RAM config: D, then copy buffers
B_BSRC        .equ    0x6100
B_BDST        .equ    0x6200
SA1_BMAP      .equ    2               ; SA-1 window = BW-RAM $4000-$5FFF, idle value

;;; ------------------------------------------------------- S-CPU WRAM
;;; $0000-$00FF direct page (V_*), $0200 S-CPU benchmark stop flag (IM_STOP
;;; again, by construction), $0300-$05FF benchmark DP and buffers, $0800 line
;;; buffer, $0a00 results, $1000 the WRAM copy of wait_stop, stack below $1FFF.
V_T0          .equ    0x00            ; scratch, 8 words
V_T1          .equ    0x02
V_T2          .equ    0x04
V_T3          .equ    0x06
V_T4          .equ    0x08
V_T5          .equ    0x0a
V_PTR         .equ    0x0c            ; 24-bit pointer for [dp],y
V_COL         .equ    0x10            ; line buffer column
V_PAGE        .equ    0x12            ; page being written (0-2)
V_SHOWN       .equ    0x14            ; page on screen
V_NOK         .equ    0x16
V_NFAIL       .equ    0x18
V_SA1OK       .equ    0x1a            ; SA-1 answered at start
V_NUM         .equ    0x20            ; 32-bit operands for udiv32 / ln_dec
V_DEN         .equ    0x24
V_REM         .equ    0x28
V_FRAMES      .equ    0x2c            ; frames seen by the bounded waits
V_WPTR        .equ    0x2e            ; word watched by wait_word
V_WVAL        .equ    0x30
V_WMAX        .equ    0x32
V_IRQN        .equ    0x34            ; SA-1 -> S-CPU IRQs taken via $FFEE
V_IRQM        .equ    0x36
V_IRQALT      .equ    0x38            ; ... and via the SIV override
V_TICKS       .equ    0x3a            ; V-timer IRQs
V_LEFT        .equ    0x3c            ; frames left in an S-CPU benchmark
V_ROWS        .equ    0x40            ; rows used per page, 3 words
V_PAD         .equ    0x48            ; joypad last frame
V_FLEN        .equ    0x4a            ; length of the fail list
V_T6          .equ    0x4c
V_T7          .equ    0x4e
V_MBPOS       .equ    0x50            ; mailbox ring position (windex mod size)
V_LNMODE      .equ    0x52            ; ln_end: bit0 no transcript, bit1 no screen
V_WMODE       .equ    0x54            ; wait_eq / wait_ne private state
V_WPREV       .equ    0x56
V_L0          .equ    0x58            ; text.s private scratch: V_T* stay the caller's
V_L1          .equ    0x5a
V_L2          .equ    0x5c
V_L3          .equ    0x5e
V_L4          .equ    0x60
V_FAST        .equ    0x62            ; run_s from bank $80 in FastROM

LINEBUF       .equ    0x0800          ; 32 chars
FAILLIST      .equ    0x0840          ; names of failed checks, 64 chars
OFSTR         .equ    0x0880          ; sum overflow flags as '0'/'1', 16 chars
SAVEBUF       .equ    0x0c00          ; BW-RAM probe: 128 saved words
RESULTS       .equ    0x0a00          ; speed counts: [bench*5 + config] x 4 B
WAIT_WRAM     .equ    0x1000          ; wait_stop copied here for the idle runs
S_STACK       .equ    0x1fff

SHADOW        .equ    0x7e2000        ; 3 pages x 32 x 32 words
SHADOW_PAGE   .equ    0x800
CCREAD        .equ    0x7e4000        ; char conversion read-back, 2 KB + 1 KB
SINK          .equ    0x7f0000        ; 16 KB DMA sink for the rate test

;;; The test mailbox, same layout as snes/kernel/mailbox.h.
MBX           .equ    0x7ef000
MBX_MAGIC     .equ    0x7ef000
MBX_STATUS    .equ    0x7ef002
MBX_WINDEX    .equ    0x7ef004
MBX_RING      .equ    0x7ef010
MBX_RINGSIZE  .equ    3552

;;; ----------------------------------------------------------- BW-RAM
;;; Offsets into BW-RAM (S-CPU $40:0000 + n).
BW_BOOT       .equ    0x1ff0          ; "SA1P" + boot count, survives the probe
BW_IMG8       .equ    0x2000          ; 64x32 chunky 8bpp, 2 KB
BW_IMG4       .equ    0x3000          ; 64x32 packed 4bpp, 1 KB
BW_RATE       .equ    0x8000          ; 128x128 8bpp, 16 KB, contents don't matter

;;; ------------------------------------------------------------- VRAM
;;; Mode 3: BG1 8bpp picture, BG2 4bpp text.
VR_TEXTMAP    .equ    0x0000          ; BG2 map, words
VR_PICMAP     .equ    0x0400          ; BG1 map
VR_FONT       .equ    0x1000          ; BG2 chars, 4bpp, char code = tile
VR_PIC        .equ    0x4000          ; BG1 chars, 8bpp; tile 0 blank
PIC_COL       .equ    24              ; picture: 8 x 4 tiles, bottom right of
PIC_ROW       .equ    22              ; page 2 only (BG1 is off on the others)
TOP_ROW       .equ    1               ; first text row: row 0 is lost to overscan

;;; Entries in check_rom's tables (checks.s).
ROM_PROBES    .equ    10
ROM_PROBES_SA1 .equ   5

;;; How long each benchmark runs.
BENCH_FRAMES  .equ    16
