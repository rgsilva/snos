# The Pico W adapter: keyboard + network on controller port 2

One Raspberry Pi Pico W on **controller port 2** carries both the network link
and the keyboard. Nothing above `link_recv()/link_send()` changes; the keyboard
is one more record channel on the same frames, feeding the same input ring the
Mesen harness types into. This is PLAN.md M7: tier 1 relays frames to the host
daemon over WiFi, while tier 2 makes the Pico the slirp box when no relay host
is configured.

Status (2026-09-29, five-wire revision): **running on silicon.** The `wire` test passes console → Pico at 60 frames/s with zero errors; Pico → console needed the open-drain DATA1 and the inverted read described in §2.2.
`pico/` builds to `snos_pico.uf2` (pico-sdk 2.1.1); `make LINK_HW=1` builds
the console side into `build-hw/snos.sfc`; `test-kbd` covers the keyboard
channel in the emulator; `pico/tests/link_test.py` checks the Pico's link
layer against `gateway.py` over 10,000 exchanges. Every voltage and timing
number below is a design value; the "first power-up" checklist at the end is
what turns them into measurements.

## 1. Port 2, and why

| | port 1 | port 2 |
|---|---|---|
| clock | `$4016` read | `$4017` read |
| latch | `$4016` write bit 0 | same line — **shared** |
| data 1 / data 2 | `$4016` read bits 0/1 | `$4017` read bits 0/1 |
| IOBit out / in | `$4201` bit 6 / `$4213` bit 6 | `$4201` bit 7 / `$4213` bit 7 |
| side effect of IOBit | none | falling edge latches PPU H/V counters (`$213C/D`) |

Port 2 leaves port 1 for the Everdrive menu and a real pad. The H/V latch side
effect is harmless: SNOS never reads the counters. The shared latch line is
fine too: pulsing it re-latches whatever pad sits in port 1, which changes
nothing the auto-joypad read does not already do every frame.

## 2. Connector pinout and the circuit

**Five wires.** The cable in hand carries pins 1-4 and 7 only, so the design
uses CLK, LATCH, DATA1, +5 V and GND and nothing else. IOBIT (pin 6) and
DATA2 (pin 5) are not used even if the console has them.

Looking into the **console's** socket, pins 1-4 are the square group, 5-7 the
rounded group (numbering as on the pad's plug, left to right):

| pin | signal | direction | role | Pico W |
|---|---|---|---|---|
| 1 | +5V | — | pull-up source for DATA1 only (§2.2); the Pico is USB-powered | 1 kΩ to pin 4 |
| 2 | CLK | SNES → Pico | the clock: one pulse per `$4017` read | GP2 via divider |
| 3 | LATCH | SNES → Pico | **console's data out**: `$4016` bit 0 | GP3 via divider |
| 4 | DATA1 | Pico → SNES | Pico's data out, open-drain: `$4017` bit 0, read inverted | GP5 via 330 Ω, 1 kΩ to +5 V |
| 5 | DATA2 | — | not in the cable | — |
| 6 | IOBIT | — | not in the cable | — |
| 7 | GND | — | | GND |

GP0/GP1 stay free for the debug UART. GP23-25 and GP29 belong to the CYW43
radio on the Pico W and must not be used.

### 2.1 SNES → Pico: CLK and LATCH (5 V push-pull outputs)

Pico GPIOs are **not** 5 V tolerant. Plain resistor dividers on both:

```
  SNES pin ──[ 2.2k ]──┬──> Pico GPIO
                       │
                     [ 3.9k ]
                       │
                      GND
```

5 V × 3.9 / 6.1 = **3.2 V** high level, source impedance ≈ 1.4 kΩ. Against
~25 pF of pin + wiring that is a ~35 ns edge; the fastest thing on either
line is the `$4017` read strobe, a ~560 ns low pulse. Fine with margin.

### 2.2 Pico → SNES: DATA1 (measured 2026-09-29)

Two facts the console taught us, both now built in:

- **3.3 V is not a high for the 5A22.** Driven push-pull from the Pico
  through 330 Ω, pin 4 sat at 3.3 V and the console read a constant 0 (the
  input threshold is ~0.7 × 5 V). So DATA1 is driven **open-drain**: the PIO
  only pulls the line low or releases it, and a **1 kΩ pull-up to pin 1**
  (the console's own 5 V, present in the cable even though the Pico is not
  powered from it) supplies the high. Pin 4 then sits near 4 V. The 330 Ω
  stays: with GP5 released it limits the current into the Pico's clamp
  diode to ~1 mA, which is a bench-grade compromise, not a spec-clean one.
- **The CPU reads the line inverted.** A pad's active-low buttons read as
  1 bits, so line high = 0 in `$4017`. `link_serial_xfer` flips the bit
  (`eor #1`) on the console side, where the property lives. The Pico sends
  true polarity.

Spec-clean alternative for the final board: a **74AHCT125** (5 V supply,
TTL-level input accepts 3.3 V, 5 V output) between GP5 and pin 4, with the
push-pull firmware (`snos_wiretest_pp`, or build without
`DATA1_OPEN_DRAIN`). The console-side inversion stays either way.

### 2.3 Power

The Pico is powered from its own USB cable, **not** from the console. Pin 1
is left unconnected. The Pico's USB port is also the debug console (USB CDC,
115200 8N1 or any rate; `screen /dev/ttyACM0` or the like), so one cable
does both. Pin 7 ties the two grounds together, and it must: every signal
level above is measured against the console's ground.

