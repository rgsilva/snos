;;; void ld_nmi_entry(void) -- LINKDIAG only (kernel/linkdiag.c).
;;;
;;; The NMI's first C call, before pad_latch: $4212, then the beam latched
;;; and read (V low, V high, H low, H high) and STAT78 after the latch, raw
;;; into ld_e[0..5]; ld_e[6] = 1 marks the NMI's context, ld_net_ran = 0.
;;; In assembly because the same in C at -O0 cost 1.1 lines here, and
;;; pad_latch has to start while the auto-joypad read is still running.
;;; Touches no _Dp byte; returns with A/X/Y 16 bits wide.
              .rtmodel version, "1"
              .rtmodel core, "*"

              .section farcode, root
              .public ld_nmi_entry
              .extern ld_e, ld_net_ran

ld_nmi_entry:
              sep     #0x20
              lda     long:0x004212
              sta     long:ld_e
              lda     long:0x00213f         ; read toggle to the low half
              lda     long:0x002137         ; latch H and V
              lda     long:0x00213d
              sta     long:ld_e+1
              lda     long:0x00213d
              sta     long:ld_e+2
              lda     long:0x00213c
              sta     long:ld_e+3
              lda     long:0x00213c
              sta     long:ld_e+4
              lda     long:0x00213f         ; STAT78, and the toggle reset
              sta     long:ld_e+5
              lda     #1
              sta     long:ld_e+6
              lda     #0
              sta     long:ld_net_ran
              rep     #0x20
              rtl
