;;; S-CPU, PPU and SA-1 register addresses (bank $00 / $80 view).
;;; SA-1 names and bits: fullsnes "SNES Cart SA-1 I/O Map"; see docs/sa1-notes.md.

;;; ------------------------------------------------------------------ PPU
INIDISP       .equ    0x2100
OBSEL         .equ    0x2101
BGMODE        .equ    0x2105
MOSAIC        .equ    0x2106
BG1SC         .equ    0x2107
BG2SC         .equ    0x2108
BG3SC         .equ    0x2109
BG4SC         .equ    0x210a
BG12NBA       .equ    0x210b
BG34NBA       .equ    0x210c
BG1HOFS       .equ    0x210d
BG1VOFS       .equ    0x210e
BG2HOFS       .equ    0x210f
BG2VOFS       .equ    0x2110
VMAIN         .equ    0x2115
VMADDL        .equ    0x2116
VMDATAL       .equ    0x2118
VMDATAH       .equ    0x2119
CGADD         .equ    0x2121
CGDATA        .equ    0x2122
W12SEL        .equ    0x2123
TM            .equ    0x212c
TS            .equ    0x212d
TMW           .equ    0x212e
CGWSEL        .equ    0x2130
CGADSUB       .equ    0x2131
COLDATA       .equ    0x2132
SETINI        .equ    0x2133
SLHV          .equ    0x2137
OPVCT         .equ    0x213d
STAT78        .equ    0x213f
WMDATA        .equ    0x2180
WMADDL        .equ    0x2181
WMADDH        .equ    0x2183

;;; ---------------------------------------------------------------- S-CPU
NMITIMEN      .equ    0x4200
WRMPYA        .equ    0x4202
WRMPYB        .equ    0x4203
HTIMEL        .equ    0x4207
VTIMEL        .equ    0x4209
MDMAEN        .equ    0x420b
HDMAEN        .equ    0x420c
MEMSEL        .equ    0x420d
RDNMI         .equ    0x4210
TIMEUP        .equ    0x4211
HVBJOY        .equ    0x4212
RDMPYL        .equ    0x4216
JOY1L         .equ    0x4218
DMAP0         .equ    0x4300
BBAD0         .equ    0x4301
A1T0L         .equ    0x4302
A1B0          .equ    0x4304
DAS0L         .equ    0x4305

;;; ----------------------------------------------- SA-1, written by S-CPU
CCNT          .equ    0x2200          ; 7 IRQ to SA-1, 6 wait, 5 reset, 4 NMI, 0-3 msg
SIE           .equ    0x2201          ; 7 SA-1 IRQ enable, 5 char-conv IRQ enable
SIC           .equ    0x2202          ; 7 clear SA-1 IRQ, 5 clear char-conv IRQ
CRV           .equ    0x2203          ; SA-1 reset vector (16-bit, bank $00)
CNV           .equ    0x2205          ; SA-1 NMI vector
CIV           .equ    0x2207          ; SA-1 IRQ vector
CXB           .equ    0x2220          ; Super MMC: 7 also map LoROM, 0-2 megabyte
DXB           .equ    0x2221
EXB           .equ    0x2222
FXB           .equ    0x2223
BMAPS         .equ    0x2224          ; S-CPU $6000-$7FFF window: 8 KB block 0-31
SBWE          .equ    0x2226          ; 7 S-CPU BW-RAM write enable
BWPA          .equ    0x2228          ; protected area $40:0000 + 256 << n
SIWP          .equ    0x2229          ; S-CPU I-RAM write enable, bit n = $3n00

;;; ------------------------------------------------- SA-1, written by SA-1
SCNT          .equ    0x2209          ; 7 IRQ to S-CPU, 6 use SIV, 4 use SNV, 0-3 msg
CIE           .equ    0x220a          ; 7 S-CPU IRQ, 6 timer, 5 DMA, 4 S-CPU NMI
CIC           .equ    0x220b          ; same bits, 1 = clear
SNV           .equ    0x220c          ; S-CPU NMI vector override
SIV           .equ    0x220e          ; S-CPU IRQ vector override
BMAP          .equ    0x2225          ; SA-1 $6000-$7FFF window: 7 bitmap, 0-6 block
CBWE          .equ    0x2227          ; 7 SA-1 BW-RAM write enable
CIWP          .equ    0x222a          ; SA-1 I-RAM write enable, bit n = $0n00
DCNT          .equ    0x2230          ; 7 enable, 5 char conv, 4 type 1, 2 dst, 0-1 src
BRF           .equ    0x2240          ; type 2 pixels: 2240-2247 row n, 2248-224F row n+1
MCNT          .equ    0x2250          ; 0 mul, 1 div, 2 cumulative sum (resets sum)
MA            .equ    0x2251          ; signed multiplicand / dividend
MB            .equ    0x2253          ; signed multiplier / unsigned divisor, high starts

;;; ---------------------------------------------- SA-1, written by either
CDMA          .equ    0x2231          ; 7 end conv, 2-4 width log2 chars, 0-1 depth
SDA           .equ    0x2232          ; 24-bit DMA source
DDA           .equ    0x2235          ; DMA dest; writing $2236 starts a type 1 conv

;;; --------------------------------------------------------- SA-1, reads
SFR           .equ    0x2300          ; S-CPU: 7 SA-1 IRQ, 6 SIV, 5 conv IRQ, 4 SNV, 0-3 msg
CFR           .equ    0x2301          ; SA-1: 7 S-CPU IRQ, 6 timer, 5 DMA, 4 NMI, 0-3 msg
MR            .equ    0x2306          ; 40-bit arithmetic result
OF            .equ    0x230b          ; 7 cumulative-sum overflow
VC            .equ    0x230e          ; "version code"; open bus on known carts
