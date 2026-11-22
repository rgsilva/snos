;; sa1probe memory description: everything the linker places is in LoROM bank
;; $00 ($00:8000-$FFFF = ROM offset $0000-$7FFF), which both CPUs execute
;; from. RAM is laid out by hand in src/layout.inc, not here. The rest of the
;; 4 MB image (markers, filler) is written by tools/mksfc.py.
(define memories
  '((memory Bank0
            (address (#x8000 . #xffff))
            (type ROM)
            (fill #xff)
            (section code
                     (romtag #xff80)
                     (sneshdrext #xffb0)
                     (sneshdr #xffc0)
                     (vectors #xffe0)))))
