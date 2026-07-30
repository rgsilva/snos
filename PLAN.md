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
| App RAM | **Every app links to the same arena** (`$7F0000–$7FFFFF`) | Only one app runs at a time. Like DOS `.COM` at a fixed org. Per-app `linker.scm` differs *only* in ROM base ⇒ a Makefile template parameterised by bank number. |
| Exit | **`COP #0`** (exit code in A); kernel restores its saved `S` | Works from arbitrary call depth, which `RTL` cannot. `RTL` from the entry point also lands in a kernel trampoline, so trivial apps can use either. |
| Kill switch | **Kernel NMI handler force-exits on START+SELECT held ~1 s** | Answers "how do I get out of BASIC" *generally*, for every app including buggy ones. Non-negotiable — it's the only thing standing between a hung app and a power cycle. |
| Interrupts | **Kernel owns NMI unconditionally.** Apps hook via a RAM vector table, never by writing `$00:FFEA` | Vectors are fetched from bank `$00` = ROM, so they're baked at build time; indirection through RAM is how BS-X and sd2snes both do it. Kernel-owned NMI is *also* what makes background networking possible without cooperative yielding (§5). |
| Console | **`snespdp11/snes/console.c` verbatim** — Mode 5, 64×28, 8 px font | Already written, already debugged, standalone by design. **80 columns is impossible** (512/80 = 6.4 px — non-integer cell alignment; needs 1792 tiles vs a 1024 index limit; ~3 lo-res px/glyph). 64 is the hardware ceiling. |
| Input | **Abstraction from day 1**: `kbd_getchar()` → ASCII, three backends | OSK (M1), Lua test-feed (M1, drives CI), Pico USB bridge (M7). Never let the shell talk to `$4218` directly. |
| Network | **Controller port 1, synchronous serial**, byuu/near protocol | ~20 KB/s practical each way, full duplex free, ~$10–13 of parts, **no cartridge hardware**, and already emulated in bsnes. Keyboard moves to port 2. |
| Test harness | **Mesen `--testrunner` + WRAM mailbox at `$0100`** | Reuse `snesmac/snes/calypsi/mailbox.{h,c}` (bidirectional stdio ring) + `emu8086/snes/test/suite.sh` `RESULT PASS/FAIL` protocol wholesale. |

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

| Range | Size | Owner |
|---|---|---|
| `$0000–$00FF` | 256 B | Kernel direct page |
| `$0100–$01FF` | 256 B | **Test mailbox** (Mesen Lua polls it; same convention as snespdp11) |
| `$0200–$07FF` | 1.5 K | Kernel stack |
| `$0800–$08FF` | 256 B | App direct page |
| `$0900–$18FF` | 4 K | App stack |
| `$1900–$1FFF` | 1.75 K | Kernel hot globals, RAM vector table, syscall scratch |

At M10 the app DP+stack region (`$0800–$18FF`, 4.25 K) gets carved into 512 B per-process
slots → **8 processes**, or 12 if the kernel stack is trimmed.

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
| `$1E` | 2 | Header checksum |

### Exec contract

1. Kernel saves `S`, sets up app DP (`$0800`) and app `S` (`$18FF`), sets `DB` = app bank.
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

### M10 — Preemptive multitasking *(the crazy one)*
NMI-driven round-robin scheduler. Carve `$0800–$18FF` into 8 × 512 B process slots (§2). Context
switch = push A/X/Y/P/D/DB, `TSC` save, `TCS` load, pop — **under ~40 cycles**. Background
daemons (network poll, clock). Status bar showing live processes.
**Done when:** IRC stays connected in the background while BASIC runs in the foreground.
**Why it's plausible:** the SNES already has a 60 Hz preemption source, and the 65816's whole
context is 6 registers. The hard part isn't the switch — it's the 8 KB in §2.

### M11 — Real TCP/IP: the SNES gets an address
Today the gateway *is* the network — it holds the sockets and the SNES holds a byte stream, so
the console has no address and nothing on the LAN can reach it except through something the
gateway is already doing. This milestone inverts that: **the SNES runs an IP stack and owns an
address**, and the adapter stops being a proxy and becomes a converter — framing the console's
packets onto the wire and back, and nothing more.

