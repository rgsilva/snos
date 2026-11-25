# SNOS Pico W adapter

Tier 1 firmware for controller port 2. Follow `../docs/pico-adapter.md` for
wiring and first power-up measurements. The five wires carry power, ground,
CLK (GP2 input), LATCH / console DATA-OUT (GP3 input), and DATA1 (GP5 output).
GP4 and GP6 are unused. UART0 uses GP0 TX / GP1 RX at 115200 baud, 8N1.
CLK and LATCH require the documented dividers; Pico GPIO is not 5 V tolerant.

WiFi and relay settings are configured at runtime through USB CDC or UART0.
No credentials header is needed to build.

Build with pico-sdk **2.1.1** and the ARM GNU toolchain. The Bluetooth
keyboard needs the SDK's BTstack submodule, which a shallow SDK clone lacks:
`git -C $PICO_SDK_PATH submodule update --init lib/btstack`.

```sh
export PICO_SDK_PATH=/home/ricardo/pico/pico-sdk
export PATH=/home/ricardo/pico/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin:$PATH
cd pico
mkdir -p build
cd build
cmake -G Ninja -DPICO_SDK_PATH=$PICO_SDK_PATH -DPICO_BOARD=pico_w ..
ninja
```

Hold BOOTSEL while connecting USB, then copy `build/snos_pico.uf2` to the
RPI-RP2 drive. Watch UART with a 3.3 V USB/UART adapter (GP0 to adapter RX,
common ground), for example `screen /dev/ttyUSB0 115200`. USB CDC stdio is
also enabled (`screen /dev/ttyACM0 115200`). Enter a command followed by Enter
(CR or LF):

```text
wifi My WiFi|my password|192.168.1.100
wifi My WiFi|my password|
wifi?
wifi clear
mode
mode nat
mode relay 192.168.1.100
stats
log debug
log info
log off
log on
log
bt
bt scan
bt pair 1
bt forget
```

`wifi SSID|PASSWORD|HOST` saves settings and starts an asynchronous join.
SSID/password/host limits are 32/64/63 bytes; `|` cannot appear in fields.
An empty password selects an open network. An empty HOST selects tier-2 NAT;
otherwise HOST is the IPv4 address or DNS name of the machine running
`picoslirp.py` and tier-1 relay uses TCP port 3210. `mode` reports the active
setting; `mode nat` and `mode relay HOST` save mode changes without changing
WiFi credentials. Existing saved settings with a host remain relay settings.
In NAT mode HELLO reports the Pico DHCP address and `stats` includes flows.
`net speedtest` with no argument connects to the built-in port-7778 peer at
that address, so it measures the console-to-Pico link. An explicit `HOST:PORT`
still measures that remote peer. The `[NAT]` stats line includes `speed=`, the
number of active built-in speedtest flows.
In NAT mode the console's pings reach real hosts: the Pico forwards echo
requests over lwIP raw ICMP and maps the replies back. It answers pings to
10.0.2.2, 10.0.2.3 and its own DHCP address itself. ICMP errors (unreachable,
TTL exceeded) are dropped, not relayed to the console. `icmp=` in the `[NAT]`
line counts echo flows: at most four, each freed after 15 idle seconds.
A mode switch never touches the link; keyboard input is delivered exactly once.
Input is echoed, including the password in the setup command; `wifi?` masks it.
Backspace edits input. Overlong lines are discarded.

Settings persist in the last 4 KiB sector of the Pico W's 2 MiB flash.
`wifi clear` erases that sector and disconnects WiFi until configured again.
Bluetooth bonds use BTstack's two-sector flash bank immediately before that
sector, so saving WiFi settings does not erase keyboard bonds.
An optional local `wifi_secrets.h` can supply boot defaults when flash has no
valid settings (including after clearing and rebooting); the template defaults
are empty. Flash erase/program briefly pauses interrupt-driven link service.
Avoid changing settings during an active SNES exchange.

Port, link, and keyboard service starts without waiting for WiFi. Failed joins
retry after 10 seconds; a join has a 30-second deadline. Without an SSID the
firmware waits for configuration. WiFi state changes and the stats lines are
logged (see below); `stats` prints immediately.

Every line the firmware prints starts with a subsystem prefix in square
brackets, so the output can be filtered (`grep '^\[BT\]'`):

