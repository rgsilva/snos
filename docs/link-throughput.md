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
