-- M6: the gateway, implemented in Lua.
--
-- This script is not a stub standing in for a gateway — it speaks the real
-- protocol, frame for frame, with the same sequence numbers, the same checksum
-- and the same stop-and-wait rules the cable will use. That fidelity is the
-- point: everything above the link is genuinely exercised now, so M7 replaces a
-- bit-banger rather than discovering a stack's worth of bugs with a soldering
-- iron in hand.
--
-- It also does something no real cable can be made to do on demand: drop a
-- frame and corrupt a checksum, deterministically, at a chosen moment. Those
-- are the paths that matter most and are otherwise never exercised, because
-- terminating TCP at the gateway makes this hop the only unprotected one and it
-- carries the entire payload.
local L = dofile("test/lib.lua")

local MB = 0xE000          -- link mailbox, WRAM $7EE000
local MAGIC = 0x4B4C       -- "LK"
local FRAME, HDR = 64, 4
local SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
local F_DATA = 0x01
local F_SYN = 0x02
local CH_CTRL, CH_PAD = 0, 0xFF
local WRAM = emu.memType.snesWorkRam

local C2S_FETCH, C2S_CLOSE, C2S_IMAGE, C2S_HELLO = 0x01, 0x02, 0x03, 0x04
local C2S_CONNECT, C2S_LISTEN, C2S_HANGUP = 0x07, 0x08, 0x09
local S2C_HELLO = 0x84
local S2C_OPEN, S2C_EOF, S2C_ERR = 0x81, 0x82, 0x83

local function rb(o) return emu.read(MB + o, WRAM) end
local function wb(o, v) emu.write(MB + o, v & 0xFF, WRAM) end
local function ru16(o) return emu.read16(MB + o, WRAM) end
local function wu16(o, v) emu.write16(MB + o, v & 0xFFFF, WRAM) end

local function sum_of(f)
  local s = 0
  for i = 0, FRAME - 1 do
    if i ~= SUM then s = s + f[i] end
  end
  return s & 0xFF
end

-- ---------------------------------------------------------------- gateway
local gw = {
  tx_seq = 0, rx_next = 0, awaiting = false, inflight = nil, inflight_len = 0,
  last_snes_seq = 0, out = {},            -- queue of {ch=, data=} to send
  cmsg = { state = 0, typ = 0, len = 0, buf = {} },
  window = {},        -- frames sent but not yet acknowledged
  stall = 0,          -- polls since the window head last moved
  drop_snes = 0,      -- ignore this many incoming frames (simulates loss)
  drop_out = 0,       -- discard this many outgoing frames (loss toward the SNES)
  corrupt_out = 0,    -- send this many frames with a broken checksum
  fetches = {},
  listens = {},      -- ports the SNES asked to bind
  hangups = 0,       -- connections the app ended itself
  upstream = {},     -- bytes the SNES SENT, per channel
  images = {},       -- urls seen, for assertions
}

-- Canned responses. The gateway has already done DNS, TLS, redirects, chunked
-- decoding and gzip, so a body here is exactly what the SNES will see.
local BODY_OK = table.concat({
  "SNOS fetched this through a gateway.",
  "TLS was terminated on the far side, which is the only reason",
  "this works at all: a handshake on a 3.58 MHz 65816 runs to minutes.",
  "The SNES saw plain bytes and a status code, and nothing else.",
}, "\n") .. "\n"
local BODY_404 = "not found\n"

