# Calypsi C + interrupts: the direct-page pseudo-register hazard

**Symptom:** occasional single wrong bytes. Not a crash, not a hang — one
character wrong in a 53-byte file, one byte wrong in 20 000, a filesystem magic
number that turns into `00` every few dozen boots. Everything looks correct in
the source, in the listing, and in the emulator's memory viewer afterwards.

**Cause:** Calypsi's generated code keeps working values in direct-page
"pseudo registers" (`_Dp`, `_Dp+2`, `_Dp+4`, …).

**Correction, 2026-07-28.** This document previously said the linker assigns
each function its own slot range from the static call graph. That is wrong, and
the link map says so plainly:

```
_Dp in section 'registers'  placed at address 000000-000013 of size 000014
```

**One shared 20-byte block, for every function in the program.** There is no
per-function partitioning to be defeated. Anything holding a value in `_Dp`
across a call is relying on the compiler having spilled it, and any second
context using `_Dp` at the same time collides — always, not occasionally.

That makes the hazard broader than described, and it explains the interrupt case
better than the original story did: a handler does not "reuse slots the linker
thought were free", it uses *the same twenty bytes*.

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

SNOS keeps **four** disjoint direct pages live (`snes/calypsi/linker.scm`):

| D | Owner |
|---|---|
| `$0000` | kernel foreground |
| `$1900` | the running application |
| `$1B00` | COP and BRK — synchronous traps |
| `$1C00` | NMI |

### One "interrupt" page is not enough

The first fix gave all three trap handlers a single shared page, which is wrong
for the same reason the original bug was wrong: **NMI fires while a system call
is executing**, so COP and NMI are concurrent with each other. Sharing one page
merely moves the collision from (foreground, NMI) to (COP, NMI).

It resurfaced as BASIC's `LOAD` reading `30 NEXT` back as line `3` — one dropped
character, in a file that was byte-for-byte correct on disk. Same signature,
one level down.

The rule: **every context that can be live simultaneously needs its own direct
page.** Today that is four. If M10 adds preemptive multitasking, each process
needs one too — which is another claim on the 8 KB of bank $00, and worth
remembering when budgeting that milestone.

### It is not only interrupts

Same block, third instance, 2026-07-28. The shell gained a fallback that runs an
application when a typed word is not a builtin. Written the obvious way —

```c
static int try_run_app(const char *name, const char *args) {
  const app_entry_t *a = app_find(name);
  if (a == 0) return 0;
  run_app_entry(a, args);          /* one more frame than cmd_run() has */
  return 1;
}
```

— the launched application crashed before reaching `main`, deterministically.
The panic dump is the giveaway:

```
PB:0000 PC:0005   D:1900 DB:007F S:18F2
```

The app's direct page, data bank and stack are all correctly installed, so
`app_exec` ran; but it jumped to `$00:0005`, meaning it was handed a bank and
entry of roughly zero. The `app_entry_t *` — a 24-bit pointer, too wide for the
accumulator and therefore travelling through `_Dp` — arrived corrupt.

The identical find-then-launch sequence inside `cmd_run()` has always worked. It
has one fewer call frame. Inlining the lookup at the call site fixes it, and
that is how it ships, with the reasoning attached so nobody "cleans it up".

The lesson generalises past interrupts: **a wide value passed down a call chain
lives in twenty bytes that everything else is also using.** When a change that
should be behaviour-neutral produces corruption, suspect an added frame before
suspecting your logic.

The trap shims in `snes/calypsi/header.s` do:

```asm
    lda     ##SYS_DP        ; $1B00 in the COP/BRK shims — NOT $0000
    tcd
...
    lda     ##NMI_DP        ; $1C00 in the NMI shim — and NOT $1B00 either
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
