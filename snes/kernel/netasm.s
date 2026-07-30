;;; SNOS link fast paths: the two byte loops the NMI handler cannot afford.
;;;
;;; WHY THIS FILE EXISTS. net_service() runs inside NMI, and it was measured at
;;; a mean of 144.8 scanlines — 55% of every frame, ~32,900 CPU cycles — whenever
;;; an application owns the PPU. It is not doing anything clever: it is moving
;;; ~320 bytes one at a time through far pointers at -O0, at ~103 cycles a byte.
;;; That is why an application on this machine has no usable vblank (the long
;;; comment in apps/vnc/vnc.c) and why LINK_FRAME could not be raised to 256
;;; without the handler overrunning its own frame (docs/performance.md).
;;;
;;; Two routines cover almost all of it:
;;;
;;;   link_mb_move   the frame copies in and out of the emulated mailbox, as an
;;;                  MVN block move — ~7 cycles/byte instead of ~103. Only the
;;;                  EMULATED backend has these copies; link_serial.c bit-bangs
;;;                  straight out of the frame, so this half buys nothing on real
;;;                  hardware. Faster testing is the point (PLAN.md).
;;;   net_checksum   the 8-bit frame checksum, twice per poll, ~8 cycles/byte.
;;;                  This half helps BOTH backends.
;;;
;;; ------------------------------------------------- the C calling convention
;;;
;;; Read out of a compiler listing, not guessed, and re-derivable the same way:
;;; write the prototype, compile a call at -O0, look at build/<file>.lst.
;;;
;;;   first far pointer   _Dp   (offset) and _Dp+2 (bank)
;;;   second far pointer  _Dp+4 (offset) and _Dp+6 (bank)
;;;   last scalar         A, 16 bits
;;;   return value        A; the caller does its own `sep #0x20` afterwards, so
;;;                       these must hand back A/X/Y still 16 bits wide
;;;
;;; Both are entered in that same 16-bit state — it is Calypsi's normal one, and
;;; kernel/stack.s documents the same assumption — but neither actually relies on
;;; it: each sets the widths it needs before reading anything, and each restores
;;; 16-bit A/X/Y before returning. Neither saves P, and neither is a leaf that
;;; calls anything, so the flags it leaves are its own business.
;;;
;;; `_Dp` is ONE shared 20-byte block addressed relative to D, so nothing here
;;; may assume a slot survives a call — see docs/calypsi-interrupt-hazard.md.
;;; Reading the argument slots is safe (the caller has just written them and
;;; treats them as clobbered); the higher slots are the caller's and are left
;;; alone, which is why neither routine needs to save any of them. That is also
;;; why NMI's own direct page ($1C00) needs no thought here: every _Dp reference
;;; below is D-relative, so it lands in whichever page the caller was using.
;;;
;;; ------------------------------------------------------------ MVN, exactly
;;;
;;; MVN takes X = source offset, Y = destination offset, A = count-1, all three
;;; 16 bits, and it LEAVES DB SET TO THE DESTINATION BANK. Both routines below
;;; therefore bracket themselves with phb/plb: without that, the C code that
;;; runs next reads its near data ($1D00-$1FFF in bank $00) out of bank $7E and
;;; the failure is a handful of wrong bytes now and then, not a crash.
;;;
;;; TWO SPELLING TRAPS, both verified against emitted bytes:
;;;   * Calypsi wants `mvn #dst,#src` — DESTINATION FIRST, i.e. object-code
;;;     order, the reverse of the WDC mnemonic. `mvn #0x11,#0x22` assembles to
;;;     54 11 22. Both banks are $7E here so it cannot bite today; it would bite
;;;     the moment they differ.
;;;   * `adc 0,y` is direct-page indexed and silently wrong under a relocated D.
;;;     `adc abs:0,y` is the absolute,Y this file means. See
;;;     docs/calypsi-asm-syntax.md — the assembler tracks no register widths
;;;     either, so every operand below states its own.
;;;
;;; ------------------------------------------------------- the bank the move
;;;                                                         assumes
;;; MVN's bank operands are immediates, so link_mb_move can only move WITHIN one
;;; bank, and that bank is hard-coded here as $7E. That is a linker invariant,
;;; not a hope: calypsi/linker.scm gives the kernel's far data a single HiRAM
;;; region, $7E2000-$7EDFFF, and the mailbox is fixed at $7EE000 by link.h. Every
;;; `zfar` object is therefore in bank $7E by construction, and overflowing the
;;; region is a link error rather than a silent move to the wrong bank. If HiRAM
;;; is ever made to span banks, this routine has to take its banks from the
;;; pointers — there is a matching note in linker.scm.

              .rtmodel version, "1"
              .rtmodel core, "*"

              .extern _Dp

              .section farcode, root
              .public link_mb_move, net_checksum

