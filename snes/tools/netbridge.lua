-- Tier 2: carry link frames between the emulator and a real gateway.
--
-- UNVERIFIED. Everything in M6 is tested hermetically against a Lua gateway
-- that speaks the same protocol; this script and tools/gateway.py are the path
-- to the actual internet, and neither can be exercised headlessly. Treat the
-- first run as bring-up.
--
-- HOW TO RUN IT
--   1. Enable Debug -> Script Window -> "Allow access to I/O and OS functions".
--      This is OFF by default and off in Ricardo's settings; turning it on
--      grants file and process access to ANY Lua script you subsequently run,
--      so turn it back off when you are done.
--   2. python3 snes/tools/gateway.py        (leave it running)
--   3. Load snos.sfc in the Mesen GUI, then load and Run this script.
--   4. In SNOS:  net      -> should say the gateway is up
--                get example.com
--
-- WHY FILES RATHER THAN A SOCKET OR A PIPE
--   Mesen's Lua has no socket library — `require("socket")` fails even with
--   I/O access enabled — so the process that opens sockets has to be a separate
--   one, and something has to carry bytes to it. A FIFO would be the obvious
--   choice and is the wrong one: a blocking read inside an endFrame callback
--   freezes the emulator. Reading a regular file at a tracked offset returns
--   whatever is there and never blocks, which is the property that matters.
--
--   The two files grow for the length of a session. That is fine for a dev
--   bridge and is why they are truncated at startup.
--
-- WHY THE EXCHANGE HANGS OFF MEMORY CALLBACKS AND NOT endFrame
--   It used to be one endFrame callback doing everything, which put a hard 60 Hz
--   ceiling on the link — and that ceiling was a property of the callback alone.
--   Not of the protocol, not of the SNES, and not of the Pico that replaces this
--   script at M7: a real cable is SNES-clocked and answers in microseconds, so
--   many exchanges fit in one video frame. Hanging the data path off the
--   console's own mailbox accesses instead makes the rig MORE like the cable, not
--   less — the bridge now answers whenever the console asks, at whatever rate it
--   asks, and it answers within the frame rather than at the end of it.
--
--   There are two callbacks because a poll is two events, and WHICH ACCESS each
--   one hangs off is worth a round trip apiece (measured; see each in place):
--
--     read of  LINK_MB_MAGIC     ($7EE000) -> hand a frame TO the console
--     write of LINK_MB_SNES_SEQ  ($7EE002) -> take the frame FROM the console
--
--   A poll is net_service(), and it touches the mailbox in this order every time:
--
--     net_up() -> link_present() -> link_wram_present()   reads LINK_MB_MAGIC
--     link_poll() -> link_wram_poll()                     reads LINK_MB_HOST_SEQ,
--                                                         copies RX, writes TX,
--                                                         bumps LINK_MB_SNES_SEQ
--
--   The magic read is therefore the FIRST thing a poll does, and loading the
--   mailbox there is what makes the console see the frame on the poll that asked
--   for it. Hooking the HOST_SEQ read instead — which is what this script did
--   between c64d8c3 and now — looks equivalent and is not; see the note on the
--   read callback below for the measurement that killed it.
--
--   The frame mechanism is untouched: still one frame per poll in each direction,
--   still LINK_FRAME bytes, still seq/ack and stop-and-wait. Only the relay moved.
--
--   Measured end to end against tools/gateway.py and a real VNC server, emulator
--   held at 60 fps so the seconds are the seconds a person waits. A full 48 KB
--   `vnc` screen: 3470 frames (57.8 s) at the endFrame relay, 2660 (44.3 s) with
--   the HOST_SEQ hook, and see the read-callback note for where it is now. The
--   8 KB `get file:///tmp/bench.bin` benchmark was already at the protocol's
--   2-poll floor and must stay there — it is the control that catches a change
--   that only helps by luck.

local UP = "/tmp/snos-up.bin"     -- SNES -> gateway
local DOWN = "/tmp/snos-down.bin" -- gateway -> SNES

local MB = 0xE000                 -- link mailbox, WRAM $7EE000
local MAGIC = 0x4B4C              -- "LK"
local FRAME = 128
-- Mailbox buffer offsets are DERIVED, never literals: they used to be 0x10 and
-- 0x50, which is exactly FRAME apart at 64, so a bigger frame overlapped them
-- silently. See the note on LINK_MB_RX in kernel/link.h.
local TXOFF, RXOFF = 0x10, 0x10 + FRAME
local WRAM = emu.memType.snesWorkRam

if type(io) ~= "table" then
  print("netbridge: I/O is disabled. Enable Script Window -> " ..
        "'Allow access to I/O and OS functions' and run this again.")
  return
end

