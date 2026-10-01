;;; The port-2 link exchange: one frame out, one frame in, over a five-wire
;;; controller cable (docs/pico-adapter.md §4).
;;;
;;;   void link_serial_xfer(const uint8_t *tx, uint8_t *rx, uint16_t n)
;;;
;;; The console is the master. Per bit: write the outgoing bit to LATCH
;;; ($4016 bit 0), then read $4017 -- the read samples DATA1 (bit 0, the
;;; adapter's bit, INVERTED by the CPU) and emits the one clock pulse the
;;; adapter samples LATCH on.
;;; MSB first, n bytes. There is no sync line: the adapter finds frame
;;; boundaries by the silence between bursts, so this routine guarantees
;;; >= 300 us with no clocks before the first bit, after waiting out any
;;; auto-joypad read (which clocks the port by itself).
;;;
;;; Calling convention as in netasm.s: tx in _Dp/_Dp+2, rx in _Dp+4/_Dp+6,
;;; n in A. Both pointers are used through [dp],y, so any bank works. The
;;; scratch bytes are the pad bytes of the two pointer slots (_Dp+3, _Dp+7);
;;; nothing above _Dp+7 is touched. DB is set to $00 for the registers and
;;; restored. ~26 CPU cycles a bit, ~8 us; a 64-byte frame is ~2.5 ms.
;;;
;;; Shared verbatim by the kernel (link_serial.c) and apps/wire, which links
;;; its own copy through a symlink: an app is a separate link.
              .rtmodel version, "1"
              .rtmodel core, "*"

              .extern _Dp

              .section farcode, root
              .public link_serial_xfer, link_serial_kick

;;; One bit. `lda #0 / rol a` turns the carry into bit 0 without a branch.
xsbit         .macro
              asl     dp:.tiny (_Dp+3)   ; C = the next tx bit, MSB first
              lda     #0
              rol     a                  ; A = that bit, in bit 0
              sta     abs:0x4016         ; LATCH = the bit
              lda     abs:0x4017         ; samples DATA1, then clocks
              eor     #1                 ; the CPU reads the line inverted
                                         ; (a pad's active-low buttons read
                                         ; as 1): line high = 0, measured
              lsr     a                  ; C = the adapter's bit
              rol     dp:.tiny (_Dp+7)   ; into the rx byte
              .endm

link_serial_xfer:
              rep     #0x30
              phb
              tax                     ; X = n, the byte countdown
              ldy     ##0             ; Y = byte index for both pointers
              sep     #0x20
              lda     #0
              pha
              plb                     ; DB = $00: the registers below are absolute
              ;; An auto-joypad read in progress clocks the port; wait it
              ;; out, bounded (256 x ~14 cycles covers its 4224 master cycles).
              sta     dp:.tiny (_Dp+7)
xs_busy:      lda     abs:0x4212
              and     #0x01
              beq     xs_idle
              dec     dp:.tiny (_Dp+7)
              bne     xs_busy
xs_idle:
              ;; LATCH low, then silence: 220 x 5 cycles at 6 master cycles
              ;; is ~6600 master cycles, ~307 us, past the adapter's gap
              ;; timeout with margin. It restarts at bit 0 in that time.
              lda     #0
              sta     abs:0x4016
              lda     #220
xs_quiet:     dec     a
              bne     xs_quiet
xs_byte:
              lda     [.tiny _Dp],y   ; the tx byte
              sta     dp:.tiny (_Dp+3)
              xsbit
              xsbit
              xsbit
              xsbit
              xsbit
              xsbit
              xsbit
              xsbit
              lda     dp:.tiny (_Dp+7)
              sta     [.tiny (_Dp+4)],y
              iny
              dex
              beq     xs_done
              brl     xs_byte         ; the unrolled byte is past a short branch
xs_done:
              lda     #0
              sta     abs:0x4016      ; leave LATCH low, as a game would
              rep     #0x30           ; A/X/Y back to 16 bits for the caller
              plb
              rtl

;;; void link_serial_kick(void)
;;;
;;; Eight bare clocks on port 2 and no LATCH writes: a short burst, the shape
;;; of the vblank auto-joypad read, which the adapter's gap detector turns
;;; into a restart ~64 us later. A restart is where pico/port.c stages the
;;; answer it computed to the last exchange, so link_serial.c kicks before an
;;; exchange that follows another in the same video frame -- one with no
;;; auto-read before it (docs/link-throughput.md). The adapter drops the
;;; burst by count, as it drops the auto-read's sixteen clocks.
;;;
;;; The clocks are ~6 us apart -- a little quicker than the data bits' ~8 us
;;; cell, far slower than the PIO's ~160 ns poll -- and LATCH stays low. An
;;; auto-joypad read in progress is waited out first, as link_serial_xfer
;;; does. Touches no _Dp byte; DB is set to $00 and restored.
lkbit         .macro
              lda     abs:0x4017      ; one clock
              nop
              nop
              nop
              nop
              nop
              nop
              nop
              nop
              .endm

link_serial_kick:
              rep     #0x30
              phb
              sep     #0x30           ; A and X 8 bits
              lda     #0
              pha
              plb                     ; DB = $00 for the registers
              ldx     #0              ; bounds the wait: 256 turns
lk_busy:      lda     abs:0x4212
              and     #0x01
              beq     lk_idle
              dex
              bne     lk_busy
lk_idle:
              lkbit
              lkbit
              lkbit
              lkbit
              lkbit
              lkbit
              lkbit
              lkbit
              rep     #0x30           ; A/X/Y back to 16 bits for the caller
              plb
              rtl
