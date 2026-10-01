;;; SNOS link fast paths: the byte loops the receive path cannot afford.
;;;
;;; WHY THIS FILE EXISTS. net_service() runs inside NMI, and it was measured at
;;; a mean of 144.8 scanlines — 55% of every frame, ~32,900 CPU cycles — whenever
;;; an application owns the PPU. It is not doing anything clever: it is moving
;;; ~320 bytes one at a time through far pointers at -O0, at ~103 cycles a byte.
;;; That is why an application on this machine has no usable vblank (the long
;;; comment in apps/vnc/vnc.c) and why LINK_FRAME could not be raised to 256
;;; without the handler overrunning its own frame (docs/performance.md).
;;;
;;; Three routines cover almost all of it:
;;;
;;;   link_mb_move   the frame copies in and out of the emulated mailbox, as an
;;;                  MVN block move — ~7 cycles/byte instead of ~103. The serial
;;;                  backend has one too now: link_serial_recv hands each received
;;;                  frame over with it, where its byte loop cost 34k master
;;;                  cycles a poll inside NMI (docs/link-throughput.md).
;;;   net_checksum   the 8-bit frame checksum, twice per poll, ~8 cycles/byte.
;;;                  This half helps BOTH backends.
;;;   ring_move      the two receive-ring copies in kernel/net.c — ring_put on
;;;                  the NMI side and net_read on the foreground one. Both were
;;;                  measured at ~230 cycles a byte, which is worse than the
;;;                  mailbox loop rather than better: indexing ring[ch][head]
;;;                  recomputes a 24-bit address from scratch for every single
;;;                  byte. This one helps BOTH backends too, because the ring is
;;;                  above the wire.
;;;
;;; ------------------------------------------------- the C calling convention
;;;
;;; Read out of a compiler listing, not guessed, and re-derivable the same way:
;;; write the prototype, compile a call at -O0, look at build/<file>.lst.
;;;
;;; THE RULE, which this comment used to state as "the LAST scalar goes in A":
;;; **A takes the FIRST argument that fits in sixteen bits.** Everything else is
;;; laid out in _Dp in declaration order, four bytes for a far pointer and two
;;; for a scalar, with the A-passed one skipped. The two spellings agree for all
;;; three routines here — each takes its pointers first and its count last — so
;;; the old wording was never wrong about them, and would have been wrong about
;;; the first routine written with a scalar in front. net_read's own listing is
;;; the proof: `net_read(int ch, uint8_t __far *dst, int max)` passes CH in A,
;;; dst in _Dp/_Dp+2 and max in _Dp+4.
;;;
;;;   first far pointer   _Dp   (offset) and _Dp+2 (bank)
;;;   second far pointer  _Dp+4 (offset) and _Dp+6 (bank)
;;;   the count           A, 16 bits, for all three routines below
;;;   return value        A; the caller does its own `sep #0x20` afterwards, so
;;;                       these must hand back A/X/Y still 16 bits wide
;;;
;;; All three are entered in that same 16-bit state — it is Calypsi's normal one,
;;; and kernel/stack.s documents the same assumption — but none actually relies
;;; on it: each sets the widths it needs before reading anything, and each
;;; restores 16-bit A/X/Y before returning. None saves P, and none is a leaf that
;;; calls anything, so the flags it leaves are its own business.
;;;
;;; `_Dp` is ONE shared 20-byte block addressed relative to D, so nothing here
;;; may assume a slot survives a call — see docs/calypsi-interrupt-hazard.md.
;;; Reading the argument slots is safe (the caller has just written them and
;;; treats them as clobbered); the higher slots are the caller's and are left
;;; alone, which is why no routine here needs to save any of them. It matters:
;;; ring_put keeps `src` in _Dp+8 ACROSS its call to ring_move. That is also why
;;; NMI's own direct page ($1C00) needs no thought here: every _Dp reference
;;; below is D-relative, so it lands in whichever page the caller was using.
;;;
;;; ------------------------------------------------------------ MVN, exactly
;;;
;;; MVN takes X = source offset, Y = destination offset, A = count-1, all three
;;; 16 bits, and it LEAVES DB SET TO THE DESTINATION BANK. Both routines that use
;;; it bracket themselves with phb/plb: without that, the C code that
;;; runs next reads its near data ($1D00-$1FFF in bank $00) out of bank $7E and
;;; the failure is a handful of wrong bytes now and then, not a crash.
;;;
;;; TWO SPELLING TRAPS, both verified against emitted bytes:
;;;   * Calypsi wants `mvn #dst,#src` — DESTINATION FIRST, i.e. object-code
;;;     order, the reverse of the WDC mnemonic. `mvn #0x11,#0x22` assembles to
;;;     54 11 22. link_mb_move has both banks equal so it could not bite there;
;;;     ring_move's $7F-destination path is the first place in this file where
;;;     they differ, and getting it backwards would read the ring out of the
;;;     application's arena.
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
;;; pointers — there is a matching note in linker.scm. ring_move relies on the
;;; same invariant for its SOURCE and would need the same edit.

              .rtmodel version, "1"
              .rtmodel core, "*"

              .extern _Dp

              .section farcode, root
              .public link_mb_move, net_checksum, ring_move, ip_sum, far_copy

;;; The one bank kernel far data and the link mailbox share; see above.
WRAM_BANK     .equ    0x7e

