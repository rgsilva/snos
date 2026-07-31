-- httpd, tested against a Lua client speaking the real link protocol.
--
-- Same machinery as test/net.lua's netecho scenario — the hermetic gateway
-- accepts a "connection" onto the listening channel, streams a request, and
-- half-closes — but four times over, one per branch of the server: a 200 with
-- the right Content-Type and the file's actual bytes, a 404, a 400 for a path
-- that tries to leave the flat volume, and a 405 for a method it does not
-- serve. Each connection ends with the server's own hangup, which is what
-- returns the channel to LISTEN and lets the next one in — so the sequence
-- also exercises channel reuse on a listening channel, the path the kernel's
-- mark/flag reuse handoff exists for.
--
-- Assertions are anchored on the responses the "client" actually received
-- (gw.upstream, snapshotted at each hangup), not on the console transcript:
-- the console says what the server THINKS it did, the byte stream is what a
-- browser would have gotten.
local L = dofile("test/lib.lua")

local MB = 0xE000          -- link mailbox, WRAM $7EE000
local MAGIC = 0x4B4C       -- "LK"
local FRAME, HDR = 128, 4
local TXOFF, RXOFF = 0x10, 0x10 + FRAME
local SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
local F_DATA = 0x01
local CH_CTRL, CH_PAD = 0, 0xFF
local WRAM = emu.memType.snesWorkRam

local C2S_CLOSE, C2S_HELLO = 0x02, 0x04
local C2S_LISTEN, C2S_HANGUP = 0x08, 0x09
local S2C_OPEN, S2C_EOF, S2C_HELLO = 0x81, 0x82, 0x84

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
  tx_seq = 0, rx_next = 0, last_snes_seq = 0, out = {},
  cmsg = { state = 0, typ = 0, len = 0, buf = {} },
  window = {}, stall = 0,
  upstream = {}, hangups = 0,
  listen_ch = nil, listen_port = nil,
  responses = {},    -- one entry per served connection, at its hangup
}

local function push(ch, s)
  gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 }
end

local function push_ctrl(typ, payload)
  push(CH_CTRL, string.char(typ, #payload) .. payload)
end

-- One "client connection": accepted onto the listening channel, one request,
-- then EOF — every HTTP client half-closes and waits for the answer.
local function connect_and_request(req)
  local ch = gw.listen_ch
  push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, 0, 0))
  push(ch, req)
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
  if not c.dispatch then return end
  c.dispatch = false

  if c.typ == C2S_LISTEN and #c.buf >= 3 then
    gw.listen_ch = c.buf[1]
    gw.listen_port = c.buf[2] | (c.buf[3] << 8)
  end
  if c.typ == C2S_HANGUP and #c.buf >= 1 then
    -- The server has finished a connection: what the client holds at this
    -- moment IS the response, whole, because hangup refuses to go out while
    -- bytes are still queued.
    gw.hangups = gw.hangups + 1
    local ch = c.buf[1]
    gw.responses[#gw.responses + 1] = gw.upstream[ch] or ""
    gw.upstream[ch] = ""
  end
  if c.typ == C2S_HELLO then
    push_ctrl(S2C_HELLO, string.char(1) .. "10.0.0.1")
  end
  if c.typ == C2S_CLOSE and #c.buf >= 1 then
    local ch = c.buf[1]
    local i = 1
    while i <= #gw.out do
      if gw.out[i].ch == ch then table.remove(gw.out, i) else i = i + 1 end
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
      local s = {}
      for i = 0, len - 1 do s[#s + 1] = string.char(f[o + 2 + i]) end
      gw.upstream[ch] = (gw.upstream[ch] or "") .. table.concat(s)
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

local function seq_le(a, b) return ((b - a) & 0xFF) < 128 end

local function write_frame(f)
  for i = 0, FRAME - 1 do wb(RXOFF + i, f[i]) end
  wu16(0x04, ru16(0x04) + 1)
end

local function service()
  local ss = ru16(0x02)
  local polled = (ss ~= gw.last_snes_seq)
  if polled then
    gw.last_snes_seq = ss
    local f = {}
    for i = 0, FRAME - 1 do f[i] = rb(TXOFF + i) end
    if f[SUM] == sum_of(f) then
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

  if not polled then return end

  local WIN, RESEND_AFTER = 4, 12
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
local deadline_guard = 24000

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL httpd - no mailbox magic"); emu.stop(1) end
    return
  end

  if not gw.claimed then
    if frame < 200 then return end
    wu16(0x00, MAGIC)
    gw.claimed = true
    at = frame + 60
    return
  end

  service()
  if at and frame < at then return end

  if frame > deadline_guard then
    print("RESULT FAIL httpd - wedged at step " .. tostring(step) ..
          " hangups=" .. gw.hangups)
    emu.stop(1)
    return
  end

  if step == 1 then
    L.feed("httpd 8081\n")
    at = frame + 240
    step = 2
    return
  end

  -- Wait for the LISTEN, then run the four connections, each gated on the
  -- server's hangup of the one before — which is also what proves the server
  -- came back to accept again rather than serving once and wedging.
  if step == 2 then
    if gw.listen_ch then
      connect_and_request("GET /MOTD.TXT HTTP/1.1\r\nHost: snes\r\n\r\n")
      step = 3
    end
    at = frame + 30
    return
  end

  if step == 3 then
    if gw.hangups >= 1 then
      connect_and_request("GET /NOPE.TXT HTTP/1.1\r\n\r\n")
      step = 4
    end
    at = frame + 30
    return
  end

  if step == 4 then
    if gw.hangups >= 2 then
      connect_and_request("GET /../ETC HTTP/1.1\r\n\r\n")
      step = 5
    end
    at = frame + 30
    return
  end

  if step == 5 then
    if gw.hangups >= 3 then
      connect_and_request("POST / HTTP/1.1\r\n\r\n")
      step = 6
    end
    at = frame + 30
    return
  end

  if step == 6 then
    if gw.hangups >= 4 then
      L.feed("q")            -- any key stops the server
      at = frame + 240
      step = 7
    else
      at = frame + 30
    end
    return
  end

  local r = L.new_report("httpd")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  for i, resp in ipairs(gw.responses) do
    print(string.format("--- response %d (%d bytes) ---", i, #resp))
    print(resp:sub(1, 200))
  end

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.listen_port == 8081,
          "the port from the command line did not reach the bind")

  -- Four connections, four hangups, four responses: the count is the
  -- non-vacuous part — a server that wedged after one request would leave
  -- the later responses missing, not merely wrong.
  r.check(#gw.responses == 4,
          "expected 4 served connections, got " .. #gw.responses)

  local ok = gw.responses[1] or ""
  r.check(ok:find("HTTP/1.1 200 OK", 1, true) ~= nil,
          "MOTD.TXT did not come back 200")
  r.check(ok:find("Content%-Type: text/plain") ~= nil,
          "a .TXT file was not served as text/plain")
  r.check(ok:find("Performance does not matter", 1, true) ~= nil,
          "the response body is not the file's bytes")

  r.check((gw.responses[2] or ""):find("404", 1, true) ~= nil,
          "a missing file did not come back 404")
  r.check((gw.responses[3] or ""):find("400", 1, true) ~= nil,
          "a path escape attempt did not come back 400")
  r.check((gw.responses[4] or ""):find("405", 1, true) ~= nil,
          "a non-GET did not come back 405")

  r.finish()
end, emu.eventType.endFrame)