| Prefix    | Lines                                                            |
|-----------|------------------------------------------------------------------|
| `[SNES]`  | the controller-port link: the stats line (gaps, frames, ...), restarts |
| `[NAT]`   | the NAT stats line (`in= out= drop= ...`) and NAT events         |
| `[BT]`    | everything Bluetooth: scan, pairing progress, keyboard state     |
| `[WIFI]`  | join progress and state, `wifi?`, saved or cleared settings      |
| `[KBD]`   | `type`, `kbd test`, `kbd map`                                    |
| `[RELAY]` | tier-1 relay events                                              |
| `[MODE]`  | `mode`                                                           |
| `[CTRL]`  | control-channel warnings                                         |
| `[LOG]`   | `log`, `log on`, `log off`                                       |
| `[CON]`   | the command list and console line errors                         |
| `[WIRE]`  | the `snos_wiretest` targets, every line                          |

The prefix replaces the old lower-case tag: `[BT] keyboard ready [LE boot]`,
`[SNES] gaps=...`, `[NAT] in=...`. The echo of the characters you type has no
prefix.

Unsolicited output has a level, set with `log off|info|debug`; `log` alone
reports it. The default at every boot is `info`, and the setting is not saved
to flash. `log on` still works and means `debug`. The level is not printed in
the line.

| Level   | Shows                                                                  |
|---------|------------------------------------------------------------------------|
| `off`   | no unsolicited lines                                                   |
| `info`  | events and problems: Bluetooth connect, pairing, ready, disconnect and the two "press a key" lines; WiFi joining, joined, lost, failed, not configured; `[SNES]` link up, link lost, restarted; `[RELAY]` link up/down; every warning or error (queue full, stream desync, init or setup failed) |
| `debug` | all of `info`, plus the stats lines every two seconds (`[SNES] gaps=...`, `[NAT] in=...`) |

Replies print at every level, `off` included: the answer to a command, `stats`,
`bt scan` results, `bt`, `wifi?`, `mode`, the command list, errors a command
causes, passkeys, and the progress of an operation you started. After
`bt pair`, every `[BT]` line prints until `keyboard ready` (or two minutes
pass); after `wifi ...`, `wifi clear` or `mode ...`, the join progress prints
until it succeeds or fails (or a minute passes). At `info` and `debug` those
windows add nothing visible; at `off` they are all you see.

From the repository root on the relay host:

```sh
python3 snes/tools/picoslirp.py
python3 snes/tools/picoslirp.py --selftest
```

Allow TCP port 3210 and application forward ports through the host firewall.
Only one Pico session is serviced at a time. Disconnecting releases NAT flows
and forwards. `--debug` dumps IP datagrams; `SNOS_GW_STATS=1` enables host stats.
HELLO reports protocol version 2 and the relay host's address. The keyboard
module sends `abcdef` every five seconds only with `kbd test on`; `type <text>`
sends text plus Enter as keystrokes.

Bluetooth HID keyboards pair from the console. Both Bluetooth Classic and BLE
HID-over-GATT keyboards are supported (one bonded keyboard at a time):

```text
bt scan
bt pair 1
bt
```

`bt scan` runs for about 10 seconds and prints `[LE]` and `[BR]` devices, with
likely keyboards first. LE scanning is active, so names from scan responses are
included. `bt pair <n|addr>` connects to an entry from the latest scan, or to a
Bluetooth address (an address entered directly is Classic). Every pairing and
connection step prints, at any log level, up to `[BT] keyboard ready [...]`.
When the LE path waits for the first key report or for the keyboard to
reconnect in report protocol, it says `[BT] press a key on the keyboard now`
or `[BT] waiting for the keyboard: press a key to wake it`. If a passkey is
printed, type those digits on the keyboard and press Enter. A bonded keyboard
reconnects automatically at boot and after sleep disconnects; LE reconnect uses
the bonded identity/IRK and controller accept list, so Logitech private
addresses are handled. `bt` shows the keyboard transport. `bt forget`
disconnects and drops either kind of bond target.

For multi-device Logitech keyboards (MX Keys, ERGO, K860, Wave Keys), put the
desired Easy-Switch slot in pairing mode first: hold its Easy-Switch key for
about three seconds until the light blinks quickly.
`bt off`/`bt on` power the Bluetooth host stack down/up.