;;; The application arena, apps/app.scm.in: every app links its far data to
;;; $7F0000-$7FFFFF, so a buffer an app hands to net_read is in THIS bank while
;;; the ring it is read out of is in $7E. It is the only other bank ring_move
;;; below can move into, and net.c checks that before calling.
APP_BANK      .equ    0x7f

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
;;; void ring_move(uint8_t __far *dst, const uint8_t *src, uint16_t n)
;;;
;;; Copy n bytes (n >= 1, same MVN reason as above) OUT OF bank $7E, into either
;;; bank $7E or bank $7F, as one MVN. kernel/net.c calls it twice when a span
;;; wraps the end of a receive ring.
;;;
;;; THE SOURCE BANK IS NOT TAKEN FROM `src`. It is $7E by the same linker
;;; invariant link_mb_move relies on: `ring` and `rxframe` are `zfar` objects and
;;; calypsi/linker.scm gives kernel far data one region inside $7E. Passing a
;;; pointer whose bank byte is ignored is the trap here — it is spelled as a
;;; pointer only because that is what the caller has.
;;;
;;; THE DESTINATION BANK IS. MVN's bank bytes are instruction operands, so a
;;; runtime bank cannot be substituted; the two banks that can actually occur are
;;; branched to instead. net_read's destination is an APPLICATION buffer, which
;;; app.scm.in puts in $7F — except when the caller is the kernel itself
;;; (shell.c), where it is $7E. An app that passed a stack pointer would be in
;;; bank $00, so net.c tests the bank and keeps its byte loop for anything else:
;;; the alternative is writing the ring over the kernel's own far data.
;;; ---------------------------------------------------------------------------
ring_move:
              rep     #0x30           ; 16-bit count AND 16-bit indices
              phb                     ; MVN is about to overwrite DB
              pha                     ; the count, out of A's way — the bank test
                                      ; below needs the accumulator
              ldx     dp:.tiny (_Dp+4) ; source offset      (second argument)
              ldy     dp:.tiny _Dp     ; destination offset (first argument)
              sep     #0x20
              lda     dp:.tiny (_Dp+2) ; destination bank, the far pointer's
                                      ; third byte
              cmp     #APP_BANK
              rep     #0x30           ; REP clears only the M and X bits, so the
                                      ; comparison's Z survives it
              beq     rm_app
              pla                     ; A = n; PLA's own flags no longer matter
              dec     a               ; MVN moves A+1 bytes
              mvn     #WRAM_BANK,#WRAM_BANK
              plb
              rtl
rm_app:
              pla
              dec     a
              mvn     #APP_BANK,#WRAM_BANK    ; destination first — see above
              plb
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

;;; ---------------------------------------------------------------------------
;;; uint16_t ip_sum(const uint8_t *p, uint16_t n)
;;;
;;; The Internet ones-complement sum of p[0..n-1], NOT complemented, so callers
;;; can chain ranges and fold in a TCP/UDP pseudo-header before the final ~
;;; (RFC 1071: the sum is associative and commutative). n MUST BE EVEN — an odd
;;; trailing byte is the C caller's job (kernel/ip.c csum_odd), because handling
;;; it here costs a flag juggle on every call to serve the rare case.
;;;
;;; The loop adds LITTLE-ENDIAN words with the carry deliberately chained from
;;; one ADC into the next — that IS the end-around carry, deferred — and swaps
;;; the result once at the end (RFC 1071 §2(B): the sum is byte-order
;;; independent, so summing swapped words gives the byte-swap of the true sum).
;;; That makes the inner loop one 16-bit ADC per word, ~9 cycles per pair,
;;; against ~200+ for the same arithmetic in C at -O0 — and uIP checksums every
;;; TCP segment twice (verify in, compute out), so this is the stack's floor.
;;;
;;; Any bank: the pointer's own bank byte goes into DB, same as net_checksum.
;;; ---------------------------------------------------------------------------
ip_sum:
              rep     #0x30           ; A/X/Y 16 bits: A arrives holding n
              phb
              pha                     ; n, while the bank goes into DB
              sep     #0x20
              lda     dp:.tiny (_Dp+2) ; the buffer's bank ...
              pha
              plb                     ; ... into DB, for absolute,Y below
              rep     #0x20
              ldy     dp:.tiny _Dp    ; Y = the buffer's offset
              pla                     ; A = n
              lsr     a               ; A = word count (n even by contract)
              tax                     ; X = words; TAX sets Z, leaves C alone
              beq     ipsum_zero
              lda     ##0
              clc
ipsum_loop:
              adc     abs:0,y         ; carry chains into the NEXT add: that is
                                      ; the ones-complement end-around, deferred
              iny                     ; INY/DEX/BNE leave C untouched
              iny
              dex
              bne     ipsum_loop
              adc     ##0             ; fold the final carry ...
              adc     ##0             ; ... twice: 0xFFFF + C refolds once more
              xba                     ; back to network byte order (RFC 1071)
              plb
              rtl
ipsum_zero:
              lda     ##0
              plb
              rtl

;;; ---------------------------------------------------------------------------
;;; void far_copy(uint8_t *dst, const uint8_t __far *src, uint16_t n)
;;;
;;; n bytes, either side in any bank, n may be 0. For the copy MVN cannot do:
;;; its banks are immediates, and SYS_NET_WRITE takes its bytes from whichever
;;; bank an application keeps its buffer in. [dp],y long-indirect on both
;;; sides is ~22 cycles a byte; the C loop it replaces in traps.c was ~840
;;; master cycles a byte, 107k for every 128-byte write
;;; (docs/link-throughput.md).
;;; ---------------------------------------------------------------------------
far_copy:
              rep     #0x30
              tax                     ; X = n, the countdown
              beq     fc_done
              ldy     ##0
              sep     #0x20           ; byte moves
fc_loop:      lda     [.tiny (_Dp+4)],y
              sta     [.tiny _Dp],y
              iny
              dex
              bne     fc_loop
              rep     #0x20           ; A/X/Y back to 16 bits for the caller
fc_done:      rtl

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