-- Truncate both directions so a previous session's traffic is not replayed.
local up = io.open(UP, "wb")
if not up then print("netbridge: cannot write " .. UP); return end
up:close()
up = io.open(UP, "ab")
local down_off = 0
do
  local f = io.open(DOWN, "wb")
  if f then f:close() end
end

-- The read side is opened ONCE and held, rather than reopened per exchange.
-- The exchange now runs on every poll the console makes rather than once a video
-- frame, and an open()/close() pair per poll is a syscall bill that scales with
-- the poll rate for no benefit. Two lseeks and a read is the whole cost now.
--
-- Holding the handle is safe because both ends only ever truncate this path,
-- never replace the inode: this script truncates it at startup and gateway.py
-- opens it "ab" and appends. A truncation is visible through a held handle as a
-- shrinking size, which is exactly what the restart check below tests for.
local downf = io.open(DOWN, "rb")
if not downf then
  print("netbridge: cannot read " .. DOWN .. " - the link would be send-only")
  return
end

local function rb(o) return emu.read(MB + o, WRAM) end
local function wb(o, v) emu.write(MB + o, v & 0xFF, WRAM) end
local function ru16(o) return emu.read16(MB + o, WRAM) end
local function wu16(o, v) emu.write16(MB + o, v & 0xFFFF, WRAM) end

-- $7EE000 now carries a read callback, and this script reads it too — twice per
-- frame, from code that must not deliver a frame as a side effect. Whether a Lua
-- read reaches memory callbacks at all is a Mesen detail I would rather not depend
-- on either way, so say it explicitly: reads taken through here are the script's
-- own and deliver() ignores them.
local probing = false
local function magic()
  probing = true
  local v = ru16(0x00)
  probing = false
  return v
end

local claimed = false
local synced = false
local rx_pending = true
local sent, received, skipped = 0, 0, 0

print("netbridge: up=" .. UP .. " down=" .. DOWN)
print("netbridge: start tools/gateway.py, then 'net' in SNOS")