LE discovery requests a 15 ms active scan window every 60 ms (25% configured
duty); no over-the-air duty measurement has been made. The target record is
written by the main loop as soon as it is known, under the CYW43 async-context
lock shared with BTstack's TLV store. The target write, BTstack bond writes and
flash-bank compaction are still synchronous while that lock is held; pairing's
software ECC is also synchronous. RF coexistence and controller-port deadlines
under those operations still require hardware timing measurement.

`LINK_FRAME` is defined once in `link.h`, at 64 bytes. Each exchange is 512
clocks, MSB first, about 8 us per bit. The console writes $4016 bit 0 onto
LATCH before each $4017 read. DATA1 is sampled at the start of that read;
CLK idles high and pulses low for about 560 ns. The Pico samples LATCH on
CLK's rising (trailing) edge, then drives the next DATA1 bit. The first TX
bit is preloaded when the port is armed. LATCH is data, not a sync signal.

Framing uses silence: the console leaves at least 300 us without clocks before
an exchange. PIO runs at 25 MHz, polls CLK at most 160 ns apart, and raises a
blocking gap IRQ after 1602 cycles (64.08 us at the default 125 MHz sys clock).
Both a gap and RX DMA completion capture the burst and restart PIO and DMA,
copying the pending TX frame. Idle gaps refresh TX after application updates;
an active exchange keeps its own TX copy. A complete frame requires all 64
RX DMA bytes. Short bursts, including the 16-clock auto-joypad burst (LATCH
high, about 12 us between clocks), are discarded. Empty idle timeouts count
as gaps, not short frames. Clock stats retain the last nonempty burst's count,
including partial bytes. RX queue overflow increments overruns. Excess clocks
beyond 512 start another burst; the console must obey the exchange length.
The initial full-firmware output is a SYN frame.

For cable validation, flash `build/snos_wiretest.uf2`. This standalone target
shares the same PIO and port driver, with no WiFi, relay, or LED support. Both
UART0 (GP0 TX / GP1 RX, 115200 baud) and USB CDC stdio are enabled. TX byte 0
starts at zero and increments modulo 256 after each complete received frame;
bytes 1..63 repeat `ABCDEFGHIJ`, starting with `A`. RX byte 0 is the console
counter; bytes 1..63 must repeat `1234567890`, starting with `1`.

Once per second it prints:

```text
[WIRE] frames=N ok=M bad=K short=S gaps=G snes#=C last=<text>
```

`frames` counts complete frames processed, `ok`/`bad` count matching/mismatched
RX patterns, `short` counts discarded nonempty short bursts, and `gaps` counts
all silence timeouts (including idle ones). `snes#` is the last console counter;
`last` shows bytes 1..31 of the last complete frame, replacing nonprintable
bytes with dots. Every bad frame also produces one `[WIRE] bad=... hex=...`
line containing all 64 bytes. Counter continuity is not part of the pattern
check. Before the first frame, the counter is zero and the text is all dots.

The link queue holds 32 chunks of up to 1502 bytes; IP enqueue includes the
little-endian length prefix. Incoming relay data waits when this queue is full.
The TCP relay buffers 16 KiB in each direction. Outbound IP overflow is logged;
control forward enqueue failure returns S2C_ERR. A SNES SYN episode clears the
old link queue and reconnects the relay to release the old NAT session. WiFi
outages suspend relay connections; while WiFi is up, relay failures retry
with a 1–30 second backoff. Firmware has not been
validated on physical hardware; timing and electrical checks remain necessary.

Host-only checks (requires a native C compiler; wire test also needs the build):

```sh
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/link_test.py
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/ctrl_test.py
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/wire_test.py
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/config_test.py
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/hidkeys_test.py
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/hogp_test.py
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/console_log_test.py
PYTHONPYCACHEPREFIX="$PWD/pico/build/pycache" python3 pico/tests/ble_pairlog_test.py
```

The first compares 10,000 seeded exchanges to `gateway.py` at 64 bytes,
including checksum rejection, duplicate frames, SYN episodes, sequence wrap,
and HELLO priority. The second checks fragmented control commands and layouts. The third models
the assembled PIO instructions across 50 pulse phases, including MSB-first
TX/RX, short joypad bursts, partial bytes, and the gap/polling cycle counts.

The console log check compiles `main.c` for the host with stubbed hardware and
checks the log levels (`info` at boot), that replies print at every level, the WiFi join window, and that every line carries a listed prefix; the
pairing log check does the same for `btkbd.c`'s pairing window.

The config check mocks flash erase/program and interrupt masking, checking
version validation, persistence, string bounds, and clearing.
