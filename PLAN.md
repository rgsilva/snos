# SNOS — a native 65816 "OS" for the stock SNES — Build Plan

*Authored 2026-07-27. Basis: the 2026-07-27 research pass (SNES/65816 OS prior art, networking
options, display modes, and an inventory of reusable infra across `emu8086` / `snesmac` /
`snespdp11` / `sneslinux`). This file is the handoff doc for the build session(s).*

**What this is:** not an emulator. Every other project in this tree emulates a foreign machine.
SNOS is **native 65816 code** — a small supervisor that owns the hardware, presents a shell,
and runs applications that live in cartridge ROM.

**What it is not:** a Unix, a multitasking kernel (until M10), or a port of anything.

---

## 0. Why this is feasible, and why it's *different* from the rest of this repo

The other projects in this tree fight the "four laws" in `../IDEAS.md` §0 — speed inversion,
no framebuffer, read-only ROM, reuse cores. SNOS sidesteps three of the four:

- **No speed inversion.** There is no guest CPU. Native 65816 at 3.58 MHz means apps run at
  *full machine speed* — the first project here that isn't a slideshow.
- **ROM being read-only is a feature, not a tax.** Cart ROM is memory-mapped and directly
  executable, so "load a program" is a **build-time** decision. There is no loader, no
  relocation, no disk. `exec` is: set DB/DP/S, `JML` to a bank. Exit is `COP #0`.
  *This is the single insight the whole design rests on.*
- **RAM stops being the binding constraint.** No guest core to host. The real scarce resource
  is 8 KB (see §2), not 128 KB.

**External calibration:** a shipped, official SNES multi-game menu (Nintendo Power / SF Memory)
is **~16 KB of 65816 code**. That is the scale of the SNOS kernel. This is not a heroic project.

**Prior art (searched GitHub topics `snes`/`65816`/`65c816`, nesdev, pouët, Hackaday, 2026-07-27):**
no SNES homebrew OS with a kernel + driver model + app ABI exists. Launchers exist (sd2snes,
BS-X, SF Memory, copier BIOSes); one ported OS exists (CP/M-65, 6502, 58 KB TPA). The gap is real.