-- gateway -> SNES. One frame into the mailbox, and not one more until the console
-- has taken it. Registered on the console's READ of LINK_MB_MAGIC, further down —
-- read the note there for why that particular access and not another.
--
-- One frame per poll and no more, exactly like the cable: handing over several at
-- once would let this backend outrun what the hardware can do and hide timing
-- bugs that M7 would then inherit.
--
-- `rx_pending` is the flow control that enforces it, and the bridge had none at
-- all before — it wrote a frame into the mailbox every video frame whether or not
-- the console had read the last one. docs/performance.md records that exact hole
-- as the reason a windowed gateway stalled at 2 KB: "the host must not write a
-- new frame until the SNES has consumed the previous one".
--
-- It starts TRUE so nothing is delivered until a real poll has been seen, and the
-- send callback clears it. THAT IS ONLY SOUND BECAUSE OF WHERE THIS IS HOOKED:
-- the frame is in the mailbox with LINK_MB_HOST_SEQ already bumped before the
-- console reads HOST_SEQ, so a poll that reaches the SNES_SEQ write has certainly
-- copied it. Hooked on the HOST_SEQ read instead, that inference was FALSE and
-- half of every session's frames were destroyed unread — measured, 2026-07-30,
-- 1437 of 2873. See the read-callback note.
local function deliver()
  if probing or rx_pending or not downf then return end

  -- The far end was restarted and emptied the file under us. Without this the
  -- link is dead forever after the first gateway restart: we keep reading at an
  -- offset past the end of a file that now begins again at zero, so not one frame
  -- is ever seen and no amount of reloading this script helps, because the stale
  -- offset is only reset when the emulator itself restarts.
  local size = downf:seek("end")
  if size < down_off then down_off = 0 end

  -- Join the stream where it currently is, once per session.
  --
  -- Nothing written before the console's first frame of this session can be an
  -- answer to it, so skipping to the end drops nothing. It is exactly what
  -- gateway.py does with UP, for the same reason (see its main()).
  --
  -- It matters here because truncating both files can leave one frame behind:
  -- the gateway appends its answer to a pre-truncation UP frame after DOWN has
  -- already been emptied, and only then notices UP shrank and resets. A leftover
  -- frame is a permanent poll of latency on everything that follows it.
  --
  -- Rounding UP to a frame boundary, not just taking `size`: the gateway's
  -- 128-byte append is one write() but this can still stat the file in the middle
  -- of it, and joining the stream at a byte that is not a multiple of FRAME
  -- misframes every frame of the session after it — garbage payloads, failing
  -- checksums, a link that is up and carries nothing.
  if not synced then
    synced = true
    down_off = math.ceil(size / FRAME) * FRAME
  end

  local avail = math.floor((size - down_off) / FRAME)
  if avail < 1 then return end

  -- HAND OVER THE NEWEST FRAME AND DROP THE ONES BEHIND IT.
  --
  -- The mailbox is one frame deep, so the relay has to behave like one frame of
  -- buffering. Reading the DOWN file strictly in order instead turns it into an
  -- unbounded FIFO in front of a 1-deep mailbox, and that queue is a RATCHET: the
  -- gateway writes exactly one frame per frame it reads (its `if moved` branch),
  -- the console polls once per frame, so production and consumption are locked at
  -- the same rate and the depth can never come back down. One poll that arrives
  -- before the gateway has answered the previous one — measured: poll 310 of a
  -- `vnc` screen, because the console had polled twice inside one video frame and
  -- the gateway's answer was 20 us behind — adds a frame to the queue for the rest
  -- of the session. That cost a poll of latency on all 2565 exchanges that
  -- followed, and it was 25% of the round trip.
  --
  -- Dropping the skipped frames is safe, and it is safe because of stop-and-wait
  -- rather than in spite of it: the gateway will not build a new data frame until
  -- the console acks the last one, so AT MOST ONE frame in the queue can be
  -- carrying payload the console has not seen. Everything behind the newest is a
  -- retransmission of it or an older ack. Losing those is what a lossy link does
  -- and the protocol already recovers from it — whereas delivering them is not
  -- "reliable", it is just late.
  if avail > 1 then
    if skipped == 0 then
      print(("netbridge: DOWN was %d frames deep; handing over the newest and " ..
             "dropping the rest (said once)"):format(avail))
    end
    skipped = skipped + avail - 1
    down_off = down_off + (avail - 1) * FRAME
  end

  -- Seeking before every read is not redundant with holding the handle open: it
  -- is what discards stdio's buffer and its EOF flag, so a frame another process
  -- appended a moment ago is actually seen.
  downf:seek("set", down_off)
  local chunk = downf:read(FRAME)
  if not (chunk and #chunk == FRAME) then return end

  down_off = down_off + FRAME
  for i = 0, FRAME - 1 do wb(RXOFF + i, chunk:byte(i + 1)) end
  wu16(0x04, ru16(0x04) + 1)
  rx_pending = true
  received = received + 1
end

-- The console has published a frame. Put it on the wire.
local function exchange()
  -- The console only polls while the magic is present, so this is normally
  -- true. It matters at boot: link_wram_init() zeroes LINK_MB_SNES_SEQ, and that
  -- write reaches this callback like any other. Without the gate the very first
  -- thing the bridge would send is 64 bytes of uninitialised WRAM.
  if magic() ~= MAGIC then return end

  -- Whatever was in the mailbox has been read: see the note on rx_pending above.
  rx_pending = false

  -- SNES -> gateway.
  --
  -- Safe by construction, and this is the linchpin of the whole change:
  -- link_wram_poll() (kernel/link_wram.c) copies the entire 64-byte frame into
  -- LINK_MB_TX and only THEN bumps the sequence counter, so a callback on the
  -- counter cannot see a half-written frame.
  local bytes = {}
  for i = 0, FRAME - 1 do bytes[#bytes + 1] = string.char(rb(TXOFF + i)) end
  up:write(table.concat(bytes))
  up:flush()
  sent = sent + 1
end

-- $7EE000 is LINK_MB_MAGIC, and a poll's FIRST mailbox access is net_up() reading
-- it. Loading the mailbox here rather than at the end of the previous poll is
-- worth a whole round trip; loading it here rather than on the HOST_SEQ read a few
-- instructions later is worth another one, and that second round trip is the
-- entire reason this callback moved on 2026-07-30.
--
-- WHY NOT THE HOST_SEQ READ, WHICH IS WHERE THIS USED TO SIT
--   link_wram_poll() copies RX only `if (LINK_MB_HOST_SEQ != last_host_seq)`, so
--   the obvious hook is that read: answer the question in the instant it is asked.
--   It does not work, and the reason is the one Mesen fact that is the mirror
--   image of the one documented on the write callback below:
--
--     a READ callback fires AFTER the value the CPU gets has been taken.
--
--   So bumping HOST_SEQ from inside that read is invisible to it. The console
--   compares the PREVIOUS value, finds it equal to what it latched last poll, and
--   skips the copy — while this script has already overwritten RX. Then the next
--   poll's read does see a change, copies whatever RX holds by then, and latches
--   the counter AFTER this callback has bumped it again, so the poll after that
--   compares equal and skips. The result is an exact alternation — measured, one
--   `vnc` screen: 2872 of 2875 consecutive poll pairs alternate, 1437 of 2873
--   frames handed over were never read, and 406 of 409 frames carrying fresh
--   payload were among them. Every data frame therefore cost the poll it was
--   dropped on plus the poll its retransmission arrived on.
--
--   Hooking the magic read fixes it because the two accesses are separate
--   instructions: the bump lands in memory before link_wram_poll() reads HOST_SEQ,
--   the change test sees it, and the console copies the frame that was written
--   microseconds earlier in the same poll. Every poll then consumes exactly one
--   frame and nothing is dropped. (Measured, same run: a callback write IS visible
--   to a later CPU read in the same video frame — poll 309 latched a value this
--   script had written during poll 309.)
--
-- WHY THE MAGIC READ IS A LEGITIMATE HOOK AND NOT A TRICK
--   It is the same access the console uses to decide whether the link exists at
--   all, i.e. the first thing it does when it starts clocking an exchange. A real
--   cable is synchronous: the peer presents whatever it has buffered at the moment
--   the console begins the transfer. Loading the mailbox on the poll's first
--   access is as close to that as a mailbox gets — closer than loading it two
--   accesses later, and much closer than a video-frame boundary.
--
--   link_present() is called from a few places besides net_service(), so this can
--   fire without a poll following it. That costs nothing: the frame sits in the
--   mailbox, rx_pending keeps the next one out, and the next real poll takes it.
emu.addMemoryCallback(deliver, emu.callbackType.read, 0x7EE000, 0x7EE000,
                      emu.cpuType.snes)

-- $7EE002 is LINK_MB_SNES_SEQ, the counter the console bumps to say "a frame is
-- in the mailbox".
--
-- A CPU-bus long address works and needs no memType argument. Checked against
-- this Mesen build with a bank-$00 control at $00E002 that never fired, so the
-- bank is genuinely honoured rather than masked off.
--
-- WHY THE LOW BYTE ONLY, AND WHY THERE IS NO CHANGE DETECTION
--   Two measured facts about Mesen decide this, and getting either wrong costs a
--   frame of latency on every exchange for the whole session:
--
--   1. The callback runs BEFORE the write reaches memory. Reading the counter
--      here returns the value it had a moment ago, so a `read it back and see if
--      it changed` guard is reading the wrong thing.
--   2. A 16-bit store is two bus writes, low byte first.
--
--   Together those make the obvious guard misfire. It compares against the
--   counter, so it stays quiet on the low-byte write (nothing has changed yet)
--   and fires on the high-byte write — latching a value whose HIGH byte is still
--   stale. Every 256th poll the high byte really does change, the stale latch no
--   longer matches, and the exchange runs twice for one poll. Twice for one poll
--   means two frames sent and two answers queued for one the console can accept,
--   and the two ends are rate-locked — gateway.py writes one frame to DOWN per
--   frame it reads from UP, and this reads one per poll, which is one per frame it
--   writes to UP — so the extra frame of queue is never given back. It is a poll
--   of extra latency on every exchange from then until the session ends.
--
--   Keying on the low byte alone and acting unconditionally avoids all of it.
--   The counter is only ever written by the sequence bump in link_wram_poll() and
--   the zeroing in link_wram_init(), and each of those writes the low byte
--   exactly once, so this fires exactly once per poll — with no assumption about
--   which byte the compiler emits first, and none about when Mesen applies it.
emu.addMemoryCallback(exchange, emu.callbackType.write, 0x7EE002, 0x7EE002,
                      emu.cpuType.snes)

-- endFrame keeps the claim, and it HAS to stay here.
--
-- link_init() ZEROES this mailbox, and it runs from shell_run() — that is, after
-- this script has already started. A one-shot claim therefore gets wiped moments
-- later and the SNES reports "no gateway attached" for the rest of the session.
-- Rewriting it is idempotent, costs one compare per frame, and also survives a
-- console reset.
--
-- It cannot move into the memory callback either, and the failure would be
-- total: the console does not touch the link at all without the magic present,
-- so nothing would ever bump the sequence counter, the callback would never
-- fire, and the link would be dead for the whole session.
--
-- It deliberately does NOT deliver. Loading the mailbox on a frame boundary
-- would hand the console whatever had arrived by then and lock out the read
-- callback above, which fires later — at the poll itself — and therefore always
-- has something at least as fresh.
emu.addEventCallback(function()
  if emu.read16(0xF000, WRAM) ~= 0xCAFE then return end -- kernel not up yet
  if magic() ~= MAGIC then
    wu16(0x00, MAGIC)
    -- The magic being absent means the console zeroed the mailbox, i.e. this is
    -- a new session on its side — either the first one or a reset. Either way
    -- whatever is queued in the DOWN file answers a session that no longer
    -- exists, so let the next delivery skip past it, and deliver nothing at all
    -- until a poll has proved the console is running the link.
    synced = false
    rx_pending = true
    if not claimed then
      claimed = true
      print("netbridge: link claimed")
    end
  end
end, emu.eventType.endFrame)
