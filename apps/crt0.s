;;; SNOS application startup.
;;;
;;; The kernel has already done most of what a C runtime would: the stack points
;;; at the app's slot in bank $00, the direct page is set, and DB is $7F (the
;;; arena). There is nothing to relocate and nothing to copy, because the app is
;;; executing in place out of cartridge ROM.
;;;
;;; What the kernel canNOT do is zero the application's variables, and C
;;; promises that it will. Apps link with --no-auto-libraries — Calypsi's own
;;; startup drags in a `reset` section holding a 16-bit vector, which cannot
;;; represent an address in an app's bank — so nothing else is going to do it.
;;;
;;; Skipping it does not fail loudly. Every static keeps whatever the previous
;;; application left in the arena, so a program works or does not depending on
;;; what ran before it. The editor's `quit_flag` came up non-zero and it exited
;;; the instant it started, having drawn one frame.
;;;
;;; Zero ONLY THIS APP'S ARENA SLOT (M10 step 4/5). The original zeroed all of
;;; bank $7F, which was correct while one app ran at a time and is sabotage the
;;; moment it is not: a background spawn would wipe the foreground app's data.
;;; The bounds are substituted by buildapp.sh from the same APP_ARENA_*
;;; declaration that placed the data — one source of truth.
;;; @AZLO@ = slot start (16-bit); @AZEND@ = (slot end + 1) & $FFFF, so the
;;; full-arena case terminates on the wrap to $0000.

              .rtmodel version, "1"
              .rtmodel core, "*"

              .section code, root
              .public app_start
              .extern main

ARENA_ZLO     .equ    0x@AZLO@
ARENA_ZEND    .equ    0x@AZEND@

app_start:
              rep     #0x30

              ;; DB is already $7F (exec.s sets it before jumping here), so
              ;; absolute addressing lands in the arena.
              ldx     ##ARENA_ZLO
zero_loop:    stz     abs:0x0000,x
              inx
              inx
              cpx     ##ARENA_ZEND
              bne     zero_loop

              jsl     main              ; exit code returned in A
              cop     #0x00             ; SYS_EXIT — does not return
              ;; The kernel unwinds out of the COP above, so control never
              ;; reaches here. If it somehow does, stop rather than run off
              ;; into whatever follows in the bank.
hang:         bra     hang
