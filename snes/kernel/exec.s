;;; SNOS process switch: enter an application, and get control back.
;;;
;;; This is the whole of "exec" on a SNES, and it is short because cart ROM is
;;; memory-mapped and already executable. There is no loader, no relocation and
;;; no copying: an application is simply code sitting in a ROM bank, and running
;;; it means pointing the stack, direct page and data bank somewhere sane and
;;; jumping. PLAN.md §0 — this is the property the entire design rests on.
;;;
;;; Getting back is the interesting half. An app terminates in one of two ways
;;; and neither can `RTL`, because by then the app's stack is arbitrarily deep:
;;;
;;;   1. it calls COP #0 (SYS_EXIT), possibly from inside nested calls;
;;;   2. the user holds START+SELECT and the NMI handler kills it, which can
;;;      happen at literally any instruction — including inside a loop that
;;;      never makes a system call at all. That is the whole point of the kill
;;;      switch: an app must not be able to lock the machine.
;;;
;;; Both cases converge on app_return, which throws the app's stack away and
;;; restores the kernel's. The trap shims in calypsi/header.s check the exit
;;; flag after their C handler returns and jump here instead of RTI-ing back
;;; into an app that should no longer be running.

              .rtmodel version, "1"
              .rtmodel core, "*"

;;; Kernel exec state. Fixed addresses inside the block reserved by
;;; linker.scm at $1A00-$1A3F, reached through the $7E WRAM mirror so they work
;;; from any data bank. $1A00-$1A0F is the syscall parameter block (ABI,
;;; snos_abi.h); $1A10 upward is kernel-private.
ARGS_P2       .equ    0x7e1a04        ; app bank, set by the caller
ARGS_P3       .equ    0x7e1a06        ; app entry offset
KSP           .equ    0x7e1a10        ; kernel stack pointer across the switch
EXITFLAG      .equ    0x7e1a12        ; set => unwind to app_return
EXITCODE      .equ    0x7e1a14
APPRUNNING    .equ    0x7e1a16

APP_STACK_TOP .equ    0x18ff          ; PLAN.md §2 — the app's 3328-byte slot
APP_DP        .equ    0x1900
ARENA_BANK    .equ    0x7f            ; WRAM bank $7F: every app's data lives here

              .section code, root
              .public app_exec, app_return
              .public snos_exec_state

;;; uint16_t app_exec(void)
;;;   in : ARGS_P2 = bank, ARGS_P3 = entry offset
;;;   out: the app's exit code
app_exec:
              rep     #0x30
              php                           ; kernel context, restored by
              phd                           ; app_return
              phb
              tsc
              sta     long:KSP
              lda     ##0
              sta     long:EXITFLAG         ; (STZ has no long addressing mode)
              lda     ##1
              sta     long:APPRUNNING

              ;; --- from here on we are on the application's stack ---
              lda     ##APP_STACK_TOP
              tcs
              lda     ##APP_DP
              tcd

              ;; Far jump by pushing a return address: push the target bank,
              ;; then entry-1, and RTL. Avoids needing an indirect long jump
              ;; through a pointer we would first have to find somewhere to put.
              sep     #0x20
              lda     long:ARGS_P2
              pha                           ; target bank
              rep     #0x20
              lda     long:ARGS_P3
              dec     a
              pha                           ; target offset - 1

              ;; DB = $7F so the app's near data resolves into the arena. Its
              ;; ROM strings are far pointers, so they do not care about DB.
              sep     #0x20
              lda     #ARENA_BANK
              pha
              plb
              rep     #0x30
              rtl                           ; -> app entry

;;; Re-entered from a trap shim once EXITFLAG is set. The app's stack is
;;; abandoned wholesale, which is exactly why this cannot be an RTL from
;;; wherever the app happened to be.
app_return:
              rep     #0x30
              lda     ##0
              sta     long:APPRUNNING
              lda     long:KSP
              tcs
              plb
              pld
              plp
              lda     long:EXITCODE
              rtl                           ; -> caller of app_exec

;;; Address of the exec-state block, so C can read APPRUNNING etc. without
;;; hard-coding the constants a second time.
              .section code, root
snos_exec_state:
              .word   0x1a10