-- A canned .IMG: two tiles by two, with a palette and tile bytes chosen so a
-- wrong offset or a transposed copy cannot accidentally look right.
--   palette entry i = i          -> CGRAM byte 2i is i, 2i+1 is 0
--   tile byte j     = (j*7) & FF -> VRAM byte j is predictable
local IMG_TW, IMG_TH = 2, 2
local BODY_IMG
do
  local t = { "SIMG",
              string.char(1, 0),          -- version
              string.char(3),             -- BG mode
              string.char(0),             -- flags
              string.char(IMG_TW, 0),
              string.char(IMG_TH, 0),
              string.char(0, 0, 0, 0) }   -- reserved
  for i = 0, 255 do t[#t + 1] = string.char(i, 0) end
  for j = 0, IMG_TW * IMG_TH * 64 - 1 do t[#t + 1] = string.char((j * 7) & 0xFF) end
  BODY_IMG = table.concat(t)
end

-- Big enough that a download of it is still in flight several seconds later,
-- which is what makes the abort path testable at all. At ~1.7 KB/s this is a
-- couple of seconds of transfer.
local BODY_BIG = string.rep("the quick brown fox jumps over the lazy dog 0123\n", 200)

local function push(ch, s)
  gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 }
end

local function push_ctrl(typ, payload)
  push(CH_CTRL, string.char(typ, #payload) .. payload)
end

-- The flags byte on S2C_OPEN says "this body is Mode 3 tile data". A real
-- gateway sets it from the response's Content-Type; this one keys off the URL,
-- which is the same decision made from the only information a canned server
-- has. It is appended, so a three-byte reply still parses.
local F_IMAGE = 1

local function on_fetch(ch, url)
  gw.fetches[#gw.fetches + 1] = url
  if url:find("missing") then
    push_ctrl(S2C_OPEN, string.char(ch, 404 & 0xFF, (404 >> 8) & 0xFF, 0))
    push(ch, BODY_404)
  elseif url:find("big") then
    push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, (200 >> 8) & 0xFF, 0))
    push(ch, BODY_BIG)
  elseif url:find("%.jpg") then
    -- Auto-conversion: the caller asked for a JPEG and never said "image".
    gw.images[#gw.images + 1] = url
    push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, (200 >> 8) & 0xFF, F_IMAGE))
    push(ch, BODY_IMG)
  else
    push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, (200 >> 8) & 0xFF, 0))
    push(ch, BODY_OK)
  end
  push_ctrl(S2C_EOF, string.char(ch))
end

local function ctrl_byte(b)
  local c = gw.cmsg
  if c.state == 0 then
    c.typ = b; c.state = 1
  elseif c.state == 1 then
    c.len = b; c.buf = {}; c.state = (b > 0) and 2 or 0
    if c.state == 0 then c.dispatch = true end
  else
    c.buf[#c.buf + 1] = b
    if #c.buf >= c.len then c.state = 0; c.dispatch = true end
  end
  if c.dispatch then
    c.dispatch = false
    if c.typ == C2S_FETCH and #c.buf >= 1 then
      local ch = c.buf[1]
      local url = ""
      for i = 2, #c.buf do url = url .. string.char(c.buf[i]) end
      on_fetch(ch, url)
    end
    -- An image request is the same stream, opened differently: the gateway
    -- converts and the console receives tile data it can hand straight to the
    -- PPU. Nothing above the link knows the difference.
    if c.typ == C2S_IMAGE and #c.buf >= 1 then
      local ch = c.buf[1]
      local url = ""
      for i = 2, #c.buf do url = url .. string.char(c.buf[i]) end
      gw.images[#gw.images + 1] = url
      push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, 0))
      push(ch, BODY_IMG)
      push_ctrl(S2C_EOF, string.char(ch))
    end
    -- Liveness probe, answered unconditionally like the real daemon so that
    -- `net status` can tell "nothing behind the bridge" from "there and
    -- working". Queued like any other control record, not jumped ahead.
    if c.typ == C2S_LISTEN and #c.buf >= 3 then
      local ch = c.buf[1]
      gw.listens[#gw.listens + 1] = c.buf[2] | (c.buf[3] << 8)
      -- Accept immediately, then half-close: the client says everything it
      -- intends to and waits to read, which is what every HTTP client does and
      -- what broke two layers of this when it was first written.
      push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, 0, 0))
      push(ch, "ping over tcp\n")
      push_ctrl(S2C_EOF, string.char(ch))
    end
    if c.typ == C2S_HANGUP and #c.buf >= 1 then gw.hangups = gw.hangups + 1 end
    if c.typ == C2S_HELLO then
      -- Version, then the gateway's own address as text. The SNES has no
      -- address, so this is what anything it serves would be reached at.
      push_ctrl(S2C_HELLO, string.char(1) .. "10.0.0.1")
    end
    -- C2S_CLOSE: drop anything still queued for that channel. A real gateway
    -- closes the socket and stops reading; a canned one that keeps streaming an
    -- abandoned body starves every later request of the send window, which is
    -- exactly what an aborted download does.
    if c.typ == C2S_CLOSE and #c.buf >= 1 then
      local ch = c.buf[1]
      local i = 1
      while i <= #gw.out do
        if gw.out[i].ch == ch then table.remove(gw.out, i) else i = i + 1 end
      end
    end
  end
