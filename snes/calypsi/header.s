;;; SNOS cartridge header + CPU vectors.
;;;
;;; HiROM ($21) + FastROM => map mode $31. MEMSEL ($420D bit 0) is set by
;;; kernel_init(); reset always begins in slow mode with PB=$00, so the first
;;; instructions run out of the $00:8000 mirror regardless.
;;;
;;; ROM size byte ($FFD7) is PATCHED BY tools/raw2sfc.py to match the actual
;;; padded image size — do not hand-maintain it here, a mismatch degrades
;;; Mesen's header-scoring and shows up as "cartridge not detected".
;;;
;;; Vector rules that bit people before (PLAN.md §3): COP and BRK force PB=$00,
;;; so their handlers must live in bank $00 — i.e. in the `code` section, which
;;; linker.scm scatters to $C08000 and the CPU sees mirrored at $00:8000.
;;; Handlers indirect through the RAM vector table so M3 apps can hook them
;;; without rewriting ROM.

              .rtmodel version, "1"
              .rtmodel core, "*"

;;; ---------------------------------------------------------------- header

;;; Extended header $FFB0-$FFBF (licensee byte $33 => present)
              .section snesheaderextended, root
              .ascii  "  "           ; maker code
              .ascii  "SNOS"         ; game code
              .byte   0,0,0,0,0,0,0  ; reserved
              .byte   0              ; expansion RAM size
              .byte   0              ; special version
              .byte   0              ; cartridge sub-type

;;; Header $FFC0-$FFDF
              .section snesheader, root
              .ascii  "SNOS                 "   ; 21-byte title
              .byte   0x31           ; map mode: HiROM + FastROM
              .byte   0x02           ; cartridge type: ROM+RAM+battery
              .byte   0x0c           ; ROM size (patched by raw2sfc.py)
              .byte   0x08           ; RAM size: 256 KB cart SRAM
              .byte   0x01           ; country: USA (NTSC)
              .byte   0x33           ; licensee: extended header present
              .byte   0x00           ; version
              .word   0xaaaa         ; checksum complement (patched)
              .word   0x5555         ; checksum (patched)

;;; ---------------------------------------------------------------- vectors

              .section snesvec1, root
              .word   0, 0                  ; $FFE0,$FFE2 unused
              .word   snos_cop              ; $FFE4 native COP   (syscalls)
              .word   snos_brk              ; $FFE6 native BRK   (debugger)
              .word   snos_stub             ; $FFE8 native ABORT
              .word   snos_nmi              ; $FFEA native NMI   (vblank)
              .word   0                     ; $FFEC unused
              .word   snos_stub             ; $FFEE native IRQ
              .word   0, 0                  ; $FFF0,$FFF2 unused
              .word   snos_stub             ; $FFF4 emulation COP
              .word   0                     ; $FFF6 unused
              .word   snos_stub             ; $FFF8 emulation ABORT
              .word   snos_stub             ; $FFFA emulation NMI

              .section snesvec2, root
              .word   snos_stub             ; $FFFE emulation IRQ/BRK

;;; ------------------------------------------------------------- handlers
;;; All of these live in `code`, which linker.scm places in the bank-$00
;;; mirror. That placement is load-bearing: a trap handler in `farcode` would
;;; be unreachable, because COP/BRK/NMI force PB=$00 before the vector fetch.

              .section code, root
              .public snos_stub, snos_nmi, snos_cop, snos_brk
              .extern snos_nmi_c, snos_cop_c, snos_brk_c

snos_stub:
              rti

;;; NMI — vblank, 60 Hz. The kernel owns this unconditionally (PLAN.md §5):
;;; it is what lets background work (M6 networking) run while an app is in
;;; control, and what makes the M3 kill switch impossible for an app to defeat.
;;; Runs on whatever stack is current, which while an app runs is the APP's
;;; stack — keep this shallow.
snos_nmi:
              rep     #0x30                 ; A/X/Y 16-bit
              pha
              phx
              phy
              phd
              phb
              phk                           ; DB = this bank, so C can reach
              plb                           ; near data
              lda     ##0x0000
              tcd                           ; DP = kernel direct page
              sep     #0x20
              lda     abs:0x4210                ; ack NMI (clears the flag)
              rep     #0x20
              jsl     snos_nmi_c
              plb
              pld
              ply
              plx
              pla
              rti

;;; COP — the syscall trap. Signature byte is at [return address - 1]; the
;;; 65816 pushes PC already advanced past it. See snos_cop_c.
;;;
;;; The return value is delivered by OVERWRITING THE STACKED A in place, not
;;; via a global: after the five pushes below, the saved A sits at 8,s / 9,s,
;;; and stack-relative addressing does not depend on DB. Writing it to a near
;;; global instead would read back through the *caller's* DB after `plb` and
;;; return garbage into whatever bank the app happens to be running in.
;;;   1,s = DB   2,s..3,s = D   4,s..5,s = Y   6,s..7,s = X   8,s..9,s = A
snos_cop:
              rep     #0x30
              pha
              phx
              phy
              phd
              phb
              phk
              plb
              lda     ##0x0000
              tcd
              jsl     snos_cop_c            ; syscall result returned in A
              sta     8,s                   ; -> the stacked A, pulled below
              plb
              pld
              ply
              plx
              pla
              rti

;;; BRK — debugger trap only. Never used for syscalls, so the debugger can
;;; never collide with syscall signature space.
snos_brk:
              rep     #0x30
              phd
              phb
              phk
              plb
              lda     ##0x0000
              tcd
              jsl     snos_brk_c            ; panics; does not return
brk_hang:     bra     brk_hang