;;; The one bank kernel far data and the link mailbox share; see above.
WRAM_BANK     .equ    0x7e

;;; link.h's LINK_SUM: the header byte that holds the checksum, and so the one
;;; byte the checksum does not cover. Part of the wire format, which PLAN.md
;;; calls the contract — it does not move.
LINK_SUM      .equ    3

;;; ---------------------------------------------------------------------------
;;; void link_mb_move(void *dst, const void *src, uint16_t n)
;;;
;;; Copy n bytes within bank $7E. n must be at least 1 — MVN moves A+1 bytes, so
;;; a count of zero would move 65536.
;;; ---------------------------------------------------------------------------
link_mb_move:
              rep     #0x30           ; MVN needs a 16-bit count AND 16-bit
                                      ; indices; x=1 would use the low byte of
                                      ; X/Y and zero the high one
              phb                     ; DB is the caller's near-data bank and
                                      ; MVN is about to overwrite it
              dec     a               ; A = n-1
              ldx     dp:.tiny (_Dp+4) ; source offset      (second argument)
              ldy     dp:.tiny _Dp     ; destination offset (first argument)
              mvn     #WRAM_BANK,#WRAM_BANK
              plb                     ; near data resolves in bank $00 again
              rtl

;;; ---------------------------------------------------------------------------
;;; uint8_t net_checksum(const uint8_t *f, uint16_t n)
;;;
;;; The 8-bit sum of f[0..n-1] excluding f[LINK_SUM], which is what kernel/net.c
;;; puts in (and checks against) the frame header. n must be a non-zero multiple
;;; of 16: the sum is unrolled sixteen bytes at a time, so that raising
;;; LINK_FRAME to 256 needs no change here.
;;;
;;; Unlike the move this works in any bank — it takes the frame's bank from the
;;; pointer and puts it in DB, so the inner loop is absolute,Y and never pays for
;;; indirect addressing.
;;; ---------------------------------------------------------------------------

;;; One byte into the running sum. The `clc` per byte is not laziness: without it
;;; the carry out of the previous ADC adds itself back in, and this is a plain
;;; 8-bit sum, not a 9-bit one. 6 cycles a byte.
sumbyte       .macro  off
              clc
              adc     abs:\off,y
              .endm

sum16         .macro  base
              sumbyte \base+0
              sumbyte \base+1
              sumbyte \base+2
              sumbyte \base+3
              sumbyte \base+4
              sumbyte \base+5
              sumbyte \base+6
              sumbyte \base+7
              sumbyte \base+8
              sumbyte \base+9
              sumbyte \base+10
              sumbyte \base+11
              sumbyte \base+12
              sumbyte \base+13
              sumbyte \base+14
              sumbyte \base+15
              .endm

;;; Sixteen INYs rather than arithmetic through A, because A is the running sum.
step16        .macro
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              iny
              .endm

net_checksum:
              rep     #0x30           ; A/X/Y 16 bits: A arrives holding n
              lsr     a
              lsr     a
              lsr     a
              lsr     a
              tax                     ; X = n/16 blocks left to sum
              phb                     ; DB is the caller's near-data bank
              sep     #0x20           ; 8-bit A from here: this is a byte sum
              lda     dp:.tiny (_Dp+2) ; the frame's bank ...
              pha
              plb                     ; ... into DB, for absolute,Y below
              ldy     dp:.tiny _Dp    ; Y = the frame's offset within that bank
              lda     #0
ck_block:
              sum16   0
              step16
              dex
              bne     ck_block
              ;; LINK_SUM was summed with the rest and comes back out here. The C
              ;; version tested for it once per byte; subtracting it once is the
              ;; same value modulo 256, and mod 256 is all the header holds.
              ldy     dp:.tiny _Dp
              sec
              sbc     abs:LINK_SUM,y
              rep     #0x20           ; hand A back 16 bits wide, as the caller
              and     ##0x00ff        ; expects, holding a clean uint8_t
              plb
              rtl