If the board ever has to run without a USB host, pin 1 → Schottky → VSYS is
the fallback (the Pico W draws ~45 mA idle and ~150 mA peaks on WiFi); the
diode is what keeps a later USB connection from back-feeding the console.

### 2.4 Bill of materials

| qty | part |
|---|---|
| 1 | Raspberry Pi Pico W (headers) |
| 1 | SNES controller extension cable (cut), five conductors |
| 2 | 2.2 kΩ (CLK, LATCH divider top) |
| 2 | 3.9 kΩ (CLK, LATCH divider bottom) |
| 1 | 330 Ω (DATA1 series, clamp-current limit) |
| 1 | 1 kΩ (DATA1 pull-up to pin 1, +5 V) |
| 1 | USB cable to the Pico (power + debug console) |
| opt | 74AHCT125 (only if §2.2 proves marginal) |

### 2.5 Schematic (netlist form)

```
 console port 2                                  Pico W
 ─────────────                                   ──────
 pin1 +5V ────[1k]────┐   (pull-up for DATA1 only; the Pico is USB-powered)
                      │
 pin4 DATA1 ◀──[330R]─┴───────────────────────  GP5 (7), open-drain
 pin2 CLK ───[2.2k]──┬──────────────────────── GP2 (4)
                     └[3.9k]─ GND
 pin3 LATCH ─[2.2k]──┬──────────────────────── GP3 (5)
                     └[3.9k]─ GND
 pin7 GND ──────────────────────────────────── GND (3, 8, ...)
 (pins 5, 6: not in the cable, not used)

 USB ◀──▶ host: powers the Pico and carries the debug console (CDC).
 GP0/GP1 UART0 is enabled too if a serial adapter is ever preferred.
```

## 3. What the wire carries: the existing link frames

The kernel's link layer already defines a frame (`kernel/link.h`): header
`[seq][ack][flags][sum]` then packed records `[channel][len][payload…]` padded
with `$FF`. The emulated backend moves that frame through a WRAM mailbox; the
serial backend moves the **same bytes** through the port. The Pico therefore
speaks exactly what `tools/gateway.py` speaks — sequence, cumulative ack, SYN
on restart, checksum — and all of it is already tested against this kernel.
On the cable both ends may keep a window of frames in flight: the console
asks the Pico for one (`C2S_LINKWIN`, `net tune window`), and until it does,
and after every console restart, the Pico sends stop-and-wait exactly as
gateway.py does (docs/link-throughput.md). Whichever end restarts holds the
other's IP records until that end acks inside its window -- frames already
on the wire were cut for the peer that is gone -- and the end that lived on
empties its frames in flight when it sees the F_SYN, so both streams start
again at a record (docs/link-throughput.md, "Peer restarts").

