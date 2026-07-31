-- telnetd v2 -- remoting into a Super Nintendo.
--
-- Drives the REAL link protocol into a background telnetd and asserts the
-- three things that make it a mirror rather than a log:
--   1. on connect it clears the far terminal and paints the CURRENT screen
--      (the boot banner is already scrolled in — a stream tee could never
--      produce it, only a state diff can);
--   2. keystrokes sent down the socket reach the shell, and the result
--      appears BOTH in the mirror and on the real console;
--   3. all of it while a foreground application runs.
--
-- Transport core copied from bg.lua; consolidating the five-ish Lua gateway
-- copies is queued for M7, when the protocol next churns.
local L = dofile("test/lib.lua")

local MB = 0xE000
local MAGIC = 0x4B4C
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

local gw = {
  tx_seq = 0, rx_next = 0, last_snes_seq = 0, out = {},
  cmsg = { state = 0, typ = 0, len = 0, buf = {} },
  window = {}, stall = 0,
  upstream = {}, listen_ch = nil, listen_port = nil,
  closes = 0, hangups = 0,
}

local function push(ch, s)
  gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 }
end

local function push_ctrl(typ, payload)
  push(CH_CTRL, string.char(typ, #payload) .. payload)
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
    -- Do NOT auto-open: the test opens the connection on its own schedule,
    -- after the foreground app is already running.
    gw.listen_ch = c.buf[1]
    gw.listen_port = c.buf[2] | (c.buf[3] << 8)
  end
  if c.typ == C2S_HELLO then
    push_ctrl(S2C_HELLO, string.char(1) .. "10.0.0.1")
  end
  if c.typ == C2S_HANGUP and #c.buf >= 1 then gw.hangups = gw.hangups + 1 end
  if c.typ == C2S_CLOSE and #c.buf >= 1 then
    gw.closes = gw.closes + 1
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


-- -------------------------------------------------------------------- test
local frame, step, at = 0, 1, nil
local claimed = false
local mirror_during_fg = false
local idle_mark, idle_grew = nil, nil

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

local function upstream() return gw.upstream[gw.listen_ch or 255] or "" end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL telnetd - no mailbox magic"); emu.stop(1) end
    return
  end
  if not claimed then
    if frame < 200 then return end
    wu16(0x00, MAGIC)
    claimed = true
    at = frame + 60
    return
  end
  service()

  -- The mirror must keep working while a foreground app owns the console.
  if step >= 6 and step < 8 and not mirror_during_fg then
    if upstream():find("hello from a SNOS app", 1, true) then mirror_during_fg = true end
  end

  if at and frame < at then return end

  if step == 1 then send("run telnetd 2323 &\n", 400); step = 2; return end
  if step == 2 then
    if not gw.listen_ch then
      print("--- mailbox ---"); print(L.ring_text())
      print("RESULT FAIL telnetd - never bound its port"); emu.stop(1); return
    end
    push_ctrl(S2C_OPEN, string.char(gw.listen_ch, 200 & 0xFF, 0, 0))
    at = frame + 500          -- let the first full paint cross the link
    step = 2.4
    return
  end
  -- An IDLE screen must be SILENT. SYS_CONDIRTY is read-and-clear; if the
  -- clear half breaks, telnetd re-sends unchanged rows forever and the only
  -- symptom is wasted link -- invisible to every content assertion below.
  -- Nothing types and nothing prints during this window, so the byte count
  -- must not move at all.
  if step == 2.4 then
    idle_mark = #upstream()
    at = frame + 240
    step = 2.6
    return
  end
  if step == 2.6 then
    idle_grew = #upstream() - idle_mark
    at = frame + 1
    step = 3
    return
  end
  if step == 3 then
    -- Type into the SOCKET, as a telnet client would (CR LF for Enter).
    push(gw.listen_ch, "debug ver\r\n")
    at = frame + 500
    step = 4
    return
  end
  if step == 4 then send("run hello motd.txt\n", 500); step = 6; return end
  if step == 6 then at = frame + 300; step = 8; return end
  if step == 8 then send("kill 1\n", 200); step = 9; return end
  if step == 9 then send("debug ver\n", 200); step = 10; return end

  local r = L.new_report("telnetd")
  local text = L.ring_text()
  local up = upstream()
  print("--- console ---"); print(text)
  print("--- mirrored (first 400) ---"); print(up:sub(1, 400)); print("---")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.listen_port == 2323,
          "telnetd bound " .. tostring(gw.listen_port) .. ", not 2323")
  -- 1. the far terminal was cleared and given the screen as it STANDS
  r.check(up:find("\27[2J", 1, true) ~= nil, "no clear-screen on connect")
  r.check(up:find("\27[", 1, true) ~= nil, "no ANSI positioning at all")
  r.check(up:find("type 'help' for commands", 1, true) ~= nil,
          "the mirror did not carry the CURRENT screen (banner missing) - " ..
          "a state diff must reproduce text printed before the connection")
  -- 2. keystrokes from the socket reached the shell, and the answer came back
  r.check(text:find("kernel ABI ", 1, true) ~= nil,
          "the socket's keystrokes never reached the shell")
  r.check(up:find("kernel ABI ", 1, true) ~= nil,
          "the command's output never reached the remote terminal")
  -- 3. and it all kept working under a foreground application
  r.check(mirror_during_fg,
          "the mirror stopped while a foreground app ran")
  -- 4. a static screen costs nothing on the wire
  r.check(idle_grew == 0,
          "an idle screen sent " .. tostring(idle_grew) ..
          " bytes - the change mask is not clearing, so unchanged rows " ..
          "are being re-diffed and re-sent")

  r.finish()
end, emu.eventType.endFrame)
