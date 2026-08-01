-- M8: the IRC client, tested against an ircd written in Lua.
--
-- Same shape as test/httpd.lua and for the same reason: the link layer here
-- is the real one, frame for frame, and since the uIP cutover the transport
-- above it is the real one too — the console's own TCP, spoken against the
-- hermetic slirp box in test/gwip.lua, with an actual IRC server behind the
-- socket rather than a stub. So this exercises registration, PING/PONG,
-- JOIN, NAMES, both directions of PRIVMSG and a clean QUIT without Docker, a
-- socket or a human — which is what makes it worth running in CI.
--
-- The two protocol facts that cost other clients real time are asserted
-- directly: that a PING is answered (a missed PONG is a silent disconnection
-- minutes later), and that a CTCP PRIVMSG is dropped rather than printed (OFTC
-- sends an unsolicited VERSION on connect, and \1VERSION\1 sprayed into the
-- message pane looks exactly like the link corrupting data).
--
-- The keepalive scenario is MORE faithful now than it was over the stream
-- gateway: the ircd application goes silent while gwip's TCP keeps acking,
-- so the client's probe rides a healthy transport into a dead server —
-- exactly the failure keepalive exists for.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}

-- -------------------------------------------------------------------- ircd
local ircd = {
  sock = nil, dst = nil, port = nil, rx_seen = 0,
  nick = nil, user = nil,
  buf = "", lines = {}, privmsgs = {}, joined = {},
  registered = false, pongs = 0, quit = nil,
}

-- Server -> SNES. One IRC line, CRLF-terminated exactly as the wire has it.
local function srv(s)
  if ircd.sock then ircd.sock:send(s .. "\r\n") end
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
    -- The FIN goes out after the ERROR line's bytes are acknowledged —
    -- gwip's sock queues the close behind the send, the same ordering the
    -- old S2C_EOF-after-the-body had.
    ircd.sock:close()
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

-- The console dials out; the ircd answers whatever port it dialled. The
-- request bytes accumulate in sock.rx, drained into the line parser each
-- frame below.
gw.on_connect = function(sock, port)
  ircd.sock = sock
  ircd.dst = sock.peer_ip
  ircd.port = port
  ircd.rx_seen = 0
end

-- ------------------------------------------------------------------ test
local frame, step, at = 0, 1, nil
local claimed = false
local shot, bar_shot, status_shot, back_shot = nil, nil, nil, nil
local timeout_shot = nil
local ping_wait_until = 0
local PONG_GRACE_TEST = 1800   -- mirrors PONG_GRACE in irc.c

-- Did the CLIENT ever send a PING? (The ircd records every client line; the
-- server's own PING at step 2 is not in that list.)
local function saw_client_ping()
  for _, l in ipairs(ircd.lines) do
    if l:upper():find("^PING") then return true end
  end
  return false
end
local scrolled_shot, relive_shot = nil, nil

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL irc - no mailbox magic"); emu.stop(1) end
    return
  end

  if not claimed then
    if frame < 200 then return end
    gw:claim()
    claimed = true
    at = frame + 60
    return
  end

  gw:service()
  -- Feed the ircd whatever the client's TCP delivered since last frame.
  if ircd.sock and #ircd.sock.rx > ircd.rx_seen then
    local fresh = ircd.sock.rx:sub(ircd.rx_seen + 1)
    ircd.rx_seen = #ircd.sock.rx
    irc_rx(fresh)
  end
  if frame < at then return end

  -- Launched by name, with no `run` prefix, like every other application here.
  if step == 1 then
    L.feed("irc 10.0.0.9:6667 snesbot\n")
    -- Connect (a real handshake now), NICK/USER, then ~1.5 KB of welcome and
    -- MOTD in one burst. That burst is the thing being timed: it is what the
    -- repaint pacing exists for.
    at = frame + 2720
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
    -- QUIT names no channel; the client must route it via its user lists to
    -- the channels the quitter was in (carol is in #snos via the 353 above),
    -- and trim the server's "Quit: " prefix rather than printing it twice.
    srv(":carol!c@h QUIT :Quit: gone home")
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
    -- Keepalive: the ircd now goes COMPLETELY silent — at the application
    -- layer; gwip's TCP still acks, which is exactly the shape of a dead
    -- server on a live network. The client last heard from it at step 4, so
    -- KEEPALIVE_FRAMES (10800) after that it must probe with its own PING;
    -- the ircd deliberately never answers, and PONG_GRACE (1800) later the
    -- client must declare the connection lost. Real windows, no test
    -- backdoor — the testrunner is unthrottled, so this is wall-cheap.
    ping_wait_until = frame + 14000
    at = frame + 30
    step = 6.8
    return
  end

  if step == 6.8 then
    if saw_client_ping() then
      at = frame + PONG_GRACE_TEST + 600  -- ride out the grace, plus repaint slack
      step = 6.9
    elseif frame > ping_wait_until then
      at = frame + 30                     -- give up; the assertion will say why
      step = 6.9
    else
      at = frame + 30                     -- keep polling
    end
    return
  end

  if step == 6.9 then
    -- The timeout notice goes to the status tab and the client switches there
    -- itself; sample whatever is on screen now.
    timeout_shot = L.display_text(0, 27)
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
  print(string.format("ircd: nick=%s dst=%s:%s joined=%d privmsgs=%d pongs=%d",
        tostring(ircd.nick), tostring(ircd.dst), tostring(ircd.port),
        #ircd.joined, #ircd.privmsgs, ircd.pongs))

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  -- The connection itself: the host:port the app was given has to survive
  -- into the SYN's destination, or nothing below this means anything. This
  -- used to be a C2S_CONNECT string; now it is where the packet went.
  r.check(ircd.dst == "10.0.0.9" and ircd.port == 6667,
          "the SYN went to " .. tostring(ircd.dst) .. ":" .. tostring(ircd.port))
  r.check(gw.tcp_bad_csum == 0,
          "the console sent " .. gw.tcp_bad_csum ..
          " TCP segments with a bad checksum")

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

  -- Keepalive, both halves: the probe went out during the silence, and the
  -- unanswered probe was surfaced as a dead connection rather than the client
  -- sitting there looking connected forever.
  r.check(saw_client_ping(),
          "the client never sent its keepalive PING during server silence")
  r.check(timeout_shot ~= nil and
          timeout_shot:find("connection lost (ping timeout)", 1, true) ~= nil,
          "the unanswered PING did not surface as a connection-lost notice")

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
    -- carol's QUIT: routed to the channel she was in, reason shown once (the
    -- "Quit: " the server prefixes must not appear next to our own label).
    r.check(shot:find("* carol quit: gone home", 1, true) ~= nil,
            "carol's QUIT was not shown in the channel she was in")
    r.check(shot:find("Quit: gone home", 1, true) == nil,
            "the server's 'Quit: ' prefix was printed as part of the reason")
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
    r.check(status_shot:find("carol quit", 1, true) == nil,
            "a routed QUIT also fell through to the status window")
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