**Frame size.** `LINK_FRAME` is 256 in the emulator build. The hardware
build sets **`LINK_FRAME 64`**. The 65816 bit loop (§4) costs ~181 master
cycles a bit with DRAM refresh, so a 64-byte exchange is ~112k cycles with
its quiet lead-in: 82 of a frame's 262 scanlines, measured on the hardware
ROM under Mesen. 128 was measured too and loses -- one ~160-line exchange a
frame and an NMI of 210 lines -- and it would change the wire for both ends
at once (docs/link-throughput.md). The Pico takes it from the same constant.

**Channels.** `LINK_CH_CTRL 0`, `LINK_CH_IP $10` as today, plus one new one:

```
LINK_CH_KBD  $11   keyboard bytes, Pico → SNES only
```

Payload is ASCII plus the `SNOS_KEY_*` codes ($1C-$1F) already defined in
`snos_abi.h`; the kernel pushes every byte into the input ring with
`kbd_push()`, the same funnel the harness and telnetd use. An old kernel skips
the record by its length byte, as every parser on both sides always has.

## 4. The bit-level exchange

Three lines, one heartbeat. The console is the master; every `$4017` read
moves one bit in each direction:

```
   console                              line              Pico (PIO)
   STA $4016  (our bit N in bit 0) ──▶  LATCH = bit N
   LDA $4017 ─ samples DATA1 first ◀──  DATA1 = Pico's bit N (already there)
             └ then one CLK pulse  ──▶  CLK ↓ ↑
                                                          at the ↑ edge: sample
                                                          LATCH (bit N), then
                                                          DATA1 = its bit N+1
   STA $4016  (bit N+1) …
```

LATCH only ever changes between pulses, never during one; DATA1 only ever
changes after a pulse, never while the CPU is sampling. Bit order is **MSB
first** within each byte, bytes in frame order, `LINK_FRAME` bytes per
exchange (64 → 512 clocks). A side with nothing to say sends the frame's
padding; the exchange is always the full length in both directions.

Zoom, three bits:

```
                 bit 0                  bit 1                  bit 2
console   STA $4016   LDA $4017   STA $4016   LDA $4017   STA $4016   LDA $4017
          (out=1)     (in)        (out=0)     (in)        (out=1)     (in)
LATCH   ────┌───────────────────────┐                       ┌──────────────────
            │  = 1                  └──── = 0 ──────────────┘  = 1
CLK     ────────────────┐   ┌───────────────────┐   ┌───────────────────┐   ┌──
                        └───┘                   └───┘                   └───┘
                        ↑   ↑ Pico samples LATCH, then drives next DATA1
                        └ CPU samples DATA1 (start of the read, before the pulse)
DATA1   ═══ Pico bit 0 ═════╪═══ Pico bit 1 ═════════╪═══ Pico bit 2 ═════════╪═══
```

### 4.1 Framing by silence

There is no sync line. A frame boundary is a gap:

1. **Gap.** An exchange is 512 clocks ~8 µs apart. The console guarantees
   **≥ 300 µs with no clocks** before it starts one (a spin in
   `link_serial_xfer`, after waiting out any auto-joypad read on `$4212`).
   The Pico's PIO counts down while it waits for the next pulse; if CLK
   stays high for its timeout (40-100 µs) it raises a "gap" interrupt and the
   ARM restarts the shifter at bit 0 with the first DATA1 bit on the pin.
2. **Count.** A burst is a candidate frame only if exactly 512 clocks
   arrived between gaps. The vblank **auto-joypad read** (`$4200` bit 0,
   which SNOS keeps on for the pad and the kill switch) is a hardware burst
   of 16 clocks ~12 µs apart with LATCH held high: sixteen 1-bits, a short
   burst, dropped by count. Its own LATCH pulse is just data nobody keeps.
3. **Checksum and sequence**, as on the emulated link. A burst that passes
   the count but not the checksum is dropped and the sender resends.

Failure cases fall out of that. Pico powered on mid-exchange: realigned at
the next gap. Noise inside a burst: wrong count or bad checksum, resend. An
exchange split by the auto-read: cannot happen, the NMI poll runs right
after the auto-read finishes and never straddles the next vblank, and a
foreground exchange masks NMI and is only started where it ends before
vblank -- link_serial.c checks the beam again just before the first clock.

