# Calypsi `as65816` syntax — the non-obvious bits

Determined empirically against Calypsi 5.17 on 2026-07-27, because the assembler
accepts several plausible-looking spellings and **silently assembles the wrong
instruction** for some of them. Verified by reading the emitted bytes in a
`--list-file`, not by "it assembled without error".

## The assembler does NOT track `rep`/`sep`

There is no `.longa` / `.a16` / `.assume` / `.rega` directive — all of those are
rejected as unknown instructions. The assembler has no idea whether M/X are 8 or
16 bits, and **defaults to the short form**. You must state the width per operand.

## Immediates: `##` is 16-bit, `#` is 8-bit

```asm
    lda  #0x12          ; A9 12       8-bit immediate
    lda  ##0xa5a5       ; A9 A5 A5    16-bit immediate
    rep  #0x30          ; C2 30       correct — rep/sep operands really are 8-bit
```

`lda #0xa5a5` is a hard error ("value 42405 is out of range"), which is fine —
it's loud. The dangerous one is below.

## Addressing: `abs:` forces absolute, bare is direct page

```asm
    sta  0x0000,x       ; 95 00       STA dp,X       <-- probably NOT what you want
    sta  abs:0x0000,x   ; 9D 00 00    STA abs,X      <-- this one
    sta  !0x0000,x      ; 95 01       (something else entirely)
    jmp  long:target    ; 5C ...      long jump
    lda  dp:.tiny sym   ; direct page (see snesmac/snes/calypsi/m68krun.s)
```

**`sta 0x0000,x` assembles cleanly and does the wrong thing.** Writing to
`$00:0000+X` via direct-page indexing happens to work *only* when `D` is 0; the
moment a routine runs with a relocated direct page it silently corrupts memory
at `D+X`. Always write `abs:` when you mean absolute.

`near:` and `.a`/`.w` suffixes do not exist.

## Constants

```asm
NAME   .equ   0x0100      ; not `NAME = 0x0100` — `=` is an unknown instruction
```

## Cross-checking a suspicious line

```sh
as65816 -o t.o t.s --core=65816 --target=SNES --list-file=t.lst && cat t.lst
```

The listing shows the emitted bytes per line. For anything involving an
immediate, an indexed mode, or a symbol that might be direct-page, look at the
bytes rather than trusting the absence of an error.

## Prior art in this tree

`snesmac/snes/calypsi/tilecopy.s` and `m68krun.s` are the reference for
hand-written Calypsi 65816 — they use `##` and `long:`/`dp:.tiny` throughout.
