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
local CH_CTRL, CH_PAD = 0, 0xFF
local WRAM = emu.memType.snesWorkRam

local C2S_FETCH, C2S_CLOSE = 0x01, 0x02
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
  drop_snes = 0,      -- ignore this many incoming frames (simulates loss)
  corrupt_out = 0,    -- send this many frames with a broken checksum
  fetches = {},       -- urls seen, for assertions
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

local function push(ch, s)
  gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 }
end

local function push_ctrl(typ, payload)
  push(CH_CTRL, string.char(typ, #payload) .. payload)
end

local function on_fetch(ch, url)
  gw.fetches[#gw.fetches + 1] = url
  if url:find("missing") then
    push_ctrl(S2C_OPEN, string.char(ch, 404 & 0xFF, (404 >> 8) & 0xFF))
    push(ch, BODY_404)
  else
    push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, (200 >> 8) & 0xFF))
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
    -- C2S_CLOSE needs nothing from a canned gateway.
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

local function write_frame(f)
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
  if ss ~= gw.last_snes_seq then
    gw.last_snes_seq = ss
    if gw.drop_snes > 0 then
      gw.drop_snes = gw.drop_snes - 1     -- pretend the frame never arrived
    else
      local f = {}
      for i = 0, FRAME - 1 do f[i] = rb(0x10 + i) end
      if f[SUM] == sum_of(f) then
        if gw.awaiting and f[ACK] == gw.tx_seq then
          gw.awaiting = false
          gw.tx_seq = (gw.tx_seq + 1) & 0xFF
        end
        if (f[FLAGS] & F_DATA) ~= 0 and f[SEQ] == gw.rx_next then
          deliver(f)
          gw.rx_next = (gw.rx_next + 1) & 0xFF
        end
      end
    end
  end

  -- Outgoing
  if not gw.awaiting then
    local f, any = build()
    gw.inflight = f
    if any then gw.awaiting = true end
    write_frame(f)
  else
    gw.inflight[ACK] = (gw.rx_next - 1) & 0xFF
    gw.inflight[SUM] = 0
    gw.inflight[SUM] = sum_of(gw.inflight)
    write_frame(gw.inflight)
  end
end

-- ------------------------------------------------------------------ test
local frame, step, at = 0, 1, nil
local claimed = false

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

  if step == 1 then L.feed("net\n"); at = frame + 150; step = 2; return end
  if step == 2 then L.feed("get http://example.com/ok\n"); at = frame + 900; step = 3; return end
  if step == 3 then L.feed("get http://example.com/missing\n"); at = frame + 600; step = 4; return end
  if step == 4 then
    -- Now the paths a real cable cannot be asked to exercise: lose two frames
    -- from the SNES, and hand it two frames with broken checksums. Both must
    -- recover invisibly and the body must arrive byte-perfect.
    gw.drop_snes = 2
    gw.corrupt_out = 2
    L.feed("get http://example.com/faults\n")
    at = frame + 1200
    step = 5
    return
  end

  local r = L.new_report("net")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  print(string.format("gateway saw %d fetches, resends left: drop=%d corrupt=%d",
        #gw.fetches, gw.drop_snes, gw.corrupt_out))

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(text:find("gateway up", 1, true) ~= nil,
          "kernel did not detect the gateway")

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

  r.finish()
end, emu.eventType.endFrame)
