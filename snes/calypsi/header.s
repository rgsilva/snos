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
              .extern app_return

;;; Kernel exec state, mirrored from kernel/exec.s. An app terminates by
;;; setting EXITFLAG; both trap shims below check it and unwind to app_return
;;; rather than returning into an app that is no longer supposed to be running.
EXITFLAG      .equ    0x7e1a12
APPRUNNING    .equ    0x7e1a16
;; Scheduler handshake, owned by kernel/proc.c (M10). PROC_WANT is one-shot:
;; consumed below whether or not the deferral allows the switch.
PROC_WANT     .equ    0x7e1a18
PROC_NEXT_S   .equ    0x7e1a1a
PROC_OLD_S    .equ    0x7e1a1c
PROC_SWITCHED .equ    0x7e1a1e
;; Who the CPU runs AFTER the swap below. Published BY THE SHIM at the moment
;; it swaps, because anything later is a lie: with the update deferred to the
;; next proc_tick, a background process's entire first slice ran identified
;; as process 0 -- unmuted, unreapable, wrong.
PROC_NEXT_ID  .equ    0x7e1a20
PROC_CUR_ID   .equ    0x7e1a22

;;; Direct pages for trap handlers. NOT $0000, and this is load-bearing:
;;; Calypsi's C code keeps working values in direct-page pseudo-registers whose
;;; slots the linker hands out from the static call graph, which cannot contain
;;; an interrupt edge. Sharing a direct page with the interrupted code lets a
;;; handler quietly corrupt a pointer mid-computation — one wrong byte at a
;;; time, in whatever the foreground happened to be doing. Since the pseudo
;;; registers are direct-page relative, a separate D makes that impossible.
;;; COP and NMI get SEPARATE pages, not one shared "interrupt" page. NMI fires
;;; while a system call is running, so a single shared page just moves the
;;; collision from (foreground, NMI) to (COP, NMI) — which showed up as LOAD
;;; dropping one character of a saved BASIC program.
SYS_DP        .equ    0x1b00          ; COP and BRK (synchronous traps)
NMI_DP        .equ    0x1c00          ; NMI (fires during the above)

snos_stub:
              rti

;;; Where dead background processes go (M10 step 5). A process's SYS_EXIT
;;; cannot unwind anything — there is no caller to unwind TO — and it cannot
;;; park inside the COP handler either, because the scheduler's mid-syscall
;;; deferral would then never switch away again. So the COP handler rewrites
;;; the trap frame's return address to point here, the shim rti's into this
;;; loop with the process already marked DEAD, and the next NMI schedules
;;; away from it forever. In bank $00 because the frame's PB is one byte.
              .public proc_grave
proc_grave:   bra     proc_grave

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
              lda     ##NMI_DP
              tcd                           ; NMI gets a direct page of its own
              sep     #0x20
              lda     abs:0x4210            ; ack NMI (clears the flag)
              rep     #0x20
              jsl     snos_nmi_c
              ;; The kill switch fires from here: if the handler decided the
              ;; running app has to die, unwind now instead of returning into
              ;; it. This is what stops a wedged app from taking the machine
              ;; with it, and it needs no cooperation from the app at all.
              lda     long:APPRUNNING
              beq     nmi_ret
              lda     long:EXITFLAG
              beq     nmi_ret
              ;; ...and only from PROCESS 0's context. The foreground app IS
              ;; process 0's continuation; with a background job live, this
              ;; NMI may have interrupted THAT process, and unwinding from its
              ;; context would teleport the CPU onto the kernel stack with the
              ;; wrong identity published and the background frame lost.
              ;; proc_tick steers the rotation to process 0 while the flag is
              ;; armed, so deferring here costs at most a slice.
              lda     long:PROC_CUR_ID
              bne     nmi_ret
              ;; ...unless this NMI interrupted a system call. Unwinding from
              ;; the middle of a COP handler abandons whatever kernel mutation
              ;; it was inside — a half-queued control record misaligns the
              ;; gateway's parser until a link restart, a half-closed file
              ;; leaks its bytes. The interrupted D is at 2,s (the phd above);
              ;; if it is the syscall DP, let the COP shim do the unwind at
              ;; handler exit — it re-checks EXITFLAG on every return, and the
              ;; blocking loops in input.c give up when the flag is armed, so
              ;; deferring never parks the kill. The two one-instruction
              ;; windows in the COP shim where D is still the app's (before
              ;; tcd, after pld) unwind from here, harmlessly: no handler is
              ;; in progress at either.
              lda     2,s
              cmp     ##SYS_DP
              beq     nmi_ret
              jmp     long:app_return
nmi_ret:
              ;; The context switch (M10). Everything above decided whether a
              ;; process DIES; this decides whether a different one RESUMES.
              ;; The interrupted process's whole context is already on its
              ;; stack — the frame this path is about to pop — so the switch
              ;; is: publish old S, adopt new S, and let the SAME pops and
              ;; rti below resume whoever the new stack belongs to. A forged
              ;; frame (proc.c) makes first entry and resumption identical.
              lda     long:PROC_WANT
              beq     nmi_pop
              ;; Same rule as the kill switch, same reason: a process inside
              ;; a COP handler holds the kernel mid-mutation and the single
              ;; syscall DP. It finishes before anyone else runs. The request
              ;; STAYS ARMED on deferral — the syscall exit path (cop_ret)
              ;; takes it. Consuming it here was the step-5 pin: a process
              ;; whose life is a poll loop of syscalls is mid-syscall at
              ;; every NMI, so its deferrals starved everyone else forever.
              lda     2,s
              cmp     ##SYS_DP
              beq     nmi_pop
              lda     ##0
              sta     long:PROC_WANT
              tsc                           ; the entire context switch:
              sta     long:PROC_OLD_S       ;   save one register,
              lda     long:PROC_NEXT_S      ;   load one register
              tcs
              lda     long:PROC_NEXT_ID     ; identity switches WITH the stack,
              sta     long:PROC_CUR_ID      ; not a frame later
              lda     ##1
              sta     long:PROC_SWITCHED    ; tells proc_tick to file OLD_S