A foreground exchange that follows another in the same frame is preceded by
a **kick**: eight bare clocks, a short burst like the auto-read's, so the
adapter restarts and stages the answer it has computed by then
(`link_serial_kick`, docs/link-throughput.md).

Option kept in reserve: the hardware build could turn the auto-read off and
read port 1 by hand in the NMI (one LATCH pulse, sixteen `$4016` reads,
which clock port 1 only). Then port 2 sees no bursts but ours. Not needed
while the count filter works, and the filter is what re-syncs after a reset
anyway.

### 4.2 Looking like an empty port

Until the Pico has received a valid frame it keeps every DATA1 bit
released, so the auto-joypad read and any menu that polls port 2 (the
Everdrive's does) see no buttons. It drops back to that idle two seconds
after the last valid frame. Before this existed, plugging the adapter in
before the console booted fed the menu phantom pad-2 presses.

### 4.3 Timing budget (design values; bit period measured at ~5 µs)

| item | value |
|---|---|
| bit loop, unrolled, FastROM | 176 master cycles, ~181 with DRAM refresh ≈ 8.4 µs |
| 64-byte exchange | first to last clock 104,858 master cycles (4.9 ms); ~112k, 82 scanlines, with the ~300 µs quiet |
| Pico response to CLK edge (PIO at 25 MHz) | < 160 ns |
| gap timeout (PIO countdown) | 64 µs |
| exchanges per second | 60 from the NMI, plus kbd_idle's where one fits (`net tune exch`) |
| payload | 58 bytes a frame each way; with a window of frames in flight every exchange can carry a fresh one, so 60/s is ~3.5 KB/s of link stream each way at most. Stop-and-wait, which the link ran first, moved one fresh frame per two exchanges: ~1.7 KB/s at most, ~0.9 KB/s of TCP measured |

## 5. Firmware architecture (`pico/`)

```
 pico/
   CMakeLists.txt        pico-sdk, PICO_BOARD=pico_w, lwIP (cyw43 threadsafe bg)
   snoslink.pio          the 4021-that-listens state machine
   port.c/.h             PIO + DMA + LATCH IRQ; hands complete RX frames up,
                         stages the link layer's replies in order, one per
                         exchange
   link.c/.h             port of gateway.py's frame logic: seq/ack, SYN, checksum,
                         record packing (greedy fill, CH_PAD), next_out(), and
                         the window of frames in flight
   ctrl.c/.h             the control verbs: HELLO (v2), IPFWD, IPUNFWD, LINKWIN
   kbd.c/.h              keyboard source; TEST MODE: "abcdef" every 5 s
   relay.c/.h            tier 1: TCP client to the host, carries IP datagrams
                         and forward requests; the host runs slirp.py
   wifi_secrets.h.in     SSID / password / relay host, copied to wifi_secrets.h
   main.c                cyw43 init, WiFi join, service loop, UART log
 tools/
   picoslirp.py          host side of the relay: accepts the Pico, feeds slirp.py
```

**Threading.** Everything in the main loop; the PIO/DMA side raises a flag
from the LATCH IRQ and the "frame received" DMA IRQ. lwIP runs in
`cyw43_arch_lwip_threadsafe_background` mode; relay callbacks only enqueue.

**The link layer owns the frame in every mode.** Keyboard bytes are local to
the Pico, so the Pico must build frames itself; making it the link peer in
tier 1 as well means tier 2 replaces `relay.c` with a local NAT and nothing
else moves.

### 5.1 Relay protocol (Pico ↔ host, TCP, port 3210)

Length-prefixed messages, little-endian, both directions:

```
 [type u8][len u16][payload]
   type 1  IP datagram (one IPv4 packet)        both directions
   type 2  forward request  [port u16][ch u8]   Pico → host  (IPFWD)
   type 3  forward release  [port u16][ch u8]   Pico → host  (IPUNFWD)
   type 4  forward error    [port u16][ch u8]   host → Pico  (S2C_ERR)
   type 5  hello info       [ipv4 x4]           host → Pico  (address to
                                                report in S2C_HELLO)
```

`tools/picoslirp.py` is ~150 lines: a socket server, `slirp.Slirp` with its
`output` callback wired to type-1 messages back to the Pico, `forward_add/del`
for types 2/3. `gateway.py` and the file mailbox are not involved.

### 5.2 Tier 2: Pico NAT

`nat_core.c` is the fixed-size, host-testable IPv4/TCP/UDP state machine and
`nat.c` is its lwIP raw-API adapter. An empty relay host selects it; a saved
host keeps tier-1 relay mode. Outbound TCP is spliced through `tcp_connect`;
the core emits console-side SYN-ACK, ACK, data, FIN and RST packets. It keeps
two-MSS receive chains per flow, sends only one console segment at a time,
records uIP's advertised window, and calls `tcp_recved()` only
after the console ACKs. RTO is 2.5 s, data retries 120, idle 300 s,
TIME_WAIT 4 s, inbound FIN grace 750 ms and the per-port accept cap is 4,
matching `slirp.py`.

Port 7778 at the DHCP address reported in HELLO is a local speedtest peer.
It is a NAT-core flow, not a loopback lwIP socket: its protocol source/sink
therefore follows the same console window, one-segment output, busy-link
deferral, retry and FIN rules as an external TCP flow. `net speedtest` with no
argument measures the console-to-Pico link in tier 2; an explicit host and port
continues to use a remote peer, and tier 1 remains the PC `speedtest.py` path.
The `nat:` status line reports active local peers as `speed=`.

UDP has eight fixed flows and preserves the console-visible source address.
`10.0.2.3` rotates the DHCP resolvers and falls back to 1.1.1.1 then 8.8.8.8;
unchanged DHCP configuration keeps its retry cursor, and unspecified
resolvers are ignored. Replies are restored to `10.0.2.3` and oversized
replies are counted and dropped. Echoes to `10.0.2.2`
and `10.0.2.3` are answered locally. External ICMP is not implemented yet:
it needs a separate lwIP raw-ICMP request/reply demux.

`IPFWD`/`IPUNFWD` bind/listen on the Pico and accepted sockets become inbound
console SYNs. Accepted sockets take ownership callbacks before they are
queued, and mode/reset tears down their listeners and PCBs. HELLO reports the
DHCP IPv4 address. Foreground raw-API calls are under
`cyw43_arch_lwip_begin/end`; callback output uses a four-packet handoff, and
the link queue itself is spinlock protected for every producer. The fixed core
is 35.4 KiB (eight TCP flows × two-MSS output buffers); its 40,000-byte store,
receive scratch and callback handoff are static. `lwipopts.h` uses an 18 KiB
heap, 2-MSS TCP windows, 16 pbufs, 32 TCP segments, 12 TCP PCBs, eight listen
PCBs and 10 UDP PCBs. Packet scratch is static under the lwIP serialization;
the ARM stack report's deepest NAT/link call chain is 296 bytes (1,032 bytes
including `main`), and the firmware reserves
8 KiB across the linked scratch banks for main, link and Bluetooth callbacks;
this is a reservation, not a measured hardware stack high-water mark.