**Closest existing thing:** [micropython-snes](https://github.com/FabianKuebler/micropython-snes)
(MIT) — MicroPython on a stock SNES via Calypsi, with an `mpyos.sfc` "workstation" image
(file manager over SRAM, full-screen editor, OSK, REPL). **Read its patch set before writing
C** — it works around 23 Calypsi codegen bugs, overlapping our own notes in
`snesmac/docs/INTERNALS.md` and `snespdp11/docs/M{3,4}-notes.md`.

### Why build rather than port (decided 2026-07-27)

| Candidate | Verdict |
|---|---|
| **CP/M-65** ([repo](https://github.com/davidgiven/cpm65), BSD-2) — *a SNES port already exists*, 64-col Mode 5 console, romdisk + SRAM ramdisk | **No.** It's a **6502** reimplementation capped at a 58 KB TPA — throws away the 65816 and ~85% of our RAM. Its SNES target also assumes unreleased Nintendo prototype floppy/keyboard hardware and a patched bsnes-plus. Worth reading for its relocatable-binary scheme. |
| **Fuzix** ([repo](https://github.com/EtchedPixels/FUZIX)) | **No.** Needs banked RAM (6502 target assumes 512 K + 512 K), a **writable block device** for `/` and exec, 512 B of bank-$00 *per process*; the 65816 port explicitly lacks exec relocation and treats the CPU as "a 65C02 with extras" (cc65 has no 65816 codegen). |
| **Contiki 2.x** ([oliverschmidt fork](https://github.com/oliverschmidt/contiki), BSD-3) | **Don't port wholesale — harvest.** Best architectural fit alive (~30 KB RAM *with* GUI, protothreads, uIP), but welded to cc65 and to 64 KB-flat assumptions. Take uIP and the **apps** (§6). |
| **GS/OS / ProDOS 16** | **No** (closed, ≥256 KB–1 MB, disk-assuming) — but the best *design* to read: OMF segments, jump-table demand loading, handle-based memory with purge levels. |

**Net: build the kernel, steal the app layer.** The kernel is days of work; the browser is
months. Optimize for app reuse, not kernel elegance.

---

## 1. Locked design decisions

| Decision | Choice | Why |
|---|---|---|
| Cart | **HiROM `$21` + FastROM (`$31`), 4 MB, 256 KB SRAM, no SA-1** | Ricardo's cart is a Super Everdrive clone → no custom mappers, no SA-1. HiROM gives 64 KB contiguous banks (clean 1-bank-per-app) **and** the 256 KB SRAM measured by `snespdp11/snes/sramprobe.c`. FastROM = measured ~14% (`snesmac/docs/PERF.md` item C). |
| Toolchain | **Calypsi C 65816 5.17** (`../../snespico8/tools/calypsi-65816-5.17`), `-O0` baseline | Two projects of precedent. `-O2` miscompiles setjmp-heavy code; `--speed` miscompiles; `__tiny` crashes the compiler and `__near` miscompiles (`snespdp11/docs/M4-notes.md`). Hand-write asm where it matters, don't chase compiler qualifiers. |
| Syscall trap | **`COP #n`**, n = `$00–$7F` | Nintendo's own SFX-DOS (1991) used exactly this (`cop 15h` init, `cop 21h` load). Vector `$00:FFE4` is unclaimed on SNES; 2 bytes at the call site vs 4 for `JSL`; ~15 cycles round trip; correctly emulated in bsnes/snes9x/Mesen. `$80–$FF` reserved by WDC — don't use. |
| Debugger trap | **`BRK`** (`$00:FFE6`), never `COP` | Keeps the debugger out of syscall signature space. Mesen's break-on-`WDM` (`$42`) is a third, emulator-only channel. |
| App binding | **Static, build-time. One app = N contiguous HiROM banks.** | No mapper on a stock cart ⇒ no remap, no reset trick. Relocation is unnecessary because ROM is already executable in place. |
| App RAM | ~~Every app links to the same arena~~ **M10 step 4: every app links to its DECLARED arena slot** (`APP_ARENA_*`) | Two apps co-run iff their slots are disjoint; refusal is at spawn, by name. Like DOS `.COM` at a fixed org. Per-app `linker.scm` differs *only* in ROM base ⇒ a Makefile template parameterised by bank number. |
| Exit | **`COP #0`** (exit code in A); kernel restores its saved `S` | Works from arbitrary call depth, which `RTL` cannot. `RTL` from the entry point also lands in a kernel trampoline, so trivial apps can use either. |
| Kill switch | **Kernel NMI handler force-exits on START+SELECT held ~1 s** | Answers "how do I get out of BASIC" *generally*, for every app including buggy ones. Non-negotiable — it's the only thing standing between a hung app and a power cycle. |
| Interrupts | **Kernel owns NMI unconditionally.** Apps hook via a RAM vector table, never by writing `$00:FFEA` | Vectors are fetched from bank `$00` = ROM, so they're baked at build time; indirection through RAM is how BS-X and sd2snes both do it. Kernel-owned NMI is *also* what makes background networking possible without cooperative yielding (§5). |
| Console | **`snespdp11/snes/console.c` verbatim** — Mode 5, 64×28, 8 px font | Already written, already debugged, standalone by design. **80 columns is impossible** (512/80 = 6.4 px — non-integer cell alignment; needs 1792 tiles vs a 1024 index limit; ~3 lo-res px/glyph). 64 is the hardware ceiling. |
| Input | **Abstraction from day 1**: `kbd_getchar()` → ASCII, three backends | OSK (M1), Lua test-feed (M1, drives CI), Pico USB bridge (M7). Never let the shell talk to `$4218` directly. |
| Network | **Controller port 1, synchronous serial**, byuu/near protocol | ~20 KB/s practical each way, full duplex free, ~$10–13 of parts, **no cartridge hardware**, and already emulated in bsnes. Keyboard moves to port 2. |
| Test harness | **Mesen `--testrunner` + WRAM mailbox** (built at `$7EF000`, not the `$0100` first planned — bank $00 is too contended) | Reuse `snesmac/snes/calypsi/mailbox.{h,c}` (bidirectional stdio ring) + `emu8086/snes/test/suite.sh` `RESULT PASS/FAIL` protocol wholesale. |

### Deliberately deferred

- **Preemptive multitasking** → M10. The design keeps the door open (§2) but nothing before
  M10 may assume more than one live process.
- **Sound.** The APU is untouched. No SPC700 code until there's a reason.
- **A GUI / windowing.** Text only. CTK is a post-M9 conversation, if ever.
- **Bytecode apps.** The app header has a type field reserving the option; nothing implements it.

---

## 2. The memory budget — read this before writing any code

### The binding constraint

The 65816 confines **both the stack and the direct page to bank `$00`** (W65C816S datasheet:
*"the effective address of Stack and Stack Relative addressing modes will always be within this
range"*; same for Direct, Direct,X, Direct,Y). On SNES, bank `$00` has exactly **8 KB of RAM**
(`$0000–$1FFF`, the WRAM mirror). Everything else in bank `$00` is I/O or ROM.

**So: every process's stack *and* zero page must live in that 8 KB, forever.** This — not the
128 KB — is the scarcest resource in the design, and it is what caps M10 at ~8–12 processes.

Validation: BS-X reserves exactly `$7E:0000–$1FFF` plus the saved SP for its return-to-town
contract; the sd2snes menu backs up exactly the same region to `$FF2000` before launching a
game. Two shipped systems converged on it independently.

### Bank $00 RAM ($0000–$1FFF, 8 KB) — allocate this first

As built (`kernel/snos.h` is the authority — this table drifted once and sat wrong for weeks;
the test mailbox also moved out of bank $00 entirely, to `$7EF000`, see `kernel/mailbox.h`):

| Range | Size | Owner |
|---|---|---|
| `$0000–$00FF` | 256 B | Kernel direct page (Calypsi `_Dp` pseudo-registers) |
| `$0100–$0BFF` | 2.75 K | Kernel stack |
| `$0C00–$11FF` | 1.5 K | Process slot 0 stack (M10; measured worst case 1,123 B) |
| `$1200–$17FF` | 1.5 K | Process slot 1 stack (the single-app path) |
| `$1800–$18FF` | 256 B | Slot 0 direct page |
| `$1900–$19FF` | 256 B | Slot 1 direct page |
| `$1A00–$1A3F` | 64 B | Syscall parameter block + exec state (`snos_abi.h`) |
| `$1B00–$1BFF` | 256 B | COP/BRK handler direct page |
| `$1C00–$1CFF` | 256 B | NMI handler direct page |

M10 carved exactly this (step 3, measured slot sizes); the table above IS the carving.

### Everything else

| Region | Size | Use |
|---|---|---|
| WRAM `$7E2000–$7EFFFF` | 56 K | Kernel data: console shadow + dirty-row map, ROMFS cache, OSK tables, later uIP buffers |
| WRAM `$7F0000–$7FFFFF` | 64 K | **App arena** — every app's bss/heap, linked to the same addresses |
| Cart SRAM `$20–$3F:6000–7FFF` | 256 K | **Writable filesystem** (32 × 8 K windows) |
| ROM `$C0` | 64 K | Kernel: vectors + header (upper 32 K, mirrored at `$00:8000`), COP handler, syscall table, console, drivers, shell |
| ROM `$C1–$C7` | 448 K | Kernel overflow, shared libs, fonts, ROMFS blob |
| ROM `$C8–$FF` | 3.5 M | **Apps**, one or more banks each |

**Speed notes that will bite:** WRAM is *always* 8 master cycles — FastROM does not apply to
RAM, and the SNES has no fast RAM. Code in WRAM runs ~33% slower than FastROM, which is another
argument for executing apps in place from ROM. Bulk WRAM loads must go DMA → `$2180`
(8 cycles/byte) not `MVN` (~44 cycles/byte). Parking `D = $2100` buys direct-page-speed PPU
access from any code bank, free.

---

## 3. The app ABI

### App header — at offset `$0000` of the app's first bank

BS-X's header with the broadcast fields deleted. Self-describing, so **no separate directory
blob is needed**: the kernel scans bank starts `$C8`–`$FF` for the magic at boot (56 checks,
trivial) and skips ahead by `bank_count`.

| Offset | Size | Field |
|---|---|---|
| `$00` | 4 | Magic `"SNOS"` |
| `$04` | 2 | App ABI version |
| `$06` | 2 | Minimum kernel ABI version required |
| `$08` | 2 | Entry point (offset within this bank) |
| `$0A` | 1 | Type: `0` = native 65816, `1` = reserved (bytecode) |
| `$0B` | 1 | Bank count |
| `$0C` | 16 | Name, ASCII, NUL-padded |
| `$1C` | 1 | SRAM quota, in 8 K blocks (0 = none) |
| `$1D` | 1 | Flags: wants-NMI-hook, wants-network, wants-raw-input |
| `$1E` | 1 | Arena start page (M10: data at `$7F:page*256`) |
| `$1F` | 1 | Arena page count (0 = 256; was the never-used checksum) |

### Exec contract

1. Kernel saves `S`, sets up app DP (`$1900`) and app `S` (`$17FF`, slot 1), sets `DB` = `$7F` (arena).
2. `JML` to `(bank, entry)`.
3. App runs. Its `DB`/`PB` are its own bank ⇒ **apps are bank-position-independent for free**,
   as long as they use `JMP`/`JSR` (bank-relative) internally and never `JSL` to themselves.
4. Exit via `COP #0` with exit code in A. Kernel restores the saved `S` and returns to the shell.
5. Kernel restores *everything* — PPU state, console, DP, DB, vectors. **An app is required to
   preserve nothing** except that it must not write outside its own DP/stack in bank `$00`.

### Syscall groups (`COP #n`)

Args in A/X/Y; complex calls pass a DP-relative param block pointer. Return in A, carry = error.

| Range | Group |
|---|---|
| `$00–$0F` | Process: `exit`, `exec`, `yield`, `getargs`, `getversion` |
| `$10–$1F` | Console: `putc`, `puts`, `getc`, `cls`, `gotoxy`, `scroll`, `setattr` |
| `$20–$2F` | Input: `getchar`, `pollpad`, `osk_toggle`, `raw_mode` |
| `$30–$3F` | File: `open`, `read`, `write`, `close`, `seek`, `readdir`, `stat` |
| `$40–$4F` | Memory: `arena_alloc`, `arena_free`, `arena_stat` |
| `$50–$5F` | Time: `frames`, `waitvbl`, `sleep` |
| `$60–$6F` | Network (M6+): `sock_open`, `send`, `recv`, `close`, `resolve` |
| `$70–$7F` | Debug: `trace`, `panic`, `mailbox_puts` |

**Handler constraints** (all verified): `COP` forces `PB = $00`, so the handler entry must live
in bank `$00` (i.e. the upper 32 K of `$C0`, which mirrors there). It sets `I` but **NMI still
fires inside a syscall** — the handler must be NMI-safe. `DB` and `D` are *not* touched, so the
handler inherits the caller's — useful for reading the caller's param block, but the kernel must
`PHD`/`TCD` if it wants its own. The pushed return address points *past* the signature byte, so
read it at `[return_address - 1]`.

---

## 4. Milestones

Each milestone ends with a **runnable ROM** and an **automated Mesen test**. No milestone is
"done" until `make test` is green on it.

### M0 — Scaffold
Fork `snespdp11/snes/{Makefile, calypsi/linker.scm, calypsi/header.s, tools/raw2sfc.py}` and
reshape `linker.scm` to §2. Boot, set FastROM, init PPU, print a banner, spin.
**Done when:** `snos.sfc` shows `SNOS v0.1` and a Lua test asserts the mailbox magic.
**Watch:** header must be HiROM `$31` + MEMSEL (copy `snesmac/snes/calypsi/macheader.s`, not
snespdp11's `$21`).

### M1 — Console + input abstraction
Drop in `console.c` unmodified. Port `snesmac/snes/tools/mkkbd.py` for the OSK. Define
`kbd_getchar()` with three backends: OSK, Lua-injected feed, stub. Line editor with history.
**Done when:** a Lua test types `hello world\n` into the line editor and asserts the echo.
**Read first:** `snespdp11/docs/console-notes.md` — two expensive bugs already paid for
(`$212C`/`$212D` must both be `$03`; build row buffers *outside* vblank, DMA inside).

### M2 — Syscall ABI + shell
`COP` handler, dispatch table, versioning. `BRK` → panic screen with register dump. Shell with
builtins: `help`, `ver`, `clear`, `mem`, `ls`.
**Done when:** interactive shell; a test drives 5 builtins and asserts output.

### M3 — Apps: exec and return ← *the milestone that proves the thesis*
App header, boot-time bank scan, `run <name>`, the `COP #0` exit path, and the START+SELECT
kill switch. Two apps:
- **`hello`** — prints, exits. The ABI smoke test.
- **`pad`** — live controller state display, exits on START+SELECT. Ricardo's requested
  controller test, and it doubles as the kill-switch test.

Build-system work: a `linker.scm` template parameterised by bank number, so adding an app is
one Makefile line.
**Done when:** shell → `run hello` → shell → `run pad` → kill switch → shell, fully automated.
**This is the "is SNOS real?" gate.** Everything after it is content.

### M4 — Filesystem
Re-run `sramprobe` under this exact cart config to confirm 256 K, then:
- **ROMFS** — read-only, packed by a Python tool (convention-over-configuration, modelled on
  `emu8086/snes/tools/mkhd_free.py`), placed via `raw2sfc.py --at`.
- **SRAMFS** — flat, writable, on the 256 K. Wipe-on-first-boot via `MVN` (Mesen persists
  `.srm`; `snesmac` learned this the hard way).

Syscalls `$30–$3F`. Apps: `cat`, `cp`, `rm`, real `ls`.
**Done when:** write a file, soft-reset, read it back — asserted in Lua.

### M5 — BASIC
Port `tinybasic/` (Stefan's BASIC — already vendored, has a HAL, no SNES port exists) as an app.
HAL → SNOS syscalls. **`BYE` → `COP #0`.** Save/load programs to SRAMFS.
**Done when:** `run basic`, enter a 10-line program, `RUN`, `SAVE`, `BYE`, `run basic`, `LOAD`, `RUN`.
**Watch:** Calypsi `int` is 16-bit; `(int8_t)` does not sign-extend; `~1u` is 16-bit; arrays
>64 KB are rejected. All four have bitten this repo before.

### M6 — Network stack, emulated
Port **uIP** (BSD-3, from Contiki). Driver over byuu/near's controller-port USART protocol.
Host-side daemon for the far end. Loopback → ARP → ICMP → TCP echo.
**Done when:** the SNES completes a TCP echo against a host daemon, in bsnes.
**Toolchain split, probably unavoidable:** bsnes emulates byuu's USART cable (verified). Mesen
support is **believed absent but not confirmed — check this first at M6**, since if Mesen does
support it, the whole existing Lua harness carries over and this milestone gets much cheaper.
Otherwise M6+ network tests run under bsnes while everything else stays on Mesen.
**Watch:** `$4201` is open-collector with a slow rising edge — the emulator won't punish you for
a missing pull-up, real hardware will.

### M7 — Real hardware ← *first silicon for this entire repo*
Build the cable (Teensy/Pico + level shifter, **pull-up on `$4201` bit 6, port 1**). Real console
+ flashcart. Real keyboard via the existing `keyboard-test-rom` Pico protocol on **port 2**.
Decide: two Picos, or one multiplexing keyboard + network.
**Done when:** SNOS boots on silicon, takes real keystrokes, completes a TCP echo.
**Risk:** nothing in this tree has ever run on real hardware. Expect this milestone to find bugs
that no emulator surfaced. Do not schedule anything tight after it.

### M8 — IRC client
Port Contiki's IRC app onto uIP + the SNOS console.
**Done when:** join a channel from a SNES and send a message.

### M9 — Browser
Port Contiki's web browser — the first true web browser for an 8-bit system. It never buffers a
page: it parses HTML on the fly and stores only what's on screen, which is exactly the design a
64×28 console with 64 KB of app arena wants.
**Done when:** fetch and render a real page over the cable.

### M10 — Preemptive multitasking *(the crazy one; agreed, built and SHIPPED 2026-07-31 — steps 1-5 in `027cbd8..880b96c`+fg)*

The founding observation, sharpened by the review work: **the kill switch and the mid-syscall
deferral ARE the scheduler's two hard mechanisms, already shipped and tested.** The NMI can
yank any process at any instruction (kill switch); it already knows the one moment it must not
(saved D == SYS_DP — the deferral). A context switch is "kill switch, but resumable": when NMI
fires, the process's whole existence is its stack (PC/P pushed by the CPU, A/X/Y/D/DB by the
shim), so a PCB is essentially one saved stack pointer, and switching is store S / load S / rti
— the same KSP trick app_exec has used since M3, made plural. One process runs one syscall to
completion before any switch, so the kernel is never re-entered and the single $1B00 syscall DP
stays correct with no locks.

Five steps, each shippable, suite green after each:

1. **Context-switch primitive**: PCB array (saved S + state), forge-an-NMI-frame spawn,
   round-robin at the NMI shim tail behind the existing deferral checks. Proven by two
   kernel-resident worker processes ping-ponging counters — both making progress despite
   neither ever returning is the assertion only preemption can pass.
2. **Blocking syscalls yield** — briefly demoted to "optimization" after step 1 (preemption
   prevents starvation for processes that never trap), then REINSTATED by a PC trace during
   step 5: a poll-loop process is mid-syscall at every NMI, so consume-on-defer starved
   everyone else forever. Landed as THREE switch points that together cover every shape:
   the NMI (outside syscalls; defers WITHOUT consuming), cop_ret (takes the pending switch
   at syscall exit, NMI-masked — both shims push identical frames, so a process abandoned
   mid-pop resumes correctly), and proc_switch_now (offers the CPU from inside blocking
   waits via kbd_idle — without it, the shell blocked at its own prompt starves every
   background process).
3. **Bank $00 carving**: per-process stack+DP slots. No app relinking — Calypsi addresses
   stack and DP through S and D, which are per-process registers; only the assignments move.
4. **Arena slots — the one real problem, solved the house way.** Bank $7F data is linked at
   absolute addresses, so co-running apps must occupy disjoint regions, and copy-swapping
   64 KB is half a second — dead. Rule: **measured need, declared placement, build-time
   enforcement** (agreed with Ricardo 2026-07-31). The linker map is the truth about size;
   `APP_ARENA_*` in the Makefile declares placement exactly like `APP_BANK_*` does for ROM;
   the build FAILS if an app outgrows its slot (the LINK_FRAME%16 discipline); the slot goes
   in the app header so the kernel refuses an overlapping spawn — a deterministic refusal at
   launch, never a corruption at runtime. MultiFinder's model, and it fit a Mac Plus.
   Foreground-only apps may share a region (edit/basic); vnc (48 KB shadow) is honestly
   exclusive; irc(~33K)+edit(~13K) co-fit, which is the demo pairing by construction.
5. **UX**: `run app &`, `jobs`, foreground-rotate hotkey, kill switch kills the foreground,
   per-process channel reaping. The PPU stays single-owner (gfx_owned, unchanged).

**Done when:** IRC stays connected in the background, flashing a notification, while `edit`
runs in the foreground. **Status: the machinery is proven** — test/bg.lua has a background
netecho answering over the link WHILE a foreground app runs, the by-name arena refusal,
kill+reap with the gateway seeing the close, and fg + keyboard-break handback. The literal
irc+edit pairing works by construction (disjoint regions A and B; irc is poll-driven and
muted-safe) and awaits a live session; and the "flashing notification" shipped
as SYS_NOTIFY + the kernel-owned attention cell (`c192c7a`): irc raises it on PMs and
word-bounded nick mentions, netecho on connections, test/bg.lua asserts it end to end.

### M11 — Real TCP/IP: the SNES gets an address ✅ *(SHIPPED 2026-08-01)*
Today the gateway *is* the network — it holds the sockets and the SNES holds a byte stream, so
the console has no address and nothing on the LAN can reach it except through something the
gateway is already doing. This milestone inverts that: **the SNES runs an IP stack and owns an
address**, and the adapter stops being a proxy and becomes a converter — framing the console's
packets onto the wire and back, and nothing more.

That changes what the machine *is*. `httpd` stops being "a thing the gateway forwards to" and
becomes a host on the network. Every application stops needing the gateway to understand its
protocol, which is the thing that currently makes each new protocol a gateway change.

**Shape agreed with Ricardo, 2026-07-31 (brainstorm, nothing started):**

1. **slirp-mode host side** — the daemon does user-mode NAT (QEMU-style): it terminates the
   SNES's TCP itself and opens real sockets on its behalf. No root, no utun, no system changes,
   and the SNES's stack is still exercised end-to-end for real — only the far side of the NAT is
   synthetic. A utun/real-interface mode can come later as a demo flourish.
2. **Coexistence as scaffolding only** — IP gets its own link channel alongside the stream
   channels so irc/vnc/httpd keep working while the stack grows up, **but the end state is
   dropping the stream gateway entirely.** The bridge needs no changes at any point: it moves
   opaque frame payloads and always has.
3. **TLS: explicitly not a goal** (Ricardo: "back in the day this wouldn't exist anyway").
   The old open-problem paragraph is retired; anything that needs TLS can wait for an M12-style
   service or simply not exist, period-correctly.
4. Console side: uIP-class single-buffer stack, with the IP/TCP ones-complement checksum in
   netasm.s from day one — the ring_put lesson, applied in advance.

The image converter does not block dropping the gateway: M12 already turns it into an ordinary
HTTP service the SNES's own client calls.

**Addressing, DHCP and DNS (Ricardo's questions, answered 2026-07-31):**

- **No DHCP in slirp mode — by design.** Slirp networks are configured by convention (QEMU's
  guests have been `10.0.2.15` forever): the SNES's address, gateway and resolver are declared
  constants on its private two-node subnet. **DNS the SNES does itself** — real UDP queries to
  the daemon's resolver address; the daemon answers from the Mac's resolver. Real packets from
  the console, recursive grunt work on the host.
- **The SNES has its own IP** — real headers it composes and parses — but that address exists
  only on the SNES↔daemon link. Externally its traffic wears the host's address (NAT, exactly
  like a QEMU guest); inbound services ride port-forwards the daemon opens.
- **On real hardware the stack never changes; only the far end does.** Tier 1: the Pico is a
  dumb wire to the Mac, same daemon. Tier 2: the Pico W *becomes* the slirp box — DHCP happens
  but the PICO does it, for itself, joining WiFi like any gadget; the SNES stays static behind
  its NAT and the Mac exits the picture entirely. Tier 3 (the dream): wired ethernet on the
  Pico (W5500 — WiFi-station bridging is messy, wired is clean), the SNES gets a REAL LAN
  address, and **that** is the day DHCP becomes worth implementing on-console — cheap by then,
  it is just UDP and the stack already speaks UDP for DNS. Router table says "SNES";
  done-when satisfied with zero Mac involvement.
- (`telnetd` was filed here 2026-07-31; extracted same day to its own item below — it does
  not need M11 and should not wait for it.)

**Shipped, and what it actually looks like.** Six steps, one per commit, on the
`m11-tcpip` branch:

1. **`LINK_CH_IP` + first light.** A raw-IP channel alongside the stream ones,
   datagrams length-prefixed inside it, `kernel/ip.c` parsing and composing
   IPv4, `ip_sum` in netasm.s from day one. Capability is negotiated: HELLO
   version 2 means "I do NAT", asked unprompted at boot, so a v1 gateway never
   sees an IP record.
2. **uIP 1.0 vendored** (`kernel/uip/`, BSD-3) and running. Compiled clean at
   -O0 with no source changes; ICMP echo became its job, and `net status`
   prints its counters — which is also how the tests prove the STACK answered
   rather than something else.
3. **DNS on the console.** `resolve NAME` and `ping NAME`: the query is built
   and the answer parsed on the 65816, compression pointers and all. The
   daemon is only the resolver's address.
4. **`SYS_NET_CONNECT` re-backed.** Same call, same channel, same states —
   uIP's SYN underneath. Applications did not change.
5. **`SYS_NET_LISTEN` re-backed** (`C2S_IPFWD`): uIP binds the port, the
   daemon holds a real socket, and `httpd` serves a page to a connection
   opened from outside the NAT — twice on the same port, because a server is
   a loop.
6. Cutover, docs, and this rewrite.

The host half is **`snes/tools/slirp.py`**: user-mode NAT, no root, no utun, no
change to anyone's routing table. macOS grants unprivileged ICMP datagram
sockets, so even `ping` to a real host works.

**Proven end to end on 2026-08-01**, real gateway + real NAT + real internet:
`resolve example.com` → 104.20.23.154, `ping 8.8.8.8` → 10-13 frame round
trips, `debug send example.com:80` → 407 bytes of a real Cloudflare response.
Every header on the console's side was composed by the 65816.

**Done when — restated, because the original was the wrong milestone's test.**
It said "the SNES answers a ping from another machine on the LAN at its own
address". Behind slirp NAT nothing on the LAN can address the console at all,
BY DESIGN — that sentence describes tier 3 (wired W5500), where the console
holds a real LAN address. M11's own bar, and what is met: **the console
composes and parses its own IP/TCP, resolves its own names, reaches the real
internet through the NAT, and serves `httpd` to a connection from outside
it.** Tier 3 keeps the original sentence, unchanged, as its done-when.

**Known, and RESOLVED at the cutover.** The resolver's one-frame race
(between an answer arriving and the asker reading it, a second lookup could
overwrite it) got the request-tag fix the original note predicted:
`ip_resolve` hands back a per-lookup tag, a superseded asker reads FAIL
rather than somebody else's address, and a stale release is a no-op instead
of a theft — so the owned-until-released failure mode (one forgotten release
locking the resolver for the boot) cannot come back. The six-second watchdog
stays. Same round: a pending by-name connect whose NAME is unusable now
fails its channel instead of wedging every later channel's connect.

Still known, still by design: uIP's listen list has no room for a channel
number, so a second connection to a bound port is refused (RST) rather than
queued while one is being served; the server loops back to LISTEN between
requests, so the refusal window is one request long.

**The cutover (2026-08-01, the milestone's coda).** The coexistence
scaffolding was scaffolding after all: with the stack proven, the stream
gateway protocol was deleted whole, on both sides, one review-and-regression
pass after M11 shipped. The control channel keeps three verbs (HELLO,
IPFWD/IPUNFWD, plus ERR for a forward that will not bind); everything else —
FETCH, CONNECT, LISTEN, CLOSE, HANGUP, S2C_OPEN/EOF, the HTTP status and
image flag — is retired, numbers reserved. Consequences worth naming:

- **`get` became the real thing**: an HTTP/1.0 client composing its request
  and parsing the status line on the 65816. https is refused with its reason
  (TLS is not a goal, ever); `file://` and in-flight image conversion died
  with the gateway brain. A ready-made .IMG downloads over plain HTTP and
  `view` shows it — M12's shape, arrived early. `net test` rides the same
  path. SYS_NET_FETCH retired at ABI 12.
- **The NMI got lighter**: build_frame packs control + IP records only, and
  the channel rings left the NMI entirely (uIP fills, the app drains, both
  foreground) — the ch_reuse mark/flag handoff and the per-channel in-flight
  accounting simply ceased to exist.
- **uIP 1.0 cannot half-close** (no CLOSE_WAIT: a FIN in ESTABLISHED FINs
  back and kills both directions), so a client that half-closes right after
  its request races the server's reply. Found by the migrated netecho test,
  real on the live path too. The daemon absorbs it for INBOUND forwards —
  the external FIN is held until the console's replies go quiet for 0.75 s
  (slirp.py FIN_GRACE) — while outbound flows keep prompt EOF, because
  `get`'s end-of-body IS the far server's FIN.
- **The test rig consolidated**: test/gwip.lua is one hermetic slirp box
  (link transport + independent TCP/DNS/ICMP peer, checksums verified in
  Lua) and net/httpd/irc/vnc/bg/telnetd/ip all ride it.

uIP remains one-segment stop-and-wait, so throughput per connection is ~MSS
a round trip — far above what the emulated link delivers, and a known lever
if M7's cable ever makes it the bottleneck.

### Backlog — parked deliberately, with the reason *(agreed with Ricardo 2026-07-30)*
Not "someday" items: each was considered, costed, and put down for a stated reason. The task list
holds only actionable work; this is the durable record.

*(Triage 2026-07-31, with Ricardo's "check if anything is worth doing; if not, drop it": each item
below was re-judged DONE, DROPPED-with-reason, or left for M7's measurement campaign to resurrect
on evidence. Dropped means dropped — M7 re-derives its own priorities from real-cable numbers, and
a stale to-do list would only argue with those measurements.)*

- ~~`build_frame`~~ — **superseded**: the pad-tracking fix (`2d206b4`) removed ~4,200 of its
  ~4,500 cycles; NMI mean fell 56.4 → 36.6 scanlines. What remains is proportional to real
  payload, which is the honest cost of sending it.
- ~~DMA coalescing in the write window~~ — **DROPPED 2026-07-31.** vnc's owner verdict is "can't
  really complain about performance", the change risks the margin the flicker fix rests on, and
  the one measured cost it would address (the content-dependent backpressure stall) is workload
  luck, not a regression. If M7's faster link makes paint the wall again, THAT measurement
  reopens this on its own evidence.
- **`LINK_FRAME` 128 → 256** — 128 shipped (`9a68863`). ~~256 is a data bug~~ **RE-TESTED
  2026-07-31: the bug no longer exists.** With every FRAME constant flipped to 256 (link.h plus
  the seven derived copies in the test gateways, gateway.py and netbridge.lua), the ENTIRE suite
  passes — net, pager, irc, the host bridge test, and test-vnc, drop/corrupt sub-tests included.
  The old failure was recorded before the poll split, the MVN ring/frame copies, the ack-release
  fixes and build_frame's pad tracking; one of those rewrites fixed it in passing, and the
  archaeology of which was judged not worth a bisect. What M7 needed is now proven: **the frame
  size is one constant, and flipping it is safe.** Still shipped at 128 because 256 buys nothing
  in the emulator (round-trip-bound) — re-measure on the real cable where payload/exchange goes
  linear again. The mailbox reservation ($7EE000, 4 KB) holds to FRAME=2043; the record length
  byte caps a single record at 255, which the greedy packer respects by construction up to
  FRAME=261 — past THAT, records simply split, so no cliff exists there either.
- ~~Sliding window~~ — **DROPPED 2026-07-31.** The 2-poll floor is already reached with
  stop-and-wait, so there is nothing here for the emulator; the real cable changes the regime
  entirely and M7's measurements decide from scratch.
- **`apps/web/web.c` has the signed-constant bug** — ~~parked~~ **executed 2026-07-31
  (`540fe0a`): `web` deleted whole**, with `browser.py`, `C2S_WEB`, the ABI-8 syscalls and the
  `render:` scheme, in one commit because gateway.py imported browser.py unconditionally.

*(Added after the 2026-07-31 review waves, same parked-with-reason discipline:)*

- ~~`net_write` + `build_frame`'s TX copy per-byte~~ — **DROPPED 2026-07-31.** Send volumes are
  tiny on every current workload; if M7 or a real serving load makes TX measurable, the ring_put
  playbook (measure, then MVN) is written down and takes an hour.
- **Shared `libsnos` for the apps** — **the mechanical half executed 2026-07-31; the picker/PPU
  half DROPPED same day** (behavioural, each copy subtly fitted to its app; the duplication is
  real but the merge risk exceeds the ~150-line win):
  `apps/lib/snoslib.{c,h}` (compiled per app by buildapp.sh — separate links, no shared address
  space) now owns the blocking send-drain, the simple open-wait with deadline+cancel, u32
  decimal printing, port parsing and host:port defaulting; get/httpd/netecho/irc/vnc migrated,
  −184/+56 lines, 14/14 green. **Kept out on purpose:** `get`'s open-wait (its timeout
  forensics ARE the feature — the others should someday adopt it, not the reverse), netecho's
  echo drain (it cancels mid-send, deliberately), edit/basic digit loops (different sinks), the
  file-picker UI and the vnc/view PPU boilerplate (behavioural; still parked). Any future shared
  code must obey the no-initialised-writable-statics rule that bit `basic`'s RND.
- ~~`netbridge.lua` still has zero test coverage~~ — **done (`795399d`)**: `test/host/bridge_test.py` covers exactly this shape and runs first in `make test`. Original text: — `make test` speaks the mailbox directly. The
  shape of the missing test: a host-side pytest driving `gateway.py` against a scripted peer that
  mimics the bridge's *file* discipline (truncate-at-start, frame-boundary joins, shrink-detect
  restart, newest-frame handover), asserting a HELLO round trip survives both a gateway restart
  and a bridge restart. Needs UP/DOWN paths to become overridable first. The Mesen callback-order
  facts stay documented-as-measured; no headless test can reach them.
- ~~`con_init`'s remaining 189 ms~~ — **DROPPED 2026-07-31.** Already 5.8× better than it was;
  nobody has noticed 189 ms and nobody is going to.

### Housekeeping — the gateway needs a clean-up *(agreed 2026-07-29, not urgent)*
`gateway.py` has grown by accretion — HTTP fetch, image conversion, a `render:` scheme, a held-
open browser, listen/accept, and now a plain TCP path for VNC — and the seams show. Known debt:

- ~~`render:` is a testing back door~~ — **gone with `web` (`540fe0a`)**, along with browser.py,
  headless Chrome, `C2S_WEB` and the ABI-8 syscalls.
- ~~`SYS_NET_IMAGE` / `C2S_IMAGE`~~ — **removed 2026-07-31**: kernel, trap, stub, gateway and
  the test-gateway dispatch all gone; $65 and wire 0x03 reserved. Image coverage now rides the
  live Content-Type path exclusively, which is the path users actually exercise.
- ~~teardown written four times~~ — **mostly unified 2026-07-31 (`458272d`)**: one `teardown()`
  serves the F_SYN reset, the link restart and Ctrl-C, and the SNES-side `S2C_ERR`-on-freed guard
  landed with the review fixes (`963853f`). What remains of the "one channel model" idea is
  taste, not a known bug.

### Performance — link throughput *(queued 2026-07-29; items 1 and 2 done)*
The `vnc` client made the link's speed the headline problem. **Progress so far, all measured:**

| | 48 KB `vnc` screen | 8192 B via `file://` |
|---|---|---|
| baseline (2026-07-28) | ~80 s, 615 B/s | — |
| after item 1, the asm (`91ec6ab`) | 57.8 s, ~860 B/s | 571 frames, 440 polls, 9.52 s |
| after item 2, the bridge (`c64d8c3`) | 44.3 s | 536 frames, 300 polls, 8.93 s |
| after `LINK_FRAME` 128 (`9a68863`) | 44.3 s — *no change, see below* | 556 frames, 295 polls |
| after the phase fix (`19b0c55`) | 32.3 s | 561 frames, **295 polls** |
| after the asm transpose (`08fa773`) | 18.8 s | — |
| after splitting the poll (`27e964b`) | 15.8 s | 551 frames, **255 polls** |
| **after MVN'ing the receive ring (`656a966`)** | **11.8 s** | — |

**Total: ~80 s → 11.8 s.** Frame refusals are **0**, so the console is no longer the limit at all.

**Total: ~80 s → 18.8 s**, reproducible to the frame. **And the bottleneck has swapped back to the
wire.** The transpose fix cut polls 2156 → 1370 — far more than the paint tail alone, because the app
had been too busy to drain its receive ring — and of those 1370 the wire needs at least 818 (409 data
frames × the 2-poll floor). So the wire is **≥60% of the run and ≥76% of what a user actually waits
for** once the harness's 288-poll startup is excluded.

That is the third time these two have traded places. The order that actually happened was: link →
console → link, and each fix exposed the other side rather than the next item on any list.

`docs/performance.md` has the full method — read it before touching any of this, because it also
records an attempt that failed and why. **Quote the table above, not the 80 s figure**: that number
has been repeated in three places after it stopped being true.

**MEASURING THE BRIDGE REQUIRES A THROTTLE, and without one the answer inverts.** `--testrunner`
runs unthrottled at ~330 fps here, and `gateway.py`'s idle poll is a **wall-clock** 4 ms timer — so
at 330 fps the gateway looks 5× slower in emulated frames and becomes the bottleneck, swamping
anything the bridge does. Measured cold, the item-2 bridge looked like a *regression* (631 frames
against 616, and a deeper down-queue); throttled to 60 fps the same two builds gave 536 against 571,
reproducible to the frame. `SNOS_FPS=60` in the bench harness is the throttle. An idle keepalive
rate is likewise worthless as a proxy — the old bridge scored *twice* the new one on it, because
writing a frame into an unread mailbox every video frame counts as delivery and is actually loss.

**The goal is iteration speed, not silicon speed** (Ricardo, 2026-07-29: *"testing things in the
emulator is super slow. If we can get it closer to the real hardware, it would be a lot easier"*).
That is worth stating because it inverts how these items rank: work that only helps the emulator is
not a consolation prize here, it is the objective, and it is also the cheap and safe end of the
list. Real silicon is already ~33× the test rig with none of this done.

The link is **round-trip-bound, not bandwidth-bound**. Frames are already 100% full; 59% of polls
are retransmissions. The SNES puts something on the wire ~58 times a second and only ~10 of those
carry new data, because stop-and-wait sends nothing while an ack is outstanding and an ack takes
~5–6 emulated frames to come back through two files and two processes.

**Turning up the emulation speed is not the answer, and the reason is worth knowing.** It does
scale throughput — cost is denominated in emulated frames, so wall time is `frames ÷ (60 × speed)`
— but Ricardo observed more timeouts at speed, and that is mechanical rather than flaky. **Every
SNOS deadline is counted in frames** (`OPEN_DEADLINE 1800u` against `sys_frames()`, get.c:45) while
the gateway's work — DNS, connect, fetch, conversion — takes *wall-clock* seconds. At 3× a 30-second
deadline becomes a 10-second deadline while the work it bounds takes exactly as long as before. The
two clocks are only coupled because a real frame is 1/60 s, which is true on silicon and false in a
fast-forwarding emulator. So the frame-denominated deadlines are **correct for hardware** — do not
"fix" them — and fast-forward is a tool for bulk transfer tests, not a way to run the console.
Useless for anything interactive anyway (cursors, input timing).

**Which hops are artifacts, and which are real** (Ricardo, 2026-07-29: *"the bridge is a hack. The
actual stuff in the Pico will not work like that"* — correct, and it localises the problem):

| Hop | Emulator today | Real hardware (M7) |
|---|---|---|
| SNES ↔ bridge / Pico | Lua `endFrame`: **one exchange per video frame, 16.7 ms** | serial cable, SNES-clocked, **microseconds**; many exchanges per frame |
| bridge/Pico ↔ gateway | two files in `/tmp`, 250 Hz poll | WiFi/TCP to the laptop, ~1 ms |

The second hop is **already roughly faithful** — a process boundary with a 4 ms poll is a fair
stand-in for a WiFi round trip, and not worth much effort. The first is **off by three orders of
magnitude**, and that single artifact is the whole story.

**A measurement from the `vnc` flicker work (2026-07-29) reorders this list and caps what it can
buy.** Two findings, neither of them predicted:

- **`net_service()` in NMI runs from the top of vblank to scanline ~104 of the *next* frame** — over
  half of every frame's CPU, spent whenever the link is up. That is not a link problem, it is the
  headline performance bug of the whole system: every app pays it. It also means the console
  **cannot poll much faster than ~120 Hz no matter what the bridge does**, which invalidates the
  earlier note here that poll rate equals syscall rate. So the `MVN` copy is not merely a
  prerequisite for bigger frames — it is the dominant cost, and it gates item 1's value rather than
  the reverse.
- **`vnc`'s tile transpose costs ~1.3 frames per tile, ~17 s a screen.** Against 80 s on the wire
  the wire leads by only ~5×, so a link that were *free* would take the `vnc` loop from 80 s to
  ~17 s and stop. **The honest ceiling for this list is ~4–5× on `vnc`, not "real cable speed."**
  Text, `get` and IRC have no transpose and do get the full win.

Ordered by risk-adjusted value against the iteration-speed goal:

1. ~~**Make the frame copy cheap: `MVN` block move.**~~ **DONE 2026-07-29** (`91ec6ab`), and it did what the promotion predicted: `kernel/netasm.s` replaced the two mailbox copies with one `MVN` (~7.4 cycles/byte) and `checksum()` with an unrolled absolute,Y loop (~9.3), against ~103 before. **Mean NMI 144.8 → 41.3 scanlines, 55% → 15.8% of a frame; worst 498.8 → 116.8; re-entry 1 → 0.** Only the checksum half helps silicon — `link_wram.c`'s copies exist solely in the emulated backend. Two things follow, below. `net_service` copies frames in and out with a
   C loop at `-O0` (~40–100 cycles/byte) inside NMI. `MVN` does ~7. This is the same
   hand-written-65816-for-the-hot-loop pattern used across this repo, and it is the **prerequisite
   for both items below** — it is what makes a bigger frame affordable in the NMI budget.
2. ~~**Make the bridge answer within the frame.**~~ **DONE 2026-07-29** (`c64d8c3`), but *not* the
   way this item described. **Moving only the send half changed nothing — bit-identical, 571
   frames.** `net_service` is NMI-only so the console polls at 60 Hz regardless, and the seq-write
   callback fires *after* `link_wram_poll` has already copied RX out, so a frame delivered there
   waits for the next poll anyway. The win came from the **receive** half: load the mailbox on the
   console's *read* of `LINK_MB_HOST_SEQ`, the first thing a poll does. Also fixed two real defects
   found on the way — the bridge had **no flow control** (it wrote into an unread mailbox every
   video frame, the hole `docs/performance.md` blames for a windowed gateway stalling at 2 KB), and
   a startup **backlog of two frames** that became permanent latency once reads went 1:1.
   Two Mesen facts I had recorded wrongly here, both silent when wrong: **write callbacks fire
   BEFORE the write reaches memory**, and a 16-bit store is two bus writes **low byte first** — so
   change-detection on the pre-write value latches a stale high byte and every 256th poll runs the
   exchange twice. Register the low byte alone and act unconditionally.
   *(original text follows)* **Make the bridge answer within the frame.** A Mesen memory callback on the mailbox address
   instead of `endFrame`, so a reply does not wait for the next video frame. **This is a fidelity
   fix, not a cheat:** the 60 Hz ceiling is a property of `endFrame` alone — not of the protocol,
   the SNES, or the Pico — and the kernel already polls `net_service` from the main loop as well as
   NMI, so the console can drive faster than 60 Hz the moment something answers it. Removing the
   ceiling makes the rig *more* like the cable. Touches no kernel code, so it cannot panic. If it
   lands, the round trip becomes hop-2-bound (~1–8 ms), which puts stop-and-wait at 64 bytes in the
   8–32 KB/s range — i.e. **in the neighbourhood of the real cable's ~20 KB/s**, which is exactly
   the goal — but see the poll-rate cap above: without item 1 the console cannot ask often
   enough to collect it.
3. **`LINK_FRAME` 64 → 256.** 4× on paper, one constant on each of four sides, no protocol change.
   **The reason it panicked may now be gone:** the handler no longer overruns its own frame (worst
   116.8 of 262 scanlines), so NMI no longer re-enters itself, which was the suspected mechanism.
   Retry it before assuming the old failure still applies — but re-measure with `tools/nmiprof.lua`
   first, because `build_frame` is what scales with the constant and it is now the dominant cost.
   **Already tried naively and reverted** — the kernel dies with a smashed return address the
   moment the link is claimed, because the `-O0` copy overruns the NMI and the next NMI re-enters
   it on the same stack and direct page. 128 fails identically, so it is not a marginal budget.
   Do item 1 first.
3b. **Find the last poll per data frame.** The floor is now ~2 polls per data frame because `net.c`
   builds its TX frame *after* the poll, so the ack for a frame received at poll *k* does not leave
   until *k+1*. `get` measures 2.19 (essentially the floor); **`vnc` measures ~3**, so one poll is
   probably still recoverable in `net.c`/`gateway.py`. It is **not** in the bridge. At 60 Hz the
   2-poll floor caps the link near 1800 B/s, so this is worth finding before reaching for item 4.

4. **Stop-and-wait → sliding window.** The larger win of the two protocol changes, and the larger
   change: it needs item 1 *and* a mailbox deeper than one frame per direction, which means the
   WRAM layout, both endpoints, the Lua bridge and the gateway all move together.

**Where the remaining 9,399 cycles go — MEASURED 2026-07-30**, by marker timestamps at
`snos_nmi` / `net_service` / `link_wram_poll` / `build_frame` / the `rti`:

| span | cycles | scanlines | share |
|---|---|---|---|
| stub prologue → `net_service` | 941 | 4.1 | 10% |
| `net_service` → `link_wram_poll` | 333 | 1.5 | 3.5% |
| **`link_wram_poll` → `build_frame`** | **3,767** | 16.6 | **40%** |
| **`build_frame` → `rti`** | **4,523** | 19.9 | **48%** |
| total | 9,399 | 41.3 | |

**An earlier note here put `build_frame` at 6,000–8,000 cycles by subtracting from a histogram. It
is ~4,523 including the stub epilogue — the inference was ~50% high**, which is exactly why it was
labelled as inference. The receive side is the surprise: the two `MVN` copies plus the rx checksum
are only ~1,545 cycles of that 3,767, so ~2,200 sit in `net_service`'s own C — ack processing,
`rx_resync`, delivery and channel dispatch — none of which anyone had suspected.

Method, so it can be repeated: `build_frame` is `static` and has no symbol, so make it non-static
temporarily and **re-read every address from `build/snos.map` afterwards** — `net_service` and
`link_wram_poll` both moved when its linkage changed.

**THE END-TO-END PATH IS ROUND-TRIP-BOUND, AND FRAME SIZE DOES NOT TOUCH IT — measured 2026-07-30.**
This is the most important number in this section and it reorders what is left.

A 48 KB `vnc` screen through the real bridge, real `gateway.py` and a real VNC server takes
**44.33 s / 2660 frames at `LINK_FRAME` 64 — and 44.33 s / 2660 frames at 128. Bit-identical.**

| | hermetic `test-vnc` | end-to-end, real bridge |
|---|---|---|
| 64 → 128 | 10427 → 6271 frames (**1.66×**) | 2660 → 2660 frames (**1.00×**) |
| what binds it | frame size | the round trip |

**Do not conflate those two harnesses.** The hermetic one has no file hop and no second process, so it
is frame-size-bound and shows the full 1.66×; the real one is round-trip-bound and shows nothing. A
1.66× measured on `test-vnc` is *not* a user-visible speed-up, and this table exists because I nearly
reported it as one.

**MEASURED HOP BY HOP 2026-07-30, and it is a phase bug, not a bandwidth one.** Two premises in the
paragraph above were wrong and the trace killed both:

- **Frames are 97% full, not 13.8%.** 409 distinct data frames carry 49,268 bytes — **120.5 B of a
  124-byte capacity**. The 17.1 B/poll figure is 49,268 ÷ 2876 polls, and the gap is not packing: the
  gateway puts 2163 data frames on the wire for 409 distinct ones, **sending each 5.3 times**.
- **`next_out()`'s stop-and-wait costs no latency at all.** Reading the ack to writing the next fresh
  frame is **20 µs**. It generates the retransmissions; it does not delay anything. My "the gateway is
  stop-and-wait on its own send side, therefore that is the cost" was a plausible mechanism attached
  to a real symptom, which is the worst kind of wrong.

Where the 2876 polls actually go (polls are one per video frame; 16.6441 ms/frame = 60.081 fps,
regressed independently of the harness clock):

| phase | polls | s |
|---|---|---|
| before the command is typed (harness waits for frame 400) | 288 | 4.80 |
| command → first framebuffer byte | 31 | 0.52 |
| **bulk transfer of the 48 KB** | **2153** | **35.88** |
| after the last data frame — console still painting, gateway idle | 404 | 6.73 |

Inside the transfer: 405 fresh frames, **1746 retransmits**. Strip three console stalls and the cycle
is exactly **4.00 polls per data frame**:

| poll | what happens | |
|---|---|---|
| k+0 | fresh frame written into the mailbox and **overwritten unread** | +1 |
| k+1 | the *retransmission* is the copy the console actually takes | 1 |
| k+2 | `net.c` builds TX after the poll, so the ack leaves now | +1 *(known)* |
| k+3 | the reply sits one frame deep in the DOWN file | +1 |

**406 of 409 fresh data frames, and 50.0% of all deliveries, are never read.** The console reads
`LINK_MB_HOST_SEQ` twice on one poll and once on the next in strict alternation, while the bridge
writes a new frame every poll regardless — and `rx_pending`, which exists to stop precisely this, is
**cleared unconditionally in `exchange()`**. That is a bug in `c64d8c3`.

**It locks on one event.** At video frame 494 two polls occurred in one frame (`net_service` runs from
NMI *and* the main loop), the gateway wrote two DOWN frames 60 µs apart, and poll 310 found the file
empty — the transfer's only starved poll. The gateway writes one DOWN per UP and the bridge consumes
at most one per poll, so **the queue ratchets 0 → 1 and can never drain**; the odd poll flipped the
parity, and a 4-poll cycle preserves parity. Before poll 310 fresh frames were consumed on arrival;
from 321 on, every one was destroyed.

**The control run is what makes this conclusive.** `get file://` on the same bridge, gateway and
instruments: **zero starvation, delivery lag 1 throughout, 67 of 68 fresh frames consumed on arrival,
modal fresh-to-fresh gap exactly 2 polls.** The 2-poll floor is reachable with this exact code.
`vnc`'s 4 is the ratchet, not the protocol.

**The named suspects were all measurably innocent:** the two file hops cost **18 µs** total, and the
gateway's 4 ms sleep contributes 0 ms median / 2.3 ms p95 of detection lag — under 7% of a 66.6 ms
round trip. Every extra poll is "wait for the next video frame".

**FIXED 2026-07-30 (`19b0c55`) — and the cycle is now at the protocol floor.** The old hook was on the
console's read of `HOST_SEQ`, which cannot work: the bump it triggers lands *after* the read that
triggered it has taken its value, so the frame stays invisible until the next poll. Hooking the read of
**`MAGIC`** instead — `net_up()` → `link_wram_present()`, several instructions earlier in the same poll
— puts the frame and the bumped counter in memory before `link_wram_poll` looks. That also makes
clearing `rx_pending` in `exchange()` genuinely correct: a poll that reaches the `SNES_SEQ` write has
certainly copied the mailbox. The same inference at the old hook was false, which *was* the bug.

The ratchet is fixed by handing over the **newest** complete frame and dropping the ones behind it,
which is safe *because of* stop-and-wait — the gateway builds no new data frame until the last is
acked, so at most one queued frame can carry unseen payload. Tested by injecting a refusal on 1 poll
in 7: delivery lag stayed at 1 throughout, where before a single such event cost a poll on every
exchange for the rest of the session.

| | before | after |
|---|---|---|
| 48 KB screen | 2660 frames, 44.33 s | **1940 frames, 32.33 s** (1.37×) |
| steady-state cycle | 4.00 polls/data frame | **2.00 — the floor** |
| deliveries never read | 1437 of 2873 (50.0%) | **0 of 2154** |
| delivery lag | 2 polls on 2565 | **1 poll on all 2154** |

`get` control unchanged at 295 polls, which is the guard against a fix that only helps by luck.
A latent bug was also caught: the skip-to-end now rounds **up** to a frame boundary, where joining the
stream mid-append misframed every frame for the rest of a session — a link that is up and carries
garbage.

**Mesen callback semantics, now pinned by measurement rather than assumption** (all three were guessed
wrongly at least once): a **write** callback fires *before* the write reaches memory; a **read**
callback fires *after* the value the CPU gets has been taken; a callback's own `emu.write` *is* visible
to a later CPU read in the same video frame; and Lua reads do **not** re-enter memory callbacks.

**THE STALL HYPOTHESIS WAS WRONG, and so was the inference I drew from its apparent fix.** I recorded
that three stalls (607 polls, 441 resends of one frame) looked like `frame_fits()` refusing a full
receive ring, and later that the asm transpose had cured them because polls fell 2156 → 1370.
Instrumented properly with a `net_stat_refused` counter (2026-07-30), **both claims fail**:

- Current build, full screen: **refused = 0**. The exact historical configuration — baseline kernel
  with `apps/vnc/vnc.c` at `08fa773^` — also gives **refused = 0**, and the stalls do not reproduce at
  all. So the ring-space mechanism is measurably not what happened, and what did remains unknown; it
  may have been environmental, since the container is a live desktop rather than fixed content.
- **The 2156 → 1370 drop was *entirely* the paint tail**, not the app finally draining:

| | pre-command | transfer | paint tail | cycle |
|---|---|---|---|---|
| old kernel + old transpose | 291 | 837 | **932** | 2.00 |
| old kernel + fast transpose | 291 | 840 | **238** | 2.00 |

  The transfer phase is unchanged to within noise. And running the *old* transpose against the *new*
  kernel gives **refused = 213**, the identical count the fast transpose gives — so the drain rate does
  not depend on the transpose at all. **`vnc`'s receive path and its paint path are decoupled**, which
  is the thing the hypothesis got backwards.

**Backpressure is FIXED (`656a966`), after two wrong explanations.** Both were tested cheaply before
anything was built, which is the only reason the third one was found:

- *The ring is too small* — doubling `NET_RING` 1024 → 2048 moved refusals 213 → **211** (0.9%).
  Reverted; 8 KB of WRAM for 1% is not a trade.
- *The paint blocks reads* — a trace of the refusal counter against paint progress shows **every
  refusal at `painted=0`**, before a tile is drawn, stopping the moment painting starts.

The real cause: both ring copies were per-byte C loops, and `ring_put` measured **234 cycles per
byte** — worse than the mailbox loop `link_mb_move` replaced, because `ring[ch][ring_head[ch]]`
rebuilds a 24-bit address from a 2-D array and a volatile index for every byte. `MVN` took it to
1,519 cycles a call (19×) and `net_read` to 350 (7.9×). **Refusals 213 → 0, screen 15.83 → 11.83 s.**

**The console is no longer the limit.** 947 host frames carried 49,152 bytes — about **52 bytes of a
124-byte payload**. The far side is not filling frames, and that is what the remaining time waits on.
Worth measuring in `gateway.py` before assuming anything: the last three explanations offered here
were wrong, and each was refuted by a cheaper test than the fix would have been.

**MEASURED 2026-07-31 (`SNOS_GW_STATS=1` in gateway.py): the fill number was an accounting
artifact, making it the fourth wrong explanation in this section.** The gateway classifies every
DOWN write: a full vnc screen is **409 data frames at 120.4 of 124 payload bytes each** —
97% full, four frames off the theoretical minimum of 403. The "52 bytes" figure was total bytes
divided by total *exchanges*, and most exchanges carry no data at all: they are empty polls plus
**207 retransmissions of a single frame** (`SNOS_GW_STATS=2` timestamps them: all 207 are seq 144,
one per poll for 3.4 s mid-transfer). The SNES-side counter agrees exactly — `refused=207` — so the
stall is **ring backpressure**: one window where the app stops draining, `frame_fits` refuses, and
stop-and-wait marks time.

Two qualifications, both measured the same night:

- **Not a regression, and content-dependent.** The pre-review baseline `57aac0e`, rebuilt in a
  worktree and run against the same desktop, reproduces the identical numbers (409/49,244/207) —
  which both exonerates the review-wave changes and shows they cost nothing here. The earlier
  celebrated `refused=0` run was a property of that day's desktop content, not of the fix: the ring
  MVN work removed the *per-byte cost*, but a content burst that outruns the paint still fills the
  1 KB ring. No plausible ring absorbs a 3.4 s stall (~25 KB at link rate) — the drain rate is the
  lever, i.e. the paint tail (DMA coalescing, backlog), or the sliding window.
- **The 768-tile DONE threshold is content-dependent too**: today's desktop peaks at ~738 non-zero
  tiles, so the old benchmark times out at a painted screen. Use ~700, or compare transfer-side
  numbers (`fresh_data` / `payload` / `resend`), which do not depend on what the desktop shows.

**Where the line actually falls** (Ricardo, 2026-07-29: *"as long as you keep the frame mechanism,
you don't really need to think too much about it"* — right, and it is a cleaner rule than the one
this section originally had). **The framing, seq/ack and stop-and-wait are the contract; the file
relay is not.** Anything that speeds up the relay while leaving the frame mechanism untouched hides
no protocol bug, because the protocol is not involved — item 1 is exactly this, and the earlier note
calling it a "cheat held in reserve" was wrong.

What stays dead is the version that breaks the contract: handing over a whole screen in one shot and
skipping the framing. That is worth ~50× and it is not worth having, because the bugs it hides are
precisely the ones that have actually bitten us — the `LINK_FRAME` panic and every flow-control
fault were properties of the real framing. **Keep the frames, speed up everything else.**

**Keep the perspective:** all of these numbers measure the *emulator path*. The real cable is
synchronous, console-clocked, and has no file bridge, no 1-deep mailbox and no 60 Hz ceiling —
~20 KB/s is the documented ballpark, about **33× what the test rig gets**. So M7 on real silicon
is faster than any of this, and items 3 and 4 are worth doing on their own merits rather than
because the demo depends on them.

### telnetd — remote shell into the console *(SHIPPED 2026-07-31, `3ac26d0`, v2 direct)*
Built straight to v2; v1 was skipped because the clear question answered itself. ABI 10 adds
SYS_CONROW (read a console row) and SYS_KBDPUSH (inject a keystroke), neither
foreground-gated. 4 KB arena at `7f4000`, bank `$D1`. `test/telnetd.lua` asserts the mirror
carries the screen as it STANDS on connect (banner printed before the connection existed —
only a state diff can do that), that socket keystrokes reach the shell and their output
reaches both screens, and that it all survives a foreground app.

**Follow-up, same day (`44987cf`): SYS_CONDIRTY, ABI 11.** The kernel already tracks
changed rows for its own VRAM flush, so telnetd asks for that mask instead of re-reading
28 rows to discover none changed. It polls every frame now: an idle screen costs one trap
returning an empty mask, a keystroke echo reads one row, and mirror latency drops from
up-to-100ms to next-frame. The mask is read-and-clear (one consumer — the exclusive arena
slot already forbids a second telnetd) and a scroll honestly reports every row, since every
row's content moved. The test additionally asserts an idle screen sends exactly zero bytes,
the regression a broken read-and-clear would produce. Building this shook loose the
hardware-ALU hazard (`7832cdd`, see §5 and docs/calypsi-interrupt-hazard.md).

Original plan, for the record:
- **v1, stream mirror (~150 lines, netecho-shaped):** a background app bridging a listening
  socket to the EXISTING mailbox rings — input's third backend is a byte ring, console output
  already tees into one. Remote and real console mirror both ways. Limitation understood:
  `clear` and positioned writes are invisible remotely (the ring carries printed characters,
  not screen operations) — the remote sees the conversation, not the screen.
- **v2, screen-faithful:** every telnet client is a VT100. Tee the console's three choke
  points (con_putc / con_putat / con_clear) into a small record ring; telnetd translates
  records to ANSI (`ESC[2J` clear, `ESC[r;cH` position, `ESC[7m` inverse). The remote
  terminal becomes a pixel-faithful 64x28 mirror INCLUDING full-screen apps — remote edit,
  remote irc. The SNES never emulates a terminal; it narrates what it did and the far
  terminal draws. Post-M11 the socket becomes a listener on the console's own stack.

### M12 — Services, not proxies *(follows M11; half-arrived with the cutover)*
Once the console has its own address, the gateway stops being a shim everything flows through
and becomes **an ordinary host offering a service**. The relationship inverts: today every new
protocol is a gateway change, because the gateway is on the path for every byte. After M11, IRC,
plain HTTP, gopher, `httpd` and `netecho` are all direct, and the only thing still worth asking a
helper for is what the 65816 genuinely cannot do.

*(Status after the M11 cutover: the "stop being a shim" half is DONE — the daemon understands no
protocol at all and `get` speaks its own HTTP. What remains of M12 is the render service itself
and the on-console GIF decode below.)*

Which is rendering. There is no version of this machine that lays out modern HTML — Wikipedia's
SNES article is 735 KB of markup against a 64 KB app arena — so the render service is permanent.
It just stops being privileged:

```
GET /render?url=news.ycombinator.com&w=512&h=384   ->   .IMG tiles
```

A normal HTTP request, from the SNES's own HTTP client, to something that is now just a website.
It can live on another machine, be shared, be restarted without touching the console, or be
swapped for a different implementation. `web` becomes a client rather than a thing wired to a
daemon.

**Images are where it gets interesting**, because one format is genuinely within reach:

- **JPEG — no.** Huffman plus an IDCT over ~900 blocks, on a CPU whose only multiplier is an
  8×8→16 unsigned unit at `$4202`. And that is before the expensive part: quantising 57,000
  pixels to 256 colours.
- **PNG — marginal.** Inflate is slow but possible; it then hands back 24-bit pixels, so the
  quantisation cost lands anyway.
- **GIF — plausible, natively.** LZW is a table walk, and the image is **already palettised at
  ≤256 colours**. The output is indexed pixels plus a palette, which is very nearly Mode 3's 8bpp
  format already. No DCT, no inflate, and **no colour quantisation at all** — the hard part was
  done by whoever saved the file. BMP and PCX are trivial for the same reason.

That is a pleasingly period-correct answer: GIF is what the web ran on when a machine this size
was current, and it is the format this machine can still read for itself.

**Done when:** `web` fetches a rendered page over ordinary TCP from a service it did not have to
be specially wired to, and the console decodes a GIF without help.

### M13 — `mc`: the SNES is a Minecraft server *(planned with Ricardo 2026-08-01, nothing started)*

The dumbest thing ever, chosen with open eyes. A real, unmodified **Java Edition** client joins
a server whose every TCP segment was composed on a Super Nintendo, spawns into a small static
world, and walks around. Prior art brackets the ambition: a 1960s UNIVAC did **login only**
([farlow.dev 2026-04-17](https://farlow.dev/2026/04/17/running-a-minecraft-server-and-more-on-a-1960s-univac-computer),
derived from bareiron, PPP through a Pi); [bareiron](https://github.com/p2r3/bareiron) on an
ESP32 does real gameplay but on a CPU ~500× ours. We clear the UNIVAC's bar in step 2 and stop
well short of bareiron's — the 65816 doesn't need gameplay, it needs the one trick this
architecture is genuinely good at.

**The trick, and why this is feasible at 3.58 MHz:** don't compute packets — **bake them**.
Same insight the OS rests on ("load a program is a build-time decision"). A Python world-baker
generates the exact wire bytes at build time — status JSON, login sequence, Join Game with its
dimension-codec NBT, every chunk packet, light packets, keepalive/position templates — and packs
them into the app's ROM banks (an app is N contiguous banks; the header already says so). The
runtime is httpd with bigger canned responses: a VarInt-length framer that skips client packets
by length (the discipline every parser in this repo already has), a handshake→status|login→play
state machine, and a loop streaming ROM blobs into `net_write`. Runtime byte-splicing is limited
to: the player's name/UUID echoed into Login Success, teleport IDs, keepalive IDs.

**Decisions, locked 2026-08-01 (Ricardo confirmed all four):**

| Decision | Choice | Why |
|---|---|---|
| Edition / protocol | **Java, offline mode, protocol 754 (1.16.5)** | Plain TCP (Bedrock is RakNet/UDP + mandatory compression — fights uIP instead of riding it). Pre-1.19 = no chat signing; pre-1.20.5 = no configuration-phase registry dump (tens of KB before the world starts, painful at 600 B/s); palette-era chunks (1.13+) make a superflat section ~1–2 KB. bareiron stays open as the map of client expectations. |
| Compression | **Never** — Set Compression is simply never sent | zlib DEFLATE on a 65816 is where the idea dies; the protocol makes it optional and the server decides. |
| Encryption | **None** — offline mode | Same reason TLS is out, period-correctly. |
| Players | **One at a time** | uIP's listen serves one connection per port by design (the M11 known limit). A Minecraft server for exactly one friend. |
| World | **Static, prebaked, small** (~9×9 chunks, view distance 2) | A palette-era superflat chunk ≈ 1–3 KB with biomes+light; the whole world is well under 200 KB against ~3 MB of free cart. Sculpt something (a block-art SNES logo) rather than bare flatness — it costs ROM, which is the one thing we have. |
| Interactions | Ignore, except the cute one | Player digging gets a canned Block Update restoring the block: **the SNES puts the world back.** Static world enforced diegetically. |

**Steps, each shippable, suite green after each:**

1. **The baker, validated without the SNES.** `tools/mkmc.py` emits the baked blobs; a
   ~100-line host prober serves those exact bytes over a real socket to a real 1.16.5 client on
   the Mac. Proves every byte before the 65816 is involved — the protocol knowledge lives in
   Python, testable at desk speed. **Done when:** a real client logs into the Python-served
   bytes and walks the baked world. **DONE 2026-08-01 (`6c38c56` + one fix):** Ricardo's real
   1.16.5 client joined, walked, punched blocks that came back, chatted. One protocol gap on
   the first attempt — the biome codec's `depth`/`scale` floats — and the client's decoder
   NAMED the missing keys in its disconnect screen, which is the exact failure-locality the
   step existed to buy. 146 KB, 25 chunks, structural checks in `test-host` (mkmc_test.py).
2. **Login on the console** — the UNIVAC bar. App `mc` (netecho-shaped): VarInt framer,
   handshake/status/login states, blobs streamed from its own ROM banks, name/UUID splice.
   **Done when:** the real client sees the SNES in its server list (MOTD and all) and completes
   login to "Loading terrain"; a gwip canned-client transcript asserts the status JSON and the
   login sequence hermetically.
3. **The world.** Chunk streaming spawn-first at view distance 2, Update Light, position +
   teleport-confirm acceptance, keepalives on a frame timer (~600 frames), client packet drain.
   **Done when:** the real client spawns and walks around; the hermetic test asserts a scripted
   client receives the spawn chunks byte-identical to the baked blobs with keepalives on
   cadence.
4. **The demo dressing.** Join/chat events on the console screen (UTF-8 sanitized to the
   console's ASCII), chat FROM the SNES keyboard as System Chat, the dig-revert Block Update,
   `mc &` background + SYS_NOTIFY on join. **Done when:** chat round-trips and a punched block
   restores itself, asserted hermetically; a live session is recorded here.
5. **Docs**: gateway.md gets a "join it from your Mac" section (add server → the address `net
   status` prints, port 25565 forwarded via IPFWD like any listener); README status row.

**Known risks, named in advance:** the join burst vs client patience is the one empirical
unknown — ~27 KB of spawn chunks is ~45 s at the emulated link's ~600 B/s, brushing client
timeouts (mitigations: spawn-chunk-first order, keepalives from the first second, view distance
2; the real cable's ~20 KB/s makes it ~2 s, so M7 hardware is where this demo gets smooth).
Client fiddliness (brand/plugin channels, teleport confirms, position spam ~1 KB/s inbound to
drain) is discovered live against the real client, which is why step 1 front-loads it on the
host. Step 2 is the milestone's early exit: if the risks bite, login-only already matches the
UNIVAC.

---

## 5. Why kernel-owned NMI is the load-bearing decision

Everything from M6 onward depends on it. If apps owned NMI, every app would need to
cooperatively yield for the network to breathe — which means every app becomes network-aware,
and porting Contiki's apps stops being cheap.

With the kernel owning NMI: the network stack polls in VBlank *while an app runs*, apps stay
oblivious, and M10 is a scheduler drop-in rather than a rewrite. The cost is that apps may never
`SEI` for long or write `$4200` directly — which is exactly the kind of rule an app ABI exists
to impose.

The same ownership imposes a rule on our own build, found 2026-07-31: **no C code on this
machine may use the S-CPU hardware ALU (`$4202-$4217`)**. NMI is unmaskable, the vblank
auto-joypad poll destroys the result registers in hardware, and a handler cannot save them —
operands are write-only, results read-only. Calypsi's `--target=snes` inlined every C multiply
on exactly those registers behind a useless `SEI`, so the whole tree (kernel and apps) compiles
bare 65816 with soft `_Mul16`/`_Div16` instead (`7832cdd`). The full forensic story is in
docs/calypsi-interrupt-hazard.md.

---

## 6. What we're harvesting, and from where

| Need | Source | License |
|---|---|---|
| TCP/IP | uIP, from [Contiki 2.x](https://github.com/oliverschmidt/contiki) | BSD-3 |
| IRC client, web browser | Contiki apps, same tree | BSD-3 |
| BASIC | `../tinybasic/` (Stefan's BASIC), already vendored | (confirm at M5) |
| Console | `../snespdp11/snes/console.c` | ours |
| OSK generator | `../snesmac/snes/tools/mkkbd.py` | ours |
| Keyboard wire protocol | `../keyboard-test-rom/src/keyboard_rom.c` | ours |
| Mailbox stdio | `../snesmac/snes/calypsi/mailbox.{h,c}` | ours |
| Test suite protocol | `../emu8086/snes/test/suite.sh` | ours |
| Build skeleton | `../snespdp11/snes/{Makefile,calypsi/*}` | ours |
| Calypsi landmines | `snesmac/docs/INTERNALS.md:73-88,224`; `snespdp11/docs/M{3,4}-notes.md`; `snesmac/docs/PERF.md` | ours |

**Design references (read, don't port):** BS-X header + MCC + return-to-town contract
([superfamicom.org](https://wiki.superfamicom.org/bs-x-satellaview-header)); SFX-DOS
([RetroReversing](https://www.retroreversing.com/super-famicom-sfx-dos)); sd2snes `snes/main.a65`
handoff; Nintendo Power `$2400` loader; CP/M-65's relocatable-binary scheme; GS/OS OMF.

---

## 7. Risks → mitigations

| Risk | Mitigation |
|---|---|
| **Calypsi codegen bugs** (23 known from micropython-snes, plus ours) | `-O0` baseline; read micropython-snes's patch set at M0; host-side mirror of every non-hardware module so logic is debugged on the desktop first (the pattern every project here already uses) |
| **8 KB bank-$00 budget** blows up at M10 | Allocate it in M0 (§2) and *test* the boundaries — a Lua assert that no stack crosses its slot. Don't discover this at M10. |
| **Real hardware never tested** (whole repo) | Pull M7 earlier if anything smells; the M3 ROM is small enough to try on silicon opportunistically |
| **Two emulators from M6** | Keep the network driver behind one interface; everything else stays Mesen-only and CI-green |
| **Contiki apps assume cc65 + 64 KB flat** | They're being ported as *SNOS apps* linked with Calypsi, not as a Contiki port. Budget real time for M8/M9 — these are the long poles, not the kernel. |
| **Scope creep into "a real OS"** | The kill switch, the ABI version fields, and "kernel restores everything" are the only generality allowed before M10. |

---

## 8. Open questions for the build session

1. **App arena size** — is 64 K enough for the browser, or does it need to spill into `$7E2000`
   and displace kernel data? Decide at M9, but keep the split adjustable in `linker.scm`.
2. **SRAMFS format** — flat table vs FAT12 (we have `emu8086/apps/fatimg.py`). Flat is right
   for M4; revisit if the browser wants a cache.
3. **`tinybasic` licensing** — confirm before M5 so BASIC can ship in a public ROM.
4. **One Pico or two** at M7 (keyboard on port 2 + network on port 1, vs one multiplexed device).
5. **Does `COP` survive a Mesen savestate cleanly?** Untested. Verify at M2, cheap to check.
6. **Kill-switch chord** — START+SELECT is also a common in-app combo. Consider requiring a
   1-second hold, or L+R+START+SELECT.