nmi_pop:
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
              lda     ##SYS_DP
              tcd
              tsc                           ; A = frame pointer for the handler:
                                            ;   sp+1 DB, +2 D, +4 Y, +6 X, +8 A,
                                            ;   +10 P, +11 PC, +13 PB
              jsl     snos_cop_c            ; syscall result returned in A
              sta     8,s                   ; -> the stacked A, pulled below
              ;; SYS_EXIT cannot simply return: the app's stack may be
              ;; arbitrarily deep by now, so unwind to the kernel instead.
              ;; APPRUNNING must gate this exactly as in the NMI shim: the
              ;; kernel itself issues COPs (debug syscheck), and unwinding one
              ;; of those through app_return would restore a dead KSP.
              lda     long:APPRUNNING
              beq     cop_ret
              lda     long:EXITFLAG
              beq     cop_ret
              lda     long:PROC_CUR_ID      ; only process 0 owns the fg app;
              bne     cop_ret               ; a bg syscall exit must not take
              jmp     long:app_return       ; another process's funeral
cop_ret:
              ;; A switch the NMI deferred while this syscall ran is taken
              ;; HERE, at the moment the mid-syscall reason evaporates. NMI is
              ;; masked around the test-and-swap so a fresh NMI cannot race
              ;; the handoff. The frame being abandoned mid-pop is a COP
              ;; frame, but both shims push the identical layout — so when
              ;; this process is later resumed by the standard pop path, it
              ;; completes THESE pops and rti's back into its own code with
              ;; the syscall's return value already in the stacked A.
              sep     #0x20
              lda     #0x01
              sta     abs:0x4200            ; NMI off, auto-joypad on
              rep     #0x20
              lda     long:PROC_WANT
              beq     cop_noswitch
              lda     ##0
              sta     long:PROC_WANT
              tsc
              sta     long:PROC_OLD_S
              lda     long:PROC_NEXT_S
              tcs
              lda     long:PROC_NEXT_ID
              sta     long:PROC_CUR_ID
              lda     ##1
              sta     long:PROC_SWITCHED
cop_noswitch:
              sep     #0x20
              lda     #0x81
              sta     abs:0x4200            ; NMI back on
              rep     #0x20
              plb
              pld
              ply
              plx
              pla
              rti

;;; void proc_switch_now(void) — offer the CPU from INSIDE a blocking
;;; syscall (kbd_idle's frame wait, and through it every blocking loop).
;;; Without this, a foreground process blocked in SYS_GETC at the shell
;;; prompt would starve every background process: the NMI always finds it
;;; mid-syscall and defers, and cop_ret only runs when a key arrives.
;;; Builds a resume frame in the SAME 13-byte shape the shims push, so the
;;; standard pop path resumes it at psn_resume; then adopts the pending
;;; process exactly as the shims do. NMI is masked across the whole dance.
              .public proc_switch_now
proc_switch_now:
              rep     #0x30
              sep     #0x20
              lda     #0x01
              sta     abs:0x4200
              rep     #0x20
              lda     long:PROC_WANT
              beq     psn_out
              lda     ##0
              sta     long:PROC_WANT
              ;; Forge "interrupted at psn_resume": PB, PC, P, then the five
              ;; register pushes, current values — we ARE the process, its
              ;; live D (the syscall DP) and DB are exactly what to restore.
              sep     #0x20
              lda     #0                    ; PB: this code is in bank $00
              pha
              rep     #0x20
              lda     ##psn_resume          ; PC of the resume point
              pha
              sep     #0x20
              lda     #0                    ; P image: 16-bit A/X on resume
              pha
              rep     #0x30
              pha
              phx
              phy
              phd
              phb
              tsc
              sta     long:PROC_OLD_S
              lda     long:PROC_NEXT_S
              tcs
              lda     long:PROC_NEXT_ID
              sta     long:PROC_CUR_ID
              lda     ##1
              sta     long:PROC_SWITCHED
              ;; Pop the ADOPTED process's frame — identical shape, same rti.
              sep     #0x20
              lda     #0x81
              sta     abs:0x4200
              rep     #0x20
              plb
              pld
              ply
              plx
              pla
              rti
psn_resume:   rep     #0x30                 ; resumed here by a later switch
psn_out:
              sep     #0x20
              lda     #0x81
              sta     abs:0x4200
              rep     #0x20
              rtl

;;; BRK — debugger trap only. Never used for syscalls, so the debugger can
;;; never collide with syscall signature space.
snos_brk:
              rep     #0x30
              pha                           ; same push order as snos_cop, so the
              phx                           ; panic dump can use one set of frame
              phy                           ; offsets
              phd
              phb
              phk
              plb
              lda     ##SYS_DP
              tcd
              tsc
              jsl     snos_brk_c            ; panics; does not return
brk_hang:     bra     brk_hang
