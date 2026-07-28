;;; SNOS application startup.
;;;
;;; The kernel has already done everything a normal C runtime would: the stack
;;; points at the app's slot in bank $00, the direct page is set, and DB is $7F
;;; (the arena). There is nothing to relocate and nothing to copy, because the
;;; app is executing in place out of cartridge ROM.
;;;
;;; So this is the entire startup: call main, then turn its return value into a
;;; SYS_EXIT. Apps may also call sys_exit() directly from any depth.
              .rtmodel version, "1"
              .rtmodel core, "*"

              .section code, root
              .public app_start
              .extern main

app_start:
              rep     #0x30
              jsl     main              ; exit code returned in A
              cop     #0x00             ; SYS_EXIT — does not return
              ;; The kernel unwinds out of the COP above, so control never
              ;; reaches here. If it somehow does, stop rather than run off
              ;; into whatever follows in the bank.
hang:         bra     hang