Leaving NAT mode tears down its flows, forwards, listeners and advertised
HELLO address. The mode switch never touches the link: its current frame and
sequence state continue unchanged, and IP/control records already crossing
the link finish whole. Only unstarted NAT or relay output is discarded. The
incoming IP stream assembler survives the switch, so a packet completed after
it is delivered to the newly selected mode. Keyboard records are delivered
exactly once and in order across the transition.

`pico/tests/nat_test.py` compiles the pure C core with a fake raw transport.
It covers local ICMP, UDP and DNS source restoration, TCP state/data/FIN,
stray/refused parsing, forwarding/cap, tuple reuse, oversize UDP,
backpressure and garbage, plus all speedtest phases, duplex flows, zero-window
deferral, RST and FIN. The lwIP adapter test confirms that the configured DHCP
address selects that local peer. WiFi association, DHCP, callback timing, loss
and real inbound routing still require hardware.

## 6. The `wire` test, and the SNOS-side changes

**`wire`** (`apps/wire`, in the default ROM) is the cable and framing test:
one 64-byte exchange per vblank through the same `link_serial_xfer` the
kernel uses. The console sends a counter plus `1234567890…`, the Pico
(`snos_wiretest.uf2`, no WiFi) answers a counter plus `ABCDEFGHIJ…`. Once a
second the console prints `frames ok bad pico# reply…` and the Pico prints
its mirror image on the UART. Both counters moving with both `bad` columns
at zero is the verdict; `short` and `gaps` on the Pico side should tick at
60/s (the auto-read bursts) and nothing else. Run it as `wire` at the shell;
B exits.

