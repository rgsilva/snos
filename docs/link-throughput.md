# Link throughput: where the time goes on the real cable, and what moved it

PLAN.md "M7 follow-ups" item 1. A running log: the method, the two models,
each step's measured effect, what was tried and rejected, and what only
hardware can confirm. Numbers are B/s of TCP payload from `net speedtest`
(download, upload, then both at once), 10 s phases, unless stated.

## The baseline, measured on silicon (2026-10-01)

`main` @ 04b19ce, Pico in NAT mode, its built-in speedtest peer:

| | download | upload | both |
|---|---|---|---|
| hardware | 876 | 919 | 876 + 561 |

Zero checksum failures, zero desyncs, zero refusals. Per 120 exchanges the
Pico's `stats` line shows ~40 fresh frames, ~40 resends and ~40 frames with
nothing to say while downloading; the console's counters show the mirror
image uploading.

## Two models, and why there are two

**Mesen, the hardware ROM, a modelled adapter** (`make LINK_HW=1
test-hwspeed`). `snes/test/hwport.lua` puts the Pico on controller port 2:
each `$4016` write is LATCH, each `$4017` read is one clock and returns the
adapter's bit inverted, as the real CPU reads the line. It frames exchanges
the way the real adapter does and stages its transmit frame the way
`pico/port.c` does: the frame shifted out is the one latched at the
adapter's last restart, and restarts happen at the end of every exchange
and ~64 us after the vblank auto-joypad read. The link peer above it is
`test/gwip.lua` driven one exchange at a time (`gw:exchange`), with the
Pico's 64-byte frames, its sender policy and a TCP shaped like the NAT's
speedtest peer (MSS 1460, one segment in flight).

It reproduces the hardware run **byte for byte**: 876 B/s (8760 bytes),
919 B/s (9195 bytes), 876 + 561 B/s (5618 bytes), and the same frame and
resend counter deltas (+677/+187 uploading, +708/+135 both). The console is
deterministic and it is the slow end, so this is the authority for every
number below: it runs the real kernel/linkxfer.s, the real NMI and the real
foreground poll gate. `HWPROF=1` times kernel functions from the link map
and listings without touching the kernel (tools/hwprof.py).

**The host bench** (`python3 pico/tests/linkbench.py`). The real
`snes/kernel/net.c`, `link.c`, `link_serial.c`, `ip.c` and uIP, with the
unchanged `apps/speedtest/speedtest.c` on top, coupled to the real
`pico/link.c`, `pico/ctrl.c` and `pico/nat_core.c` (its speedtest peer)
through a timing model: master cycles, the frame and vblank, the auto-read,
the adapter's TX staging, the Pico main loop's turnaround (`pico_us`), and
a cost table for console CPU calibrated from Mesen profiles. It runs in a
second and sweeps what Mesen cannot: the real Pico code, its turnaround
time, and staging policies for firmware that does not exist yet.

It lands within 10-20% of the hardware (1022 / 1021 / 876 + 612) with the
same structure: 3.3-3.5 exchanges per fresh frame, Pico resends 1:1 with
fresh frames. The residue is quantisation, below. Its absolute numbers are
used for ranking; Mesen's for claims.

## What the baseline actually is

The briefing's hypothesis was an extra exchange of lag in the turnaround,
from `port.c` latching its transmit frame at the end of the previous
exchange. **On the NMI-only path that lag does not exist**: the vblank
auto-joypad read clocks port 2 sixteen times before every NMI exchange, the
adapter's gap detector restarts it ~64 us later, and that restart latches
the reply the main loop has computed by then. The hardware counters say so
too: the Pico's resends equal its fresh frames, i.e. two exchanges per
fresh frame while it is sending, not three. The third exchange is time with
nothing to send, and it comes from the console:

1. **The console is CPU-bound, at -O0, in C.** Profiled on the hardware
   ROM: the NMI handler takes 240k of a frame's 357k master cycles (176 of
   262 scanlines). Inside it the exchange itself is ~112k (the bit loop is
   ~181 cycles a bit with DRAM refresh; first to last clock 104,858), but
   `link_serial_recv`'s byte-copy loop is 34k, `link_serial_send`'s
   all-zero scan ~47k, and `proc_tick` 22k. The foreground gets ~86 lines
   a frame, and `con_flush` (28k) and an idle `ip_service` pass (39k) take
   most of that before an application runs.
2. **uIP's checksum is C.** A 1500-byte datagram costs 838k cycles to take
   in (~494 a byte), a 551-byte segment 312k to send. `netasm.s` has had a
   ones-complement sum (`ip_sum`) since M11; uIP never used it.
3. **The download window reopens on a timer.** A 1460-byte segment fills
   past the point where uIP closes its window (`ip.c` keeps a whole MSS of
   ring free), so the console's ACK advertises zero; only the next 30-frame
   periodic tick reopens it. A segment takes ~0.9 s to cross, so each one
   costs one or one-and-a-half seconds depending on where the tick falls.
   The hardware lands on 1.5 s (6 segments in 10 s), the host bench on 1.0
   s (7); that is the bench's whole download error.
4. **`SYS_NET_WRITE` copies through a stack buffer with a C loop**: 126k
   cycles per 128 bytes, ~840 a byte, plus ~12k of COP dispatch per syscall.

Then, under all of that, link stop-and-wait: two exchanges per fresh frame.

## The steps

Each row is Mesen's hardware-ROM run (`make LINK_HW=1 test-hwspeed`), then
the host bench's, at that commit.

| step | download | upload | both | bench |
|---|---|---|---|---|
| baseline (silicon: 876 / 919 / 876 + 561) | 876 | 919 | 876 + 561 | 1022 / 1021 / 876 + 612 |
| 2. the turnaround is CPU | 1314 | 1226 | 1314 + 510 | 1460 / 1277 / 1314 + 510 |
| 3. link window (4) | 2336 | 1839 | 2172 + 817 | 2482 / 2043 / 2336 + 868 |
| 4. two exchanges a frame | 2482 | 1941 | 2190 + 817 | 3212 / 2554 / 3066 + 1123 |
| 5a. 1023-byte segments, writes uncapped | 2425 | 2352 | 2044 + 1534 | 3212 / 3068 / 2920 + 2148 |
| 5b. the speedtest reads and writes 512 | 2628 | 2557 | 2336 + 1738 | 3555 / 3375 / 3358 + 2454 |
| 5c. proc_tick without soft divides | 3358 | 3068 | 3066 + 2250 | 3606 / 3477 / 3358 + 2454 |

### 2. The turnaround is CPU

Four changes, none of them to the protocol:

- `link_serial_recv` hands the frame over with one MVN (34k -> 3.6k master
  cycles a poll) and `frame_ok` tells a dead line from a frame by the two
  bytes at `LINK_HDR` instead of scanning all 64 (~47k -> ~3k). The NMI
  handler drops from 176 to 126 scanlines. Alone: 1314 / 1072 / 1168 + 459.
- uIP's IP/TCP/UDP checksums run over `ip_sum` (`UIP_ARCH_CHKSUM`):
  taking in a 1500-byte datagram 838k -> 179k, sending a 551-byte segment
  312k -> 79k. Alone on top: upload 1072 -> 1175.
- `ip_service` polls a connection whose receive window it closed as soon as
  the ring has room again, so the window update leaves within a pass instead
  of at the next 30-frame tick. No change at this step -- a segment now takes
  as long to cross the link as the tick period, ~59 exchanges -- but every
  later step needs it, because the tick would otherwise cap downloads at one
  segment per half second.
- `SYS_NET_WRITE` copies with `far_copy` (netasm.s), not a C loop: 126k ->
  43k for 128 bytes. Upload 1175 -> 1226.