end

local function deliver(f)
  local o = HDR
  while o + 2 <= FRAME do
    local ch, len = f[o], f[o + 1]
    if ch == CH_PAD then break end
    if o + 2 + len > FRAME then break end
    if ch == CH_CTRL then
      for i = 0, len - 1 do ctrl_byte(f[o + 2 + i]) end
    else
      -- Stream bytes FROM the SNES. Until the upstream path existed this branch
      -- had nothing to do: every byte the console sent was a control record.
      gw.upstream[ch] = (gw.upstream[ch] or "")
      for i = 0, len - 1 do
        gw.upstream[ch] = gw.upstream[ch] .. string.char(f[o + 2 + i])
      end
    end
    o = o + 2 + len
  end
end

local function build()
  local f = {}
  for i = 0, FRAME - 1 do f[i] = 0 end
  f[SEQ] = gw.tx_seq
  f[ACK] = (gw.rx_next - 1) & 0xFF
  f[FLAGS] = 0

  local o = HDR
  local any = false
  while #gw.out > 0 and o + 2 < FRAME do
    local item = gw.out[1]
    local room = FRAME - o - 2
    local left = #item.data - item.pos + 1
    local n = (left < room) and left or room
    if n <= 0 then break end
    f[o] = item.ch; f[o + 1] = n
    for i = 0, n - 1 do f[o + 2 + i] = item.data:byte(item.pos + i) end
    o = o + 2 + n
    item.pos = item.pos + n
    if item.pos > #item.data then table.remove(gw.out, 1) end
    any = true
  end
  while o < FRAME do f[o] = CH_PAD; o = o + 1 end
  if any then f[FLAGS] = f[FLAGS] | F_DATA end
  f[SUM] = 0
  f[SUM] = sum_of(f)
  return f, any
end

-- Sequence comparison, modulo 256: is a at or before b?
local function seq_le(a, b) return ((b - a) & 0xFF) < 128 end

local function write_frame(f)
  -- Loss in the SNES-bound direction. Distinct from drop_snes, and it is the
  -- case a window introduces: with stop-and-wait a lost frame simply stalls,
  -- but a pipelined sender has later frames already on the wire that the
  -- receiver must refuse until the gap is filled.
  if gw.drop_out > 0 then
    gw.drop_out = gw.drop_out - 1
    wu16(0x04, ru16(0x04) + 1)   -- the exchange still happened; the frame is gone
    return
  end
  local corrupt = gw.corrupt_out > 0
  if corrupt then gw.corrupt_out = gw.corrupt_out - 1 end
  for i = 0, FRAME - 1 do
    local v = f[i]
    if corrupt and i == SUM then v = (v + 1) & 0xFF end
    wb(0x50 + i, v)
  end
  wu16(0x04, ru16(0x04) + 1)
end

