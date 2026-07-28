# Calypsi C + interrupts: the direct-page pseudo-register hazard

**Symptom:** occasional single wrong bytes. Not a crash, not a hang — one
character wrong in a 53-byte file, one byte wrong in 20 000, a filesystem magic
number that turns into `00` every few dozen boots. Everything looks correct in
the source, in the listing, and in the emulator's memory viewer afterwards.

**Cause:** Calypsi's generated code keeps working values in direct-page
"pseudo registers" (`_Dp`, `_Dp+2`, `_Dp+4`, …). The *linker* assigns each
function a slot range, and it does so by analysing the **static call graph** —
that is how two functions that can never be live at once safely share slots.

An interrupt is an edge that graph does not contain. When NMI fires, the
handler's C code is free to reuse exactly the slots the interrupted foreground
code had a half-computed pointer sitting in. The foreground then finishes its
address calculation with one word replaced, writes a byte to the wrong place,
and carries on.

It is intermittent by nature: it only goes wrong when the interrupt lands inside
the specific instruction window where a slot is live. The more far-memory work
per frame, the more often it bites. In SNOS it was invisible through M0-M3 and
became obvious in M4 only because the filesystem does thousands of far accesses
per second.

## Fix

`_Dp` is addressed **relative to the D register**, so handlers do not need to
save and restore anything. Give them a different direct page and the collision
becomes impossible rather than merely unlikely.

SNOS keeps three disjoint direct pages live (`snes/calypsi/linker.scm`):

| D | Owner |
|---|---|
| `$0000` | kernel foreground |
| `$1900` | the running application |
| `$1B00` | **trap handlers** — NMI, COP, BRK |

The trap shims in `snes/calypsi/header.s` do:

```asm
    lda     ##IRQ_DP        ; $1B00, NOT $0000
    tcd
```

The obvious-looking `lda ##0 / tcd` — "switch to the kernel's direct page" — is
precisely the bug.

## What this rules out, and what it does not

This only protects the *pseudo registers*. It works because no C variable is
shared between handler and foreground through the direct page: with
`--data-model=large` everything lands in far memory, and the map confirms the
`registers` section is the only thing in DP.

If a future change puts a variable in `ztiny`/`tiny`, or uses Calypsi's `__tiny`
qualifier, that variable exists at a **different address** in each of the three
direct pages, and handler and foreground will silently see different copies.
(`__tiny` is separately unusable here anyway — it crashes the compiler; see
`snespdp11/docs/M4-notes.md`.)

## How it was found

`cat MOTD.TXT` printed `"Performance does not mStter"`. One byte, deterministic
enough to notice, rare enough to look like a fluke. Chasing it through the
listings ruled out the addressing arithmetic — `sram_ptr` and the far-pointer
codegen were both correct — which left "something else is writing to these
addresses", and the only other thing running was the NMI handler.

The lesson worth keeping: a *single wrong byte* is a much stronger signal than a
crash. It points at concurrency or aliasing, not at logic.
