-- M8: the IRC client, tested against an ircd written in Lua.
--
-- Same shape as test/net.lua and for the same reason: the link layer here is
-- the real one, frame for frame, and what sits on top of it is a real IRC
-- server rather than a stub. So this exercises registration, PING/PONG, JOIN,
-- NAMES, both directions of PRIVMSG and a clean QUIT without Docker, a socket
-- or a human — which is what makes it worth running in CI.
--
-- The two protocol facts that cost other clients real time are asserted
-- directly: that a PING is answered (a missed PONG is a silent disconnection
-- minutes later), and that a CTCP PRIVMSG is dropped rather than printed (OFTC
-- sends an unsolicited VERSION on connect, and \1VERSION\1 sprayed into the
-- message pane looks exactly like the link corrupting data).
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
local C2S_CONNECT, C2S_HANGUP = 0x07, 0x09
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
}

local function push(ch, s)
  gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 }
end

local function push_ctrl(typ, payload)
  push(CH_CTRL, string.char(typ, #payload) .. payload)
end

-- -------------------------------------------------------------------- ircd
local ircd = {
  ch = nil, hostport = nil, nick = nil, user = nil,
  buf = "", lines = {}, privmsgs = {}, joined = {},
  registered = false, pongs = 0, quit = nil,
}

-- Server -> SNES. One IRC line, CRLF-terminated exactly as the wire has it.
local function srv(s)
  if ircd.ch then push(ircd.ch, s .. "\r\n") end
end

-- Long on purpose. ergo's stock MOTD is 2.5 KB of ASCII art and wraps to about
-- fifty lines at 64 columns, and running against it is what found two things:
-- that a 40-line ring dropped the welcome before the MOTD finished, and that
-- there was no way to reach the lines the pane was not showing. Thirty lines is
-- enough for both here — more than a 26-row pane, in one uninterrupted burst.
local MOTD_LINES = 30

local function welcome()
  ircd.registered = true
  srv(":snos.test 001 " .. ircd.nick ..
      " :Welcome to the SNOS test IRC network " .. ircd.nick)
  srv(":snos.test 375 " .. ircd.nick .. " :- snos.test message of the day -")
  srv(":snos.test 372 " .. ircd.nick ..
      " :- a lua ircd speaking the real protocol")
  for i = 1, MOTD_LINES do
    srv(string.format(":snos.test 372 %s :- motd line %02d of %02d",
                      ircd.nick, i, MOTD_LINES))
  end
  srv(":snos.test 376 " .. ircd.nick .. " :End of MOTD command")
end

local function irc_line(line)
  ircd.lines[#ircd.lines + 1] = line
  local cmd, rest = line:match("^(%S+)%s*(.*)$")
  if not cmd then return end
  cmd = cmd:upper()

  if cmd == "NICK" then
    ircd.nick = rest:match("^:?(%S+)")
    if ircd.user and not ircd.registered then welcome() end
  elseif cmd == "USER" then
    ircd.user = rest
    if ircd.nick and not ircd.registered then welcome() end
  elseif cmd == "PONG" then
    ircd.pongs = ircd.pongs + 1
  elseif cmd == "JOIN" then
    local chan = rest:match("^:?(%S+)")
    if chan then
      ircd.joined[#ircd.joined + 1] = chan
      -- The echo is what actually opens the tab in the client: a channel that
      -- is invite-only or full never gets one, which is the correct behaviour
      -- and only falls out if the client waits for this rather than guessing.
      srv(":" .. ircd.nick .. "!u@snos JOIN " .. chan)
      srv(":snos.test 353 " .. ircd.nick .. " = " .. chan ..
          " :@alice bob carol " .. ircd.nick)
      srv(":snos.test 366 " .. ircd.nick .. " " .. chan ..
          " :End of NAMES list")
    end
  elseif cmd == "PRIVMSG" then
    ircd.privmsgs[#ircd.privmsgs + 1] = rest
  elseif cmd == "QUIT" then
    ircd.quit = rest
    srv("ERROR :Closing Link: snos (Quit)")
    push_ctrl(S2C_EOF, string.char(ircd.ch))
  end
end

local function irc_rx(s)
  ircd.buf = ircd.buf .. s
  while true do
    local p = ircd.buf:find("\n", 1, true)
    if not p then break end
    local line = ircd.buf:sub(1, p - 1):gsub("\r$", "")
    ircd.buf = ircd.buf:sub(p + 1)
    if #line > 0 then irc_line(line) end
  end
end

-- ------------------------------------------------------------------- link
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

  if c.typ == C2S_CONNECT and #c.buf >= 1 then
    local ch = c.buf[1]
    local hp = ""
    for i = 2, #c.buf do hp = hp .. string.char(c.buf[i]) end
    ircd.ch = ch
    ircd.hostport = hp
    push_ctrl(S2C_OPEN, string.char(ch, 200 & 0xFF, (200 >> 8) & 0xFF, 0))
  end
  if c.typ == C2S_HELLO then
    push_ctrl(S2C_HELLO, string.char(1) .. "10.0.0.1")
  end
  if c.typ == C2S_HANGUP and #c.buf >= 1 then gw.hangups = gw.hangups + 1 end
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
      s = table.concat(s)
      gw.upstream[ch] = (gw.upstream[ch] or "") .. s
      if ch == ircd.ch then irc_rx(s) end
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
local claimed = false
local shot, bar_shot, status_shot, back_shot = nil, nil, nil, nil
local scrolled_shot, relive_shot = nil, nil

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL irc - no mailbox magic"); emu.stop(1) end
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
  if frame < at then return end

  -- Launched by name, with no `run` prefix, like every other application here.
  if step == 1 then
    L.feed("irc 10.0.0.9:6667 snesbot\n")
    -- Connect, NICK/USER, then ~1.5 KB of welcome and MOTD in one burst. That
    -- burst is the thing being timed: it is what the repaint pacing exists for.
    at = frame + 2600
    step = 2
    return
  end

  -- A PING the client has to answer. Nothing above the link will tell you it
  -- was missed: the server simply drops the connection several minutes later.
  if step == 2 then
    srv("PING :snos.test")
    at = frame + 300
    step = 3
    return
  end

  if step == 3 then
    L.feed("/join #snos\n")
    at = frame + 700
    step = 4
    return
  end

  -- Three messages in a row, and the middle one is a CTCP. If the first and
  -- third appear and the second does not, the client received it and dropped
  -- it — which is the assertion. Asserting only its absence would also pass if
  -- it never arrived at all.
  if step == 4 then
    srv(":alice!a@h PRIVMSG #snos :hello from the lua ircd")
    srv(":bob!b@h PRIVMSG #snos :\1VERSION\1")
    srv(":carol!c@h PRIVMSG #snos :and this one comes after it")
    at = frame + 500
    step = 5
    return
  end

  -- The other direction: a line the SNES composed itself.
  if step == 5 then
    L.feed("hello from a super nintendo\n")
    at = frame + 400
    step = 6
    return
  end

  -- Sample the screen while the client still owns it. It repaints the console
  -- from its own model with SYS_PUTS_AT and never scrolls, so what is in the
  -- tilemap is the whole user interface.
  if step == 6 then
    shot = L.display_text(0, 27)
    bar_shot = L.display_text(0, 0)
    L.feed("\016")            -- ^P, back to the status window
    at = frame + 90
    step = 6.5
    return
  end

  -- The status window is where the server's own traffic went. Sampling both
  -- windows is what turns "the MOTD is somewhere" into an assertion about
  -- ROUTING: server chatter here, channel chatter there, neither in the other.
  if step == 6.5 then
    status_shot = L.display_text(0, 27)
    L.feed("\028\028")        -- up, up: back through the MOTD
    at = frame + 120
    step = 6.6
    return
  end

  -- The lines a 26-row pane is not showing have to be reachable, or a 64-line
  -- ring is the same as no ring at all.
  if step == 6.6 then
    scrolled_shot = L.display_text(0, 27)
    L.feed("\029\029\029")    -- down, down, down: back to live
    at = frame + 120
    step = 6.7
    return
  end

  if step == 6.7 then
    relive_shot = L.display_text(0, 27)
    L.feed("\014")            -- ^N, forward to the channel again
    at = frame + 90
    step = 6.75
    return
  end

  if step == 6.75 then
    back_shot = L.display_text(0, 27)
    at = frame + 30
    step = 7
    return
  end

  if step == 7 then
    L.feed("/quit see you\n")
    at = frame + 600
    step = 8
    return
  end

  local r = L.new_report("irc")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  print("--- screen ---"); print(shot or "(never sampled)"); print("--------------")
  print(string.format("ircd: nick=%s host=%s joined=%d privmsgs=%d pongs=%d",
        tostring(ircd.nick), tostring(ircd.hostport), #ircd.joined,
        #ircd.privmsgs, ircd.pongs))

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  -- The connection itself: the host:port the app was given has to survive the
  -- link intact, or nothing below this means anything.
  r.check(ircd.hostport == "10.0.0.9:6667",
          "gateway was asked to connect to " .. tostring(ircd.hostport))

  -- Registration. No CAP LS: three networks answer that with 421 or with
  -- silence, so a client that waits for it hangs on all three.
  r.check(ircd.nick == "snesbot",
          "NICK arrived as " .. tostring(ircd.nick))
  r.check(ircd.user ~= nil, "USER was never sent, so registration never completed")
  local sent_cap = false
  for _, l in ipairs(ircd.lines) do
    if l:upper():find("^CAP") then sent_cap = true end
  end
  r.check(not sent_cap,
          "the client sent CAP - Undernet and DALnet answer 421 and QuakeNet says nothing")

  -- PING/PONG.
  r.check(ircd.pongs >= 1,
          "no PONG came back - the server would drop this connection silently")

  -- JOIN, and the client waiting for the server's echo before opening a tab.
  r.check(ircd.joined[1] == "#snos",
          "JOIN arrived as " .. tostring(ircd.joined[1]))

  -- Sending.
  local sent = table.concat(ircd.privmsgs, "\n")
  r.check(sent:find("#snos :hello from a super nintendo", 1, true) ~= nil,
          "the SNES never sent its message: PRIVMSGs seen were '" .. sent .. "'")

  -- A clean quit, with the reason surviving rather than the socket just dying.
  r.check(ircd.quit ~= nil and ircd.quit:find("see you", 1, true) ~= nil,
          "QUIT arrived as " .. tostring(ircd.quit))

  -- ------------------------------------------------------------- on screen
  if shot then
    r.check(shot:find("<alice> hello from the lua ircd", 1, true) ~= nil,
            "an incoming channel message was not displayed")
    r.check(shot:find("<carol> and this one comes after it", 1, true) ~= nil,
            "the message AFTER the CTCP was not displayed")
    r.check(shot:find("VERSION", 1, true) == nil,
            "the unsolicited CTCP was printed into the pane as if it were text")
    r.check(shot:find("<snesbot> hello from a super nintendo", 1, true) ~= nil,
            "the client did not echo its own message locally")
    -- The layout: a tab bar on row 0 that did not scroll away, and an input
    -- line still on the last row after 26 rows of traffic went past.
    r.check(bar_shot ~= nil and bar_shot:find("#snos", 1, true) ~= nil,
            "row 0 is not the tab bar: '" .. tostring(bar_shot) .. "'")
    r.check(L.display_row(27):find("^>") ~= nil or shot:find("\n>", 1, true) ~= nil,
            "the input line is not on the bottom row")
  else
    r.check(false, "never sampled the screen")
  end

  -- Routing, in both directions. The server's welcome and MOTD belong to the
  -- status window and the channel traffic to the channel's, and the point is as
  -- much that neither leaks into the other as that each arrived.
  if status_shot then
    r.check(status_shot:find("End of MOTD command", 1, true) ~= nil,
            "the MOTD burst is not in the status window, or did not finish")
    r.check(status_shot:find("<alice>", 1, true) == nil,
            "channel messages leaked into the status window")
    r.check(shot ~= nil and shot:find("motd line", 1, true) == nil,
            "the server's MOTD leaked into the channel window")
  else
    r.check(false, "never sampled the status window")
  end

  -- Scrollback. The pane shows 26 rows of a 64-line ring, so most of a real
  -- server's MOTD is off-screen the moment it finishes: the early lines have to
  -- be reachable, the view must come back, and being scrolled has to be
  -- visible — a mode you cannot see is one you get stuck in.
  if status_shot and scrolled_shot and relive_shot then
    r.check(status_shot:find("motd line 01", 1, true) == nil,
            "the first MOTD line is somehow visible live in a 26-row pane")
    r.check(scrolled_shot:find("motd line 01", 1, true) ~= nil,
            "scrolling up did not reach the start of the MOTD")
    r.check(scrolled_shot:find("scrollback", 1, true) ~= nil,
            "the tab bar does not say the window is scrolled back")
    r.check(relive_shot:find("End of MOTD command", 1, true) ~= nil,
            "scrolling back down did not return to the newest line")
  else
    r.check(false, "never sampled the scrollback")
  end

  -- ^P got there and ^N came back, which is the whole navigation model.
  r.check(back_shot ~= nil and
          back_shot:find("<alice> hello from the lua ircd", 1, true) ~= nil,
          "^N did not return to the channel window")

  -- And it gave the machine back.
  r.check(text:find("[irc exited 0]", 1, true) ~= nil,
          "irc did not exit cleanly")
  r.check(text:find("left 1 connection open", 1, true) == nil,
          "irc leaked its connection - the kernel had to reclaim it")

  r.finish()
end, emu.eventType.endFrame)