local function service()
  -- Incoming
  local ss = ru16(0x02)
  -- Has the SNES run an exchange since we last looked? Everything below is
  -- gated on this. The mailbox holds ONE frame per direction — exactly like the
  -- cable's single synchronous exchange — so writing again before the SNES has
  -- polled destroys a frame it never saw. Stop-and-wait hid this by rewriting
  -- the same frame; a window does not, and the SNES skips polls whenever the
  -- console is mid-repaint.
  local polled = (ss ~= gw.last_snes_seq)
  if polled then
    gw.last_snes_seq = ss
    if gw.drop_snes > 0 then
      gw.drop_snes = gw.drop_snes - 1     -- pretend the frame never arrived
    else
      local f = {}
      for i = 0, FRAME - 1 do f[i] = rb(0x10 + i) end
      if f[SUM] == sum_of(f) then
        -- Cumulative ack: release every window frame up to and including it.
        while #gw.window > 0 and seq_le(gw.window[1].seq, f[ACK]) do
          table.remove(gw.window, 1)
          gw.stall = 0
        end
        if (f[FLAGS] & F_DATA) ~= 0 and f[SEQ] == gw.rx_next then
          deliver(f)
          gw.rx_next = (gw.rx_next + 1) & 0xFF
        end
      end
    end
  end

  -- Outgoing.
  --
  -- A window rather than stop-and-wait. The link is a synchronous exchange, so
  -- an acknowledgement cannot ride in the exchange that carried the frame it
  -- acknowledges; a sender that waits for one therefore moves a frame every
  -- other poll and gives up half the link. The SNES already accepts a pipelined
  -- sender — it takes frames strictly in order and re-states a cumulative ack
  -- on every frame it sends — so this is the only side that had to change.
  --
  -- Go-back-N: on a gap the receiver refuses everything after it, so the whole
  -- window is resent from the head.
  local WIN = 4
  -- Must exceed the window depth plus the round trip, or normal pipelining
  -- looks like a stall: with a window of 4 and a 2-poll round trip, a frame
  -- legitimately sits unacknowledged for ~6 polls while the window fills.
  -- Resending at 3 fired go-back-N constantly and HALVED throughput.
  local RESEND_AFTER = 12

  if not polled then return end

  gw.stall = gw.stall + 1
  if #gw.window > 0 and gw.stall > RESEND_AFTER then
    gw.stall = 0
    local head = gw.window[1]
    head.frame[ACK] = (gw.rx_next - 1) & 0xFF
    head.frame[SUM] = 0
    head.frame[SUM] = sum_of(head.frame)
    write_frame(head.frame)
  elseif #gw.window < WIN then
    local f, any = build()
    if any then
      gw.window[#gw.window + 1] = { seq = gw.tx_seq, frame = f }
      gw.tx_seq = (gw.tx_seq + 1) & 0xFF
    end
    write_frame(f)
  else
    -- Window full and nothing due for resend: still answer the exchange so the
    -- SNES keeps receiving our ack.
    local f = {}
    for i = 0, FRAME - 1 do f[i] = 0 end
    f[SEQ] = gw.tx_seq
    f[ACK] = (gw.rx_next - 1) & 0xFF
    f[FLAGS] = 0
    local o = HDR
    while o < FRAME do f[o] = CH_PAD; o = o + 1 end
    f[SUM] = 0
    f[SUM] = sum_of(f)
    write_frame(f)
  end
end

-- ------------------------------------------------------------------ test
local frame, step, at = 0, 1, nil
local claimed = false
local img_cg, img_vr

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL net - no mailbox magic"); emu.stop(1) end
    return
  end

  -- Claim the link only once the kernel has zeroed it, otherwise the magic is
  -- wiped out from under us by net_init().
  if not claimed then
    if frame < 200 then return end
    wu16(0x00, MAGIC)
    claimed = true
    at = frame + 60
    return
  end

  service()

  if frame < at then return end

  -- Bare `net` now lists its subcommands rather than meaning `status`.
  if step == 1 then L.feed("net status\n"); at = frame + 150; step = 2; return end
  if step == 2 then L.feed("get http://example.com/ok -\n"); at = frame + 900; step = 3; return end
  if step == 3 then L.feed("get http://example.com/missing -\n"); at = frame + 600; step = 4; return end
  if step == 4 then
    -- Now the paths a real cable cannot be asked to exercise: lose two frames
    -- from the SNES, and hand it two frames with broken checksums. Both must
    -- recover invisibly and the body must arrive byte-perfect.
    gw.drop_snes = 2
    gw.corrupt_out = 2
    L.feed("get http://example.com/faults -\n")
    at = frame + 1200
    step = 5
    return
  end

  -- The same stream, written to the SRAM volume instead of the screen.
  if step == 5 then
    L.feed("get http://example.com/ok DLTEST.TXT\n")
    at = frame + 1200
    step = 6
    return
  end
  -- Read it back off the volume. This is the assertion that matters: it proves
  -- the bytes reached storage, not merely that they crossed the link.
  if step == 6 then L.feed("cat DLTEST.TXT\n"); at = frame + 600; step = 7; return end

  -- A download long enough to interrupt. The kill switch only reaches
  -- applications, and a shell command is not one, so without a working abort a
  -- wrong URL means power-cycling the machine.
  if step == 7 then
    L.feed("get http://example.com/big BIG.TXT\n")
    -- Interrupt while it is genuinely still running. At roughly 1.4 KB/s this
    -- lands about a kilobyte into a ten-kilobyte body; the first version aborted
    -- after the transfer had already finished and proved nothing.
    at = frame + 90
    step = 8
    return
  end
  if step == 8 then L.feed(" "); at = frame + 300; step = 9; return end
  -- A gap INSIDE the window: drop two gateway-bound frames mid-transfer. Only a
  -- pipelined sender can produce this — with stop-and-wait a loss simply stalls,
  -- whereas here later frames are already on the wire and the receiver must
  -- refuse them until the hole is filled, then go-back-N repairs it.
  if step == 9 then
    gw.drop_out = 2
    L.feed("get http://example.com/ok WINLOSS.TXT\n")
    at = frame + 1500
    step = 10
    return
  end
  if step == 10 then L.feed("cat WINLOSS.TXT\n"); at = frame + 600; step = 11; return end
  -- "partial file kept" is a claim; check the volume actually has it.
  if step == 11 then L.feed("ls\n"); at = frame + 300; step = 11.5; return end
  -- Printing tile data to the console is refused rather than sprayed.
  if step == 11.5 then
    L.feed("get http://example.com/pic.jpg -\n")
    at = frame + 900
    step = 12
    return
  end

  -- The image path: fetch a converted picture, store it, and display it. The
  -- assertions below read CGRAM and VRAM directly, because whether a picture
  -- actually reached the PPU is not something the console can be asked.
  if step == 12 then
    -- No `image` command any more, and no flag: the URL ends .jpg, the gateway
    -- notices and converts, and the name is forced to .IMG because the bytes
    -- are no longer a JPEG.
    L.feed("get http://example.com/pic.jpg TINY\n")
    at = frame + 1200
    step = 13
    return
  end
  if step == 13 then L.feed("run view TINY.IMG\n"); at = frame + 400; step = 14; return end
  if step == 14 then
    -- Sample while the picture is still on screen; the viewer holds until a key.
    img_cg = {}
    img_vr = {}
    for i = 0, 15 do img_cg[i] = emu.read(i, emu.memType.snesCgRam) end
    for i = 0, 15 do img_vr[i] = emu.read(i, emu.memType.snesVideoRam) end
    L.feed(" ")                       -- any key leaves the viewer
    at = frame + 400
    step = 15
    return
  end

  -- Leaked-channel reclamation. A keypress abort is NOT the case that matters:
  -- `get` still returns through its own cleanup and closes politely. The kill
  -- switch is the one that leaks, because it unwinds the application without
  -- running another instruction of it -- which is exactly what it is for.
  if step == 15 then
    L.feed("get http://example.com/big LEAK.TXT\n")
    at = frame + 90                  -- mid-transfer, while a channel is open
    step = 16
    return
  end
  if step == 16 then
    emu.write(0xF00E, 1, WRAM)       -- MB_BREAK: the kill switch, as F12 does
    at = frame + 400
    step = 17
    return
  end
  if step == 17 then L.feed("net status\n"); at = frame + 300; step = 18; return end
  -- The server path: bind a port, accept, echo, hang up. This is the only test
  -- of the SNES SENDING stream bytes rather than asking for them.
  if step == 18 then L.feed("netecho 7777\n"); at = frame + 900; step = 19; return end
  if step == 19 then L.feed(" "); at = frame + 400; step = 20; return end

  local r = L.new_report("net")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  print(string.format("gateway saw %d fetches, resends left: drop=%d corrupt=%d",
        #gw.fetches, gw.drop_snes, gw.corrupt_out))

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  -- `get` is an application now, reached without a `run` prefix.
  r.check(text:find("[get exited", 1, true) ~= nil,
          "get did not run as an application via the name fallback")
  r.check(text:find("that is a picture", 1, true) ~= nil,
          "printing a converted picture to the console was not refused")
  -- "alive" rather than "up": the status line is now a HELLO round trip, so
  -- this asserts the canned gateway *answered*, not merely that the mailbox
  -- had a magic word in it. The old wording passed in exactly the situation
  -- the probe was added to catch — a claimed link with nothing behind it.
  r.check(text:find("gateway alive", 1, true) ~= nil,
          "net status did not get a HELLO reply from the gateway")

  -- A URL that crossed the link intact proves framing and reassembly.
  r.check(#gw.fetches >= 3, "gateway received " .. #gw.fetches .. " fetches, wanted 3")
  r.check(gw.fetches[1] == "http://example.com/ok",
          "first URL arrived as " .. tostring(gw.fetches[1]))

  -- A successful fetch: status on the control channel, body on the data one.
  r.check(text:find("HTTP 200", 1, true) ~= nil, "no HTTP 200 status reported")
  r.check(text:find("TLS was terminated on the far side", 1, true) ~= nil,
          "body did not arrive intact")

  -- An error status must reach the application, not be swallowed.
  r.check(text:find("HTTP 404", 1, true) ~= nil, "404 status not reported")
  r.check(text:find("not found", 1, true) ~= nil, "404 body missing")

  -- Fault injection must have actually happened, and been survived. A
  -- retransmit that never runs and one that works look identical from outside,
  -- which is why the kernel counts them.
  r.check(gw.drop_snes == 0 and gw.corrupt_out == 0,
          "fault injection never fired")
  local _, bodies = text:gsub("TLS was terminated on the far side", "")
  r.check(bodies >= 2,
          "body survived " .. bodies .. " times; the fault-injected fetch lost data")

  -- download: the body has to reach the volume, not just the wire.
  r.check(text:find("-> DLTEST.TXT", 1, true) ~= nil, "download did not name a file")
  r.check(text:find("saved DLTEST.TXT", 1, true) ~= nil, "download did not report success")
  -- The body appears once from `get`, twice more from download+cat. If the file
  -- were empty or truncated the cat would not add an occurrence.
  local _, all = text:gsub("TLS was terminated on the far side", "")
  r.check(all >= 3,
          "cat of the downloaded file did not return the body (" .. all .. " copies seen)")

  -- abort: a long transfer must stop on a keypress and keep what it had.
  r.check(text:find("[aborted", 1, true) ~= nil,
          "a keypress did not abort the download - a stuck transfer would need a power cycle")

  -- The kill switch leaves a channel open by construction: it unwinds the app
  -- without running another instruction of it, so no amount of tidy code in the
  -- application reaches its own close. Channels are reused, so the leak is not
  -- cosmetic -- the next app handed that channel would read the dead one's
  -- bytes as its own. The kernel reclaims it and says so.
  r.check(text:find("[get killed]", 1, true) ~= nil,
          "the kill switch did not terminate the download")
  r.check(text:find("left 1 connection open - closed", 1, true) ~= nil,
          "a channel leaked by a killed app was not reclaimed")

  -- The SNES has no address of its own; this is what it would be reached at.
  r.check(text:find("gateway alive @ 10.0.0.1", 1, true) ~= nil,
          "net status did not report the gateway's address")

  -- Servers. The SNES bound a port, took a connection, and sent bytes it
  -- composed itself — none of which it could do before the stream API.
  r.check(gw.listens[1] == 7777,
          "netecho did not bind the port it was given (got " ..
          tostring(gw.listens[1]) .. ")")
  r.check((gw.upstream[1] or ""):find("ping over tcp", 1, true) ~= nil,
          "the echo server did not send the bytes back: upstream was '" ..
          tostring(gw.upstream[1]) .. "'")
  -- The client half-closed. An app that treats EOF as "give up" never replies,
  -- and one that hangs up early truncates; this asserts it did neither.
  r.check(gw.hangups >= 1,
          "the app never hung up, so the connection would have hung until timeout")
  r.check(text:find("BIG.TXT", 1, true) ~= nil,
          "the aborted download left nothing on the volume, despite saying it kept a partial file")

  -- Loss inside the send window must be repaired, not merely survived: the file
  -- has to come back byte-identical.
  r.check(gw.drop_out == 0, "window-loss injection never fired")
  r.check(text:find("saved WINLOSS.TXT", 1, true) ~= nil,
          "the download that lost frames mid-window never completed")
  local _, wl = text:gsub("TLS was terminated on the far side", "")
  r.check(wl >= 4,
          "the file downloaded across a window gap did not read back intact (" ..
          wl .. " copies of the body seen)")

  -- ---------------------------------------------------------------- image
  r.check(#gw.images >= 1, "gateway received no image request")
  -- Auto-conversion: nothing asked for an image. The URL ended .jpg, the
  -- gateway said so with the flags byte, and the name was forced to .IMG
  -- because the bytes are no longer a JPEG.
  -- The completion line is deliberately terse now, so what proves the
  -- auto-conversion happened is the NAME: nothing asked for an image, the URL
  -- ended .jpg, and the extension was forced to .IMG because the saved bytes
  -- are Mode 3 tiles rather than a JPEG.
  r.check(text:find("saved TINY.IMG", 1, true) ~= nil,
          "the fetched picture was not saved under a forced .IMG name")
  r.check(text:find("saved TINY.IMG", 1, true) ~= nil,
          "the converted picture never reached the volume")

  -- The palette must land in CGRAM verbatim: entry i was encoded as (i, 0).
  if img_cg then
    local pal_ok = true
    for i = 0, 7 do
      if img_cg[i * 2] ~= i or img_cg[i * 2 + 1] ~= 0 then pal_ok = false end
    end
    r.check(pal_ok, "palette did not reach CGRAM intact")
  else
    r.check(false, "never sampled CGRAM")
  end

  -- And the tile bytes must land in VRAM in file order. A transposed or
  -- mis-offset copy renders as plausible-looking noise, which is exactly the
  -- failure an on-screen check misses and this one does not.
  if img_vr then
    local vr_ok = true
    for i = 0, 15 do
      if img_vr[i] ~= ((i * 7) & 0xFF) then vr_ok = false end
    end
    r.check(vr_ok, "tile data did not reach VRAM in order")
  else
    r.check(false, "never sampled VRAM")
  end

  -- The console has to come back afterwards: the viewer destroyed the font and
  -- both tilemaps to make room for the picture.
  r.check(text:find("viewed TINY.IMG", 1, true) ~= nil,
          "the viewer never returned to the shell")

  r.finish()
end, emu.eventType.endFrame)
