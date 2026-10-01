# Measuring the link

On the gateway host, start the ordinary TCP server (Python standard library):

```sh
python3 snes/tools/speedtest.py
```

At the console:

```text
net speedtest
net speedtest 192.168.1.20:7778 10
net speedtest - 2
```

The default is the host address learned from HELLO, port 7778, ten seconds per
phase. In relay mode this is the machine running `speedtest.py`. In Pico NAT
mode it is the Pico's own WiFi address: port 7778 is a built-in peer, so the
default measures the console-to-Pico link without a PC-side server. `-` keeps
that address while changing the duration. An explicit `host:port` always uses
the requested remote peer. Durations are whole seconds, 1 through 120. Any key
cancels; connection setup and control replies have a separate 15-second timeout.
The server accepts `--bind` and `--port`. The relay gateway continues to route
TCP without knowing this protocol.

The three phases are download, upload, and simultaneous download/upload. Each
prints integer B/s and its received byte count. Download counts bytes read by
the console during a `sys_frames()` window; upload uses the server's received
count during an equally long monotonic-clock window. Both divide by the chosen
duration. TCP headers, control lines and bytes still queued at the deadline
are excluded. Setup and fetching the upload result are outside the measurement.
A healthy slow link therefore takes the same measurement time as a fast one.
Short runs include a larger proportion of the initial transit delay; use the
same duration when comparing changes, preferably the ten-second default.

The duplex phase prepares **two connections** before starting either. A single
connection would mix the shared link's contention with uIP's per-connection
single-outstanding-segment behavior. Two streams ask whether the shared stack
and link can move both ways together. Their receiver windows are offset by the
GO line's transit time; they do not require synchronized clocks. The test
measures TCP application throughput through the whole path, not raw wire speed.

The transfer code lives in `apps/speedtest`, bank D6, arena A. Its buffers belong
to the app, and every stream operation crosses COP. The small shell wrapper
supplies the learned address and prints the counters unavailable through the
existing stats syscall. Link frames, bad sums and resends appear before the
first phase and after each phase; refused frames, HELLO replies, IP drops and
IP desync appear at the end. These are cumulative 16-bit counters, like
`net status`, not reset by a run. Compare before/after counters modulo 65536.
The console's frame deadlines also use unsigned subtraction across wrap.

## Wire protocol

Each connection starts with `D frames\n` or `U frames\n`, where frames is a
decimal duration from 60 through 7200. The server replies `OK\n` and waits for
`GO\n`. No payload is exchanged while the other duplex socket is preparing.

* D: the server sends raw bytes until the console closes at its deadline.
  There is no drain: pending bytes are discarded. The host log labels its
  send count **download queued**, because that number is not a measurement of
  delivery to the console.
* U: the server starts its receive window on GO, counts raw bytes until its
  deadline, and sends `count\n`. It then discards any late tail until close.
  The console stops enqueueing at its own frame deadline and reads that count.

The client currently sends `x` bytes, but U treats the payload as opaque. The
Pico peer uses the NAT's normal console-facing TCP flow, including its advertised
window, one-segment output, retry and FIN behavior; it does not open a socket to
its own WiFi address. Connections close after each phase, including cancellation
and failure.
There is no fixed payload size, and no acknowledgment protocol above TCP.

`test-speedtest` uses a separate implementation in Lua over `gwip.lua`, with
short receive windows and independently checked byte counts. The host test
uses loopback sockets, checks concurrent directions, verifies that late upload
bytes do not enter the reported count, and exercises invalid requests and
handshake timeouts. Neither test measures real controller-port performance.
