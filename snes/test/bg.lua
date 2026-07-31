-- M10 step 5: the milestone. A background process stays CONNECTED and
-- WORKING while a foreground application runs.
--
--   run netecho 7777 &     spawn a server as a background process
--   (gateway opens the listen channel and streams bytes at it)
--   run hello motd.txt     a foreground app runs meanwhile
--   -> the echo comes back DURING the foreground run: two applications,
--      one stock SNES, no SA-1.
--
-- Also asserted, because the safety story is the point:
--   - `jobs` sees the process; the shell prompt stays alive throughout;
--   - a foreground app whose arena overlaps the background one (`get` shares
--     region C with netecho) is REFUSED by name at launch — deterministic
--     refusal, never runtime corruption;
--   - `kill` reaps the process AND its connection (the gateway sees the
--     C2S_CLOSE), and `jobs` reports the corpse exactly once.
--
-- The gateway here is the hermetic Lua one (net.lua's machinery, windowed
-- sender), driving the REAL link protocol frame for frame.
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
local echo_seen_during_fg = false

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL bg - no mailbox magic"); emu.stop(1) end
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

  -- The moment worth the whole milestone: the echo arriving WHILE the
  -- foreground app runs (steps 4..5 are hello's window).
  if step >= 4 and step < 6 and not echo_seen_during_fg then
    if (gw.upstream[gw.listen_ch or 255] or ""):find("hello%-bg") then
      echo_seen_during_fg = true
    end
  end

  if at and frame < at then return end

  if step == 1 then send("run netecho 7777 &\n", 400); step = 2; return end
  if step == 2 then send("jobs\n", 200); step = 3; return end
  if step == 3 then
    -- The server is listening. Open the connection and stream at it, then
    -- IMMEDIATELY start a foreground app: the echo has to come back while
    -- hello owns the console.
    if not gw.listen_ch then
      print("--- mailbox at failure ---")
      print(L.ring_text())
      print("RESULT FAIL bg - netecho never bound its port")
      emu.stop(1)
      return
    end
    push_ctrl(S2C_OPEN, string.char(gw.listen_ch, 200 & 0xFF, 0, 0))
    push(gw.listen_ch, "hello-bg\n")
    send("run hello motd.txt\n", 300)
    step = 4
    return
  end
  if step == 4 then at = frame + 200; step = 5; return end -- let the echo land
  if step == 5 then send("run get nothing -\n", 250); step = 6; return end
  if step == 6 then send("kill 1\n", 200); step = 7; return end
  if step == 7 then send("jobs\n", 200); step = 7.2; return end
  -- fg: respawn, bring it to the console, then the keyboard break kills the
  -- FOREGROUNDED job and hands the shell back -- with the shell asleep in
  -- cmd_fg's wait the whole time.
  if step == 7.2 then send("run netecho 7777 &\n", 300); step = 7.4; return end
  if step == 7.4 then send("fg 1\n", 150); step = 7.6; return end
  if step == 7.6 then
    emu.write16(L.MB + 0x00E, 1, WRAM) -- what F12 writes
    at = frame + 250
    step = 8
    return
  end
  if step == 8 then send("debug ver\n", 200); step = 9; return end

  local r = L.new_report("bg")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(text:find("] netecho started in the background", 1, true) ~= nil,
          "the background spawn did not report")
  r.check(text:find("netecho  running", 1, true) ~= nil,
          "jobs did not show the process running")
  r.check(gw.listen_port == 7777,
          "netecho bound " .. tostring(gw.listen_port) .. ", not 7777")
  r.check(text:find("[hello exited 42]", 1, true) ~= nil,
          "the foreground app did not run to completion")
  r.check(echo_seen_during_fg,
          "the echo did not arrive while the foreground app was running - " ..
          "background execution is not real")
  r.check(text:find("arena conflict", 1, true) ~= nil and
          text:find("netecho holds that memory", 1, true) ~= nil,
          "the overlapping foreground launch was not refused by name")
  r.check(text:find("[1] killed", 1, true) ~= nil, "kill did not report")
  r.check(gw.closes >= 1,
          "the killed process's connection was never closed at the gateway")
  r.check(text:find("no background jobs", 1, true) ~= nil,
          "jobs still shows something after the kill")
  r.check(text:find("] netecho killed", 1, true) ~= nil,
          "the keyboard break did not kill the foregrounded job")
  r.check(text:find("kernel ABI ", 1, true) ~= nil,
          "the shell did not answer at the end")

  r.finish()
end, emu.eventType.endFrame)
