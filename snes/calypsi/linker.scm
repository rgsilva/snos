;; SNOS kernel memory description — HiROM $31 (FastROM), 4 MB, 256 KB SRAM.
;;
;; THE BUDGET THAT MATTERS (PLAN.md §2): the 65816 confines both the stack and
;; the direct page to bank $00, and SNES bank $00 has exactly 8 KB of RAM
;; ($0000-$1FFF).  Every process's stack AND zero page must live there, forever.
;; This file is where that 8 KB is spent, so read it before adding anything.
;;
;;   $0000-$00FF   256 B   kernel direct page
;;   $0100-$0BFF  2816 B   kernel stack
;;   $0C00-$18FF  3328 B   APP stack        <- deliberately NOT described below,
;;   $1900-$19FF   256 B   APP direct page  <- so the linker cannot squat on it
;;   $1A00-$1A3F    64 B   SYSCALL PARAMETER BLOCK (fixed, part of the ABI)
;;   $1A40-$1AFF   192 B   (spare)
;;   $1B00-$1BFF   256 B   SYSCALL (COP/BRK) direct page  <- see below
;;   $1C00-$1CFF   256 B   NMI direct page                <- see below
;;   $1D00-$1FFF   768 B   kernel near data
;;
;; The app region is reserved by omission: no (memory ...) form covers
;; $0C00-$19FF, so ln65816 physically cannot place kernel data there.  M3 hands
;; those addresses to apps; M10 carves them into 512 B per-process slots.
;;
;; WHY INTERRUPTS HAVE THEIR OWN DIRECT PAGE
;; Calypsi's generated C code keeps working values in direct-page "pseudo
;; registers" (`_Dp`), and the linker assigns each function a slot range from
;; the STATIC call graph. An interrupt is an edge that graph does not contain,
;; so a handler's C code is free to reuse slots the interrupted code was in the
;; middle of using. The symptom is not a crash: it is one wrong byte, now and
;; then, wherever a pointer was being computed when NMI landed.
;;
;; Because `_Dp` is direct-page RELATIVE, giving the trap shims their own D
;; makes the collision impossible rather than merely unlikely.
;;
;; COP and NMI need SEPARATE pages, not one shared "interrupt" page: NMI fires
;; while a system call is executing, so sharing one merely moves the collision
;; from (foreground, NMI) to (COP, NMI). Four disjoint direct pages are live:
;;   $0000 kernel foreground   $1900 application
;;   $1B00 COP/BRK             $1C00 NMI
;;
;; Test mailbox lives at $7EF000 (out of bank $00 entirely — it is 4 KB, and
;; bank $00 is far too expensive to spend on test scaffolding).

(define memories
  '((memory KernelDirectPage
            (address (#x0000 . #x00ff))
            (type RAM)
            (section registers ztiny tiny))

    (memory KernelStack
            (address (#x0100 . #x0bff))
            (type RAM)
            (section stack))

    ;; $0C00-$19FF is the app's (stack + direct page). Reserved by omission.

    ;; $1A00-$1A3F is the syscall parameter block. It is at a FIXED address
    ;; because apps are linked separately from the kernel and cannot be told
    ;; where a linker-allocated variable ended up; see kernel/snos_abi.h.
    ;; Reserved by omission, exactly like the app stack region.

    ;; $1B00-$1BFF (COP/BRK) and $1C00-$1CFF (NMI) are trap direct pages, see
    ;; above. Reserved by omission, like the app region and the parameter block.

    (memory LoRAM
            (address (#x1d00 . #x1fff))
            (type RAM)
            (qualifier near)
            (section data znear near fastregs))

    ;; WRAM $7E2000-$7EEFFF: kernel far data (console shadow, ROMFS cache,
    ;; later uIP buffers).  $7EF000-$7EFFFF is the mailbox — see mailbox.h.
    (memory HiRAM
            (address (#x7e2000 . #x7eefff))
            (type RAM)
            (qualifier far)
            (section heap zfar far huge))

    ;; WRAM bank $7F: the app arena. Every app links its bss/heap here, at the
    ;; same addresses, because only one app runs at a time (PLAN.md §1).
    ;; The kernel does not allocate from it.
    (memory AppArena
            (address (#x7f0000 . #x7fffff))
            (type RAM)
            (qualifier far)
            (section apparena))

    ;; Bank $00 ROM window. $00:8000-$FFFF mirrors $C0:8000-$FFFF, and the CPU
    ;; fetches every vector (and every COP/BRK trap, which force PB=$00) from
    ;; bank $00 — so the trap handlers must live in `code` here, not in farcode.
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

    (memory HiROM-c0
            (address (#xc00000 . #xc0ffff))
            (type ROM)
            (qualifier far)
            (section farcode cdata cnear cfar chuge switch ifar ihuge
                     (LoROM-store   #xc08000)
                     (HeaderExt-store #xc0ffb0)
                     (Header-store  #xc0ffc0)
                     (Vector-store  #xc0ffe0)))

    ;; Kernel overflow: banks $C1-$C7 (448 KB). Apps start at $C8 (M3).
    (memory HiROM
            (address (#xc10000 . #xc7ffff))
            (type ROM)
            (qualifier far)
            (section farcode cdata cnear cfar chuge switch ifar ihuge))

    (block stack (size #xb00))
    (base-address _DirectPageStart KernelDirectPage 0)
    (base-address _NearBaseAddress LoRAM 0)
    ))
