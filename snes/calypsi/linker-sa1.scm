;; SNOS kernel memory description, SA-1 cartridge (map $23), `make SA1=1`.
;;
;; The RAM forms are linker.scm's, unchanged: read that file for the bank-$00
;; budget, the four direct pages and the one-bank HiRAM rule. Only the ROM
;; differs. The SA-1 maps ROM $0000-$7FFF at $00:8000 (linker.scm: ROM
;; $8000), so `code` and everything COP, BRK, NMI and reset reach is stored
;; at $C0:0000, the header at $C0:7FB0, the vectors at $C0:7FE0, and far code
;; and constants start at $C0:8000. Banks $C0-$FF are linear ROM, as on
;; HiROM, with the Super MMC at its reset values (main.c writes them anyway).
;;
;; BW-RAM ($40:0000-$43:FFFF) is not described here: the kernel reaches it
;; through far pointers only (kernel/sram.c).

(define memories
  '((memory KernelDirectPage
            (address (#x0000 . #x00ff))
            (type RAM)
            (section registers ztiny tiny))

    (memory KernelStack
            (address (#x0100 . #x0bff))
            (type RAM)
            (section stack))

    ;; $0C00-$1CFF: process slots, syscall block, trap direct pages.
    ;; Reserved by omission, as in linker.scm.

    (memory LoRAM
            (address (#x1d00 . #x1fff))
            (type RAM)
            (qualifier near)
            (section data znear near fastregs))

    (memory HiRAM
            (address (#x7e2000 . #x7edfff))
            (type RAM)
            (qualifier far)
            (section heap zfar far huge))

    (memory AppArena
            (address (#x7f0000 . #x7fffff))
            (type RAM)
            (qualifier far)
            (section apparena))

    ;; Bank $00 ROM window: ROM $0000-$7FAF on this cartridge.
    (memory LoROM
            (address (#x8000 . #xffaf))
            (type ROM)
            (qualifier near)
            (scatter-to LoROM-store)
            (section code compactcode cdata cnear switch itiny idata inear
                     data_init_table))

    (memory HeaderExtended
            (address (#xffb0 . #xffbf))
            (type ROM)
            (qualifier near)
            (section snesheaderextended)
            (scatter-to HeaderExt-store))

    (memory HeaderBasic
            (address (#xffc0 . #xffdf))
            (type ROM)
            (qualifier near)
            (section snesheader)
            (scatter-to Header-store))

    (memory Vector
            (address (#xffe0 . #xffff))
            (type ROM)
            (qualifier near)
            (scatter-to Vector-store)
            (section (snesvec1 #xffe0) (reset #xfffc) (snesvec2 #xfffe)))

    ;; The far view of the bank-$00 window. Nothing else goes here.
    (memory LoROM-c0
            (address (#xc00000 . #xc07fff))
            (type ROM)
            (qualifier far)
            (section (LoROM-store     #xc00000)
                     (HeaderExt-store #xc07fb0)
                     (Header-store    #xc07fc0)
                     (Vector-store    #xc07fe0)))

    (memory HiROM-c0
            (address (#xc08000 . #xc0ffff))
            (type ROM)
            (qualifier far)
            (section farcode cdata cnear cfar chuge switch ifar ihuge))

    ;; Kernel overflow: banks $C1-$C7. Apps start at $C8.
    (memory HiROM
            (address (#xc10000 . #xc7ffff))
            (type ROM)
            (qualifier far)
            (section farcode cdata cnear cfar chuge switch ifar ihuge))

    (block stack (size #xb00))
    (base-address _DirectPageStart KernelDirectPage 0)
    (base-address _NearBaseAddress LoRAM 0)
    ))
