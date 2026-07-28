;;; Kernel stack watermark instrumentation.
;;;
;;; PLAN.md §2 calls the 8 KB of bank-$00 RAM the design's binding constraint,
;;; and §7 lists "the budget blows up" as the top risk. So measure it from M0
;;; rather than discovering it at M10: paint the unused stack at boot, then
;;; report how far down it was ever driven. The Lua harness asserts the kernel
;;; stack never leaves $0100-$0BFF.
;;;
;;; Both routines assume Calypsi's normal state (A and X/Y 16-bit) and return
;;; a 16-bit value in A, which is Calypsi's convention for `uint16_t`.

              .rtmodel version, "1"
              .rtmodel core, "*"

              .section code, root
              .public stack_paint, stack_low

KSTACK_LO     .equ    0x0100
KSTACK_HI     .equ    0x0bff
PATTERN       .equ    0xa5a5
HEADROOM      .equ    48      ; don't paint over our own frame / return addr

;;; void stack_paint(void)
;;; Fill $0100 .. (S - HEADROOM) with PATTERN. Everything below the live stack
;;; pointer is by definition unused, so this cannot corrupt anything.
stack_paint:
              rep     #0x30
              tsc                           ; A = current stack pointer
              sec
              sbc     ##HEADROOM
              ;; Force even alignment. S is $0BFF at entry (odd), so without
              ;; this the loop steps ...$0103, $0101, $00FF and never writes
              ;; $0100 itself — which is exactly where stack_low starts its
              ;; scan, making the watermark read as "entire stack consumed".
              and     ##0xfffe
              tax                           ; X = highest address to paint
              lda     ##PATTERN
paint_lp:
              cpx     ##KSTACK_LO
              bcc     paint_done
              sta     long:0x7e0000,x       ; bank $00 $0000-$1FFF mirrors $7E:0000-$1FFF
              dex
              dex
              bra     paint_lp
paint_done:
              rtl

;;; uint16_t stack_low(void)
;;; Lowest address the stack ever reached: scan up from $0100 for the first
;;; word the painter's pattern no longer occupies. Returns $0BFF+1 if the
;;; stack never grew into the painted region at all.
stack_low:
              rep     #0x30
              ldx     ##KSTACK_LO
scan_lp:
              lda     long:0x7e0000,x
              cmp     ##PATTERN
              bne     scan_done
              inx
              inx
              cpx     ##KSTACK_HI
              bcc     scan_lp
scan_done:
              txa
              rtl