What is left per frame: the NMI handler 172k of 357k master cycles (the
exchange itself 112k, `proc_tick` 22k, the rest net_service's C), and in the
foreground `con_flush` 28k and an idle `ip_service` pass 39k before the
application runs. `proc_tick` and `con_flush` are not the link's and are
left alone here.

### 3. A link window

Both senders keep frames in flight: `net.c`'s txwin slots on the console,
`pico/link.c`'s frames[] on the Pico. Acks are cumulative; a full window, or
nothing new to say, goes back to the oldest unacknowledged frame. Nothing
about the frame changed, and no receiver did: every peer already took frames
strictly in order and acknowledged cumulatively (gwip.lua has pipelined
toward the console with go-back-N since M6).

Window 1 is stop-and-wait frame for frame -- `link_test.py`'s 10,000-exchange
differential against gateway.py's sender still passes, and on the hardware
ROM `net tune window 1` reproduces step 2 to the byte. The console asks the
Pico for its window with a new control verb, `C2S_LINKWIN` (0x0D, `[1][n]`),
after every HELLO reply; the Pico starts at 1 and goes back to 1 whenever the
console restarts, so an old ROM never gets anything but today's protocol, and
an old Pico ignores the verb (ctrl.c has always skipped unknown types). The
mailbox backend always runs at 1: it holds one frame per direction. While a
sender's own SYN is unacknowledged its window is 1, because the receiver
adopts the first SYN frame it sees.

At one exchange a frame a window of 2 is all it takes: the reply to exchange
k is on the wire at k+1 (the auto-read restage), so two frames in flight keep
every exchange fresh; 3, 4 and 8 measure the same. The default is 4, because
the next step makes the reply latency two exchanges on some of them.

`pico/tests/window_test.py` runs the real console `net.c` against the real
`pico/link.c` over a channel that loses, corrupts, repeats, skips and delays
frames both ways while the console stops reading for stretches: every pair
of windows 1..8, exactly-once in-order delivery of IP both ways and keyboard
records, 64/64.

### 4. More than one exchange per video frame

`input.c`'s `fg_poll_fits()` said "a serial exchange is ~92k master cycles,
most of the way to the next vblank from anywhere: it only ever fits from the
NMI". The exchange is ~112k cycles, 82 lines -- a third of a frame, not most
of it -- so the arithmetic was wrong; but the conclusion was nearly right for
a reason the comment did not give. The NMI handler, exchange included, runs
from vblank to about line 96 of the next frame (133 lines, measured with the
window in), and a foreground poll is ~110 lines with its C: it must end
before vblank, so it can only start in a ~5-line window right after the NMI.
The spin in `kbd_idle` almost never reaches it -- by the time the display
work, an `ip_service` pass and the application have run, the window has shut.

What this step does:

- `kbd_idle` takes its extra exchange first thing after the frame turns
  (hardware build only), while the caller is waiting anyway.
- `link_serial.c` keeps the budget (`net tune exch`, at most 3, default 2)
  and the gate, `link_serial_fg_fits`: start by line 100, or after the NMI
  and auto-read (229 on).
- **The last word is a check of the beam just before the clocks start**
  (`xfer()`): from the kick's first clock to the exchange's last is a fixed
  ~85 lines, but the C before it varies from 29k to 93k cycles. An exchange
  that could still be clocking at vblank is not started; the frame built for
  it is lost as if on the wire, and the window resends it. Mesen found this:
  with the early gate alone, 125 exchanges were still clocking when vblank
  began, NMIs were held off, and the run wedged. The host bench had not seen
  it -- its cost table had the C before the exchange at half its real size
  (corrected now). With the check: none, in every run since.
- An exchange that follows another in the same frame is **kicked**: eight
  bare `$4017` clocks (`link_serial_kick`, linkxfer.s), a short burst like
  the auto-read's, which the adapter's gap detector turns into a restart
  ~64 us later -- the restart that stages its answer. Without it the answer
  to the previous exchange waits one exchange more. `kick_test.py` runs the
  burst through the assembled PIO program: one byte, gap after 64.6 us.
- **`pico/port.c` stages in order.** It used to stage the newest pending
  reply at every restart; a reply staged at one restart and replaced at the
  next never reached the wire. Harmless at window 1 -- every reply was the
  same frame or an ack -- but with frames in flight a frame that silently
  never left costs a go-back. Now a staged frame is replaced only once it
  has gone out whole, and with nothing new the last frame goes out again.
  The host bench, at two exchanges a frame: with the old staging a Pico main
  loop slower than ~2 ms per turn, or one that stalls 5 ms every 20 turns,
  lost hundreds of frames unsent and download fell to 1460 B/s (duplex 584 +
  357); with in-order staging, nothing was lost and throughput did not move.
  At one exchange a frame the two policies are identical.

The gain on the hardware ROM is small: the NMI ends too late for the
foreground exchange to fit in most frames (186 of 2300 in a speedtest). The
bench says 33% of frames, because it still underestimates how much of each
frame the application has left when the NMI returns; Mesen is the number.
What would make it pay: an NMI handler that ends earlier. `proc_tick` alone is
22k cycles of it, and `net_service`'s own C ~39k.

With a new ROM and **old** firmware (no window from the Pico, newest-wins
staging), the bench says two exchanges a frame with the kick still helps
(1460 / 2043 -> 1898 / 2554); without the kick it does nothing.

### 5. What is left is above the link

Three changes, each measured alone on the hardware ROM:

- **5a. Bigger segments, uncapped writes.** `NET_TXRING` is the upload
  segment size -- uIP keeps one segment in flight and fills it from the ring
  -- so 512 meant 511-byte segments, each paying a whole round trip. 1024
  doubles that for 4 KB of WRAM (2048 would reach the MSS and does not fit:
  ~6 KB of HiRAM is left). And `SYS_NET_WRITE` copies straight from the
  application's bank into the ring (`net_write_far`, far_copy) instead of
  through a 128-byte stack buffer, which also capped every write at 128
  bytes; `mc` now writes 1024 at a time. Upload 1941 -> 2352, duplex upload
  817 -> 1534.
- **5b. The measuring app's own buffer.** `apps/speedtest` read and wrote 128
  bytes per call, and each pass round its loop is five syscalls at ~13k
  cycles apiece: ~450 cycles a byte of pure overhead, and on a download it
  is the drain time that holds the receive window shut. 512: +8-14% all
  round. This is the instrument, not the link; it is listed separately so
  the gain is not mistaken for one an application gets for free.
- **5c. `proc_tick`.** The scheduler's tick runs in every NMI, and with one
  process its round-robin "stay" cost 22k cycles -- three `% PROC_MAX` and
  two `procs[n]` reads, each a soft divide or multiply at -O0 (no
  `--target=snes`, docs/calypsi-interrupt-hazard.md). A compare and
  constant indices: 3.6k. The NMI handler drops from 134 to 120 lines and
  ends early enough that step 4's foreground exchange fits in a third of
  the frames instead of one in twelve: +28% download, +20% upload, +30%
  duplex. The fastest step of the lot, and not the link's code.

Tried and dropped:

- **Acknowledgements ahead of data in the Pico's queue.** In duplex the
  console's next upload segment waits on the NAT's ack, queued behind a
  1500-byte download segment -- or so it seemed. The NAT only emits a
  datagram when the link queue holds no IP (`link_busy`), so an ack never
  has an unbegun datagram in front of it, only the rest of the one in
  flight, which it must not cut into. No effect in the bench; reverted.

## The one test this breaks

`pico/tests/transition_test.py` ends with a property,
`p4_console_sources_equal_main`, that fails unless `snes/kernel/net.c` and
`link.h` are byte-identical to commit 99b6002. It was tier 2's guard that the
NAT work left the SNES tree alone. A console-side window cannot leave net.c
alone, so from step 3 on that one property fails; the brief says not to edit
an existing test that encodes behaviour being deliberately changed, so it is
not edited. Its 21 behavioural properties -- mode switches at every byte
offset, restarts, keyboard exactly-once -- still pass, and they run the 99b6002
console against the new windowed Pico, which is the old-ROM/new-firmware
pairing. Retiring the property, or re-pinning `console_main_fixture.MAIN`
(which would also move the other harnesses to the new console), is the
owner's call.