That changes what the machine *is*. `httpd` stops being "a thing the gateway forwards to" and
becomes a host on the network. Every application stops needing the gateway to understand its
protocol, which is the thing that currently makes each new protocol a gateway change.

**TLS is the open problem, and it stays open on purpose.** A handshake on a 3.58 MHz 65816 runs
to minutes, so the console cannot terminate it and a converter has nothing to terminate it with.
The intended shape is a **proxy configured on the console** — an explicit tunnel the SNES sends
its TLS through, rather than a gateway silently doing it — which keeps the honesty of the
current design (nothing forges a certificate) while moving the decision to the console's own
configuration. Not designed yet. Deliberately not started.

**Done when:** the SNES answers a ping from another machine on the LAN at its own address.

### Housekeeping — the gateway needs a clean-up *(agreed 2026-07-29, not urgent)*
`gateway.py` has grown by accretion — HTTP fetch, image conversion, a `render:` scheme, a held-
open browser, listen/accept, and now a plain TCP path for VNC — and the seams show. Known debt:

- **`render:` is a testing back door** and should not survive as a public URL scheme. `web` is
  being retired in favour of `vnc`, so it can probably go entirely.
- **`SYS_NET_IMAGE` / `C2S_IMAGE` are dead** — the forced-conversion mechanism from before the
  gateway decided from Content-Type. Still wired through the kernel, the trap layer and the test
  gateway.
- **The browser (`tools/browser.py` + the `web` app + `C2S_WEB`) becomes dead weight** once `vnc`
  works, along with headless Chrome as a dependency.
- The daemon now has several kinds of channel (fetch, listen, connect, web) with different
  teardown rules, discovered one race at a time. They deserve one model rather than four.

**Not a refactor for its own sake:** every fault found on 2026-07-29 — a queue that outlived its
channel, a page closed under a running thread, an `S2C_ERR` landing on a freed channel — was a
teardown bug, and they were separate bugs only because teardown is written four times.

### Performance — link throughput *(queued 2026-07-29, next up)*
The `vnc` client made the link's speed the headline problem: a 48 KB screen takes ~80 seconds at
the measured **615 B/s**. `docs/performance.md` has the full measurement — read it before touching
any of this, because it also records an attempt that failed and why.

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

1. **Make the frame copy cheap: `MVN` block move.** *(promoted — it is the dominant cost, see above)* `net_service` copies frames in and out with a
   C loop at `-O0` (~40–100 cycles/byte) inside NMI. `MVN` does ~7. This is the same
   hand-written-65816-for-the-hot-loop pattern used across this repo, and it is the **prerequisite
   for both items below** — it is what makes a bigger frame affordable in the NMI budget.
2. **Make the bridge answer within the frame.** A Mesen memory callback on the mailbox address
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
   **Already tried naively and reverted** — the kernel dies with a smashed return address the
   moment the link is claimed, because the `-O0` copy overruns the NMI and the next NMI re-enters
   it on the same stack and direct page. 128 fails identically, so it is not a marginal budget.
   Do item 1 first.
4. **Stop-and-wait → sliding window.** The larger win of the two protocol changes, and the larger
   change: it needs item 1 *and* a mailbox deeper than one frame per direction, which means the
   WRAM layout, both endpoints, the Lua bridge and the gateway all move together.

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

### M12 — Services, not proxies *(follows M11)*
Once the console has its own address, the gateway stops being a shim everything flows through
and becomes **an ordinary host offering a service**. The relationship inverts: today every new
protocol is a gateway change, because the gateway is on the path for every byte. After M11, IRC,
plain HTTP, gopher, `httpd` and `netecho` are all direct, and the only thing still worth asking a
helper for is what the 65816 genuinely cannot do.

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


---

## 5. Why kernel-owned NMI is the load-bearing decision

Everything from M6 onward depends on it. If apps owned NMI, every app would need to
cooperatively yield for the network to breathe — which means every app becomes network-aware,
and porting Contiki's apps stops being cheap.

With the kernel owning NMI: the network stack polls in VBlank *while an app runs*, apps stay
oblivious, and M10 is a scheduler drop-in rather than a rewrite. The cost is that apps may never
`SEI` for long or write `$4200` directly — which is exactly the kind of rule an app ABI exists
to impose.

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
