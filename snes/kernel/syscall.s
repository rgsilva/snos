;;; SNOS system-call thunks.
;;;
;;; One `COP #n; RTL` per call. These are assembly on purpose: this file is the
;;; single place where the register-level ABI is written down, and it stays
;;; correct regardless of what any C compiler decides to do internally.
;;;
;;; Argument 0 arrives in A (Calypsi's first-argument register, and the obvious
;;; choice for hand-written asm too) and is left there for the kernel to find in
;;; the trap frame. The return value comes back in A because the COP shim in
;;; calypsi/header.s writes the handler's result over the stacked A before
;;; pulling it.
;;;
;;; Everything else goes through the parameter block at $7E1A00 — see
;;; kernel/snos_abi.h for why not X/Y and why not Calypsi's own convention.
;;;
;;; Apps link this same file, which is the point: an app's syscall path is
;;; byte-identical to the kernel's.

              .rtmodel version, "1"
              .rtmodel core, "*"

              .section code, root
              .public sys_exit, sys_version, sys_yield
              .public sys_putc, sys_puts_raw, sys_getc, sys_cls
              .public sys_readline_raw, sys_putu, sys_puthex
              .public sys_frames, sys_waitvbl, sys_trace_raw, sys_pad
              .public sys_open, sys_getc_f, sys_putc_f, sys_close
              .public sys_fsize, sys_dir, sys_unlink

sys_exit:       cop     #0x00
                rtl
sys_version:    cop     #0x01
                rtl
sys_yield:      cop     #0x02
                rtl

sys_putc:       cop     #0x10
                rtl
sys_puts_raw:   cop     #0x11
                rtl
sys_getc:       cop     #0x12
                rtl
sys_cls:        cop     #0x13
                rtl
sys_readline_raw:
                cop     #0x14
                rtl
sys_putu:       cop     #0x15
                rtl
sys_puthex:     cop     #0x16
                rtl

sys_pad:        cop     #0x20
                rtl

sys_open:       cop     #0x30
                rtl
sys_getc_f:     cop     #0x31
                rtl
sys_putc_f:     cop     #0x32
                rtl
sys_close:      cop     #0x33
                rtl
sys_fsize:      cop     #0x34
                rtl
sys_dir:        cop     #0x35
                rtl
sys_unlink:     cop     #0x36
                rtl

sys_frames:     cop     #0x50
                rtl
sys_waitvbl:    cop     #0x51
                rtl

sys_trace_raw:  cop     #0x70
                rtl