Changes, all done:


1. `kernel/linkxfer.s` `link_serial_xfer(tx, rx, n)`: the exchange in §4, ~26
   CPU cycles a bit, both buffers through `[dp],y` so any bank works. It
   waits out an auto-joypad read (`$4212` bit 0), holds LATCH low for
   ~300 µs, then clocks `n` bytes. Opcodes verified in the listing (`b7`/`97`
   for the indirect-long-indexed pair, `8d 16 40` / `ad 17 40` for the port).
   `apps/wire/linkxfer.s` is a symlink to it.
2. `kernel/link_serial.c` (only under `LINK_HW`): `link_serial_send()` runs
   the exchange and keeps the reply; `link_serial_recv()` hands it over on the
   next poll, per link.h's shift-register contract. Presence is probed once a
   second with an empty unflagged frame until a reply checksums, then every
   poll; 120 consecutive bad exchanges drop it again. The emulator build keeps
   the old stubs, so the suite tests exactly the ROM it always did.
3. `kernel/net.c`: `LINK_CH_KBD` records go to `kbd_push()`; `frame_fits`
   reserves input-ring room the same way it does for IP. This part is in the
   default build and covered by `test/kbd.lua` (the gwip peer pushes
   `help\n` as a KBD record; the shell echo and the prompt after it are the
   verdict).
4. `kernel/link.h`: `LINK_CH_KBD 0x11`; `LINK_FRAME` is 64 under `LINK_HW`.
   `kernel/input.c`: under `LINK_HW` kbd_idle takes one foreground exchange
   right after the NMI's, where it still fits before the next vblank
   (`link_serial_fg_fits`, `net tune exch`; docs/link-throughput.md).
5. `snes/Makefile`: `make LINK_HW=1` adds `-DLINK_HW=1` and builds into
   `build-hw/` so the emulator ROM is never overwritten.

Not done: `gateway.py`/`gwip.lua`/`netbridge.lua` still carry `FRAME = 256`
and are not used with the hardware ROM; `tools/picoslirp.py` is the host
side for the adapter and takes its size from the Pico.

## 7. Upgrades kept in reserve

- **A seven-wire cable** would allow IOBIT as the console's data line (a
  hardware frame sync from LATCH, no gap detection) and DATA2 as a second
  input line (2 bits/clock Pico → SNES). Neither is worth a cable.
- **Frame size** is one constant; re-measure on silicon.
- **Real USB keyboard**: `kbd.c` grows a TinyUSB host (the Pico W's USB port
  in host mode with an OTG adapter) and the test pattern becomes a HID report
  parser. Nothing else changes.

## 8. Why not …

- *A UART on the Pico and bit-banged async on the SNES?* The SNES has no timer
  worth the name; synchronous with the console as master is the only thing
  that works on a 65816 without cycle-counting every path.
- *Both ports?* Two cables, two sockets, and the whole point is one board.
- *Bluetooth keyboard on the Pico W?* Later. HID over BTstack is a firmware
  add, not a circuit change.

## 9. First power-up checklist

1. Console on, nothing in port 2. Measure the idle levels of pins 2/3 against
   pin 7 (expect CLK high ~5 V, LATCH low ~0 V). Measured: as expected.
2. Board on the bench, powered from USB, **not** plugged in: scope GP2/GP3
   while driving the divider inputs from a 5 V source; confirm 3.2 V.
3. Plug in, console off. Power on with the wiretest firmware: its log should
   show `short` and `gaps` ticking at 60/s from the auto-joypad read alone,
   before SNOS runs anything.
4. Flash `snos_wiretest.uf2`, boot the default SNOS ROM, run `wire` (§6).
   Both sides count frames, `bad` stays 0.
5. Flash `snos_pico.uf2` (WiFi), boot the `LINK_HW=1` ROM. `net status`
   reports the Pico's HELLO; `abcdef` appears at the prompt every 5 s.
