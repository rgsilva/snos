-- irc at 32 columns: the native layout, against the same kind of scripted ircd
-- as test/irc.lua.
--
-- The shell is switched to 32 columns first, and irc must NOT switch it back:
-- it lays itself out for the width it finds. Asserted here, by reading the
-- screen the way test/mode.lua does:
--
--   the user strip     one row for a few nicks, two with "(+N)" for many,
--                      none in the status window or a private-message window
--   NAMES replies      a fresh 353/366 replaces the list, silently; a list
--                      split over two 353s still counts as one
--   the full list      ^U opens it, up/down and PgDn scroll it, L (controller)
--                      closes it
--   wrapping           at 32 with a one-space indent; a long word is split
--   backlog            PgUp/PgDn through wrapped rows
--   the input line     longer than the width, scrolled; sent whole
--   the keyboard       22 rows, the strip and the input line still in place
--   leaving            the shell comes back at 32 columns
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}

local ircd = { sock = nil, rx_seen = 0, nick = nil, user = nil, buf = "",
               privmsgs = {}, registered = false, quit = nil,
               names = "@alice bob carol snesbot" }

local function srv(s)
  if ircd.sock then ircd.sock:send(s .. "\r\n") end
end

local function irc_line(line)
  local cmd, rest = line:match("^(%S+)%s*(.*)$")
  if not cmd then return end
  cmd = cmd:upper()
  if cmd == "NICK" then
    ircd.nick = rest:match("^:?(%S+)")
  elseif cmd == "USER" then
    ircd.user = rest
  end
  if ircd.nick and ircd.user and not ircd.registered then
    ircd.registered = true
    srv(":snos.test 001 " .. ircd.nick .. " :Welcome " .. ircd.nick)
  elseif cmd == "JOIN" then
    local chan = rest:match("^:?(%S+)")
    srv(":" .. ircd.nick .. "!u@snos JOIN " .. chan)
    srv(":snos.test 353 " .. ircd.nick .. " = " .. chan .. " :" .. ircd.names)
    srv(":snos.test 366 " .. ircd.nick .. " " .. chan .. " :End of NAMES list")
  elseif cmd == "PRIVMSG" then
    ircd.privmsgs[#ircd.privmsgs + 1] = rest
  elseif cmd == "QUIT" then
    ircd.quit = rest
    srv("ERROR :Closing Link: snos (Quit)")
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

gw.on_connect = function(sock, port)
  ircd.sock = sock
  ircd.rx_seen = 0
end

-- A NAMES reply for #snos: `list` split into 353s of `per` names, then 366.
local function names(list, per)
  local i = 1
  while i <= #list do
    local part = {}
    for k = i, math.min(i + per - 1, #list) do part[#part + 1] = list[k] end
    srv(":snos.test 353 snesbot = #snos :" .. table.concat(part, " "))
    i = i + per
  end
  srv(":snos.test 366 snesbot #snos :End of NAMES list")
end

local function numbered(prefix, n)
  local t = {}
  for i = 1, n do t[i] = string.format("%s%02d", prefix, i) end
  return t
end

-- Screen rows as plain text, inverse bit stripped, and the top of the screen.
local function row(r)
  return (L.display_row(r):gsub("[\128-\255]", function(c)
    return string.char(c:byte() & 0x7F)
  end))
end
local function rows(from, to)
  local t = {}
  for r = from, to do t[#t + 1] = row(r) end
  return t
end
local function whole()
  local _, n = L.con_state()
  return rows(0, n - 1)
end
local function joined(t) return table.concat(t, "\n") end

-- The pad's L, pressed for a few frames, and SELECT, for the keyboard.
local press_l, press_sel
local frame, at, step = 0, nil, 1
emu.addEventCallback(function()
  if press_l and frame >= press_l and frame < press_l + 6 then
    emu.setInput({ l = true }, 0)
  elseif press_sel and frame >= press_sel and frame < press_sel + 6 then
    emu.setInput({ select = true }, 0)
  end
end, emu.eventType.inputPolled)

local obs = {}
local claimed = false

local LONGMSG = "the quick brown fox jumps over the lazy dog and keeps on running"
local LONGWORD = string.rep("x", 70)
local TYPED = "0123456789abcdefghijklmnopqrstuvwxyz0123456789ABCD"

-- Each step waits its frame count, then acts.
local STEPS = {
  { 150, function() L.feed("mode 32\n") end },
  { 300, function() L.feed("irc 10.0.0.9:6667 snesbot #snos\n") end },
  -- connect, register, join, one NAMES reply of four nicks
  { 2200, function()
      obs.cols_in = L.con_cols()
      obs.one = whole()
    end },
  -- seven nicks: two rows, no marker
  { 10, function() names(numbered("nick", 7), 7) end },
  { 500, function() obs.two = whole() end },
  -- twenty: two rows and the marker, and none of the earlier names
  { 10, function() names(numbered("nick", 20), 20) end },
  { 500, function() obs.many = whole() end },
  -- twelve, over two 353s: still one list, and the count follows the list
  { 10, function() names(numbered("bot", 12), 6) end },
  { 500, function() obs.split = whole() end },
  -- forty: the full list, scrolled, and closed from the pad
  { 10, function() names(numbered("usr", 40), 10) end },
  { 500, function() L.feed("\021") end },
  { 300, function() obs.list = whole(); L.feed("\029") end },
  { 300, function() obs.list_down = whole(); L.feed("\145") end },
  { 300, function() obs.list_page = whole(); press_l = frame + 2 end },
  { 300, function() obs.list_closed = whole() end },
  -- back to twelve for the rest, then wrapping
  { 10, function() names(numbered("bot", 12), 12) end },
  { 400, function()
      srv(":alice!a@h PRIVMSG #snos :" .. LONGMSG)
      srv(":bob!b@h PRIVMSG #snos :" .. LONGWORD)
    end },
  { 500, function() obs.wrap = whole() end },
  -- a private message opens a window, which has no strip; so has the status one
  { 10, function() srv(":dave!d@h PRIVMSG snesbot :psst") end },
  { 400, function() L.feed("\014") end },
  { 300, function() obs.pm = whole(); L.feed("\016\016") end },
  { 300, function() obs.status = whole(); L.feed("\014") end },
  -- backlog: twelve messages of three rows each, then back through them
  { 300, function()
      for i = 1, 12 do
        srv(string.format(":alice!a@h PRIVMSG #snos :msg%02d %s", i,
                          "lorem ipsum dolor sit amet consectetur elit sed do eiusmod tempor"))
      end
    end },
  { 900, function() obs.live = whole(); L.feed("\144") end },
  { 400, function() obs.up1 = whole(); L.feed("\144\144\144\144") end },
  { 600, function() obs.top = whole(); L.feed("\145\145\145\145\145\145") end },
  { 600, function() obs.back_live = whole() end },
  -- an input line wider than the screen
  { 10, function() L.feed(TYPED) end },
  { 300, function() obs.typed = whole(); L.feed("\n") end },
  { 500, function() obs.sent = whole(); press_sel = frame + 2 end },
  -- the on-screen keyboard: 22 rows
  { 400, function()
      local _, n = L.con_state()
      obs.osk_rows = n
      obs.osk = whole()
      press_sel = frame + 2
    end },
  { 300, function() L.feed("/quit bye\n") end },
  { 900, function()
      obs.after = whole()
      obs.cols_after = L.con_cols()
    end },
}

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL irc32 - no mailbox magic"); emu.stop(1) end
    return
  end
  if not claimed then
    if frame < 200 then return end
    gw:claim()
    claimed = true
    at = frame + 60 + STEPS[1][1]
    return
  end
  gw:service()
  if ircd.sock and #ircd.sock.rx > ircd.rx_seen then
    local fresh = ircd.sock.rx:sub(ircd.rx_seen + 1)
    ircd.rx_seen = #ircd.sock.rx
    irc_rx(fresh)
  end
  if frame < at then return end

  if step <= #STEPS then
    local s = STEPS[step]
    s[2]()
    step = step + 1
    at = frame + (STEPS[step] and STEPS[step][1] or 0)
    return
  end

  local r = L.new_report("irc32")
  local text = L.ring_text()
  local function show(name, t)
    print("--- " .. name .. " ---"); print(joined(t or { "(never sampled)" }))
  end
  show("one row of nicks", obs.one)
  show("two rows and the marker", obs.many)
  show("the full list", obs.list)
  show("wrapped messages", obs.wrap)
  show("backlog, one page up", obs.up1)
  show("typing past the width", obs.typed)
  show("keyboard open", obs.osk)

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  local function have(t, s) return t and joined(t):find(s, 1, true) ~= nil end
  local function trim(s) return (s:gsub("%s+$", "")) end

  -- irc ran at 32 and nothing switched the console
  r.check(obs.cols_in == 32, "the console was not at 32 columns while irc ran")
  r.check(text:find("[64 columns while it runs]", 1, true) == nil,
          "the kernel switched the console to 64 for irc")

  -- one row of nicks, under the tab bar, above the messages
  local o = obs.one
  r.check(o and o[1]:find("2#snos", 1, true) and o[1]:find("snesbot", 1, true),
          "the tab bar lost the channel or the nick")
  r.check(o and trim(o[2]) == "alice bob carol snesbot", "the strip is not one row of nicks")
  r.check(o and not o[3]:find("alice bob", 1, true), "a second strip row for four nicks")
  r.check(have(o, "* joined #snos"), "the join notice is missing")
  r.check(have(o, "* 4 users (^U/L lists them)"), "the user count is missing or too wide")
  r.check(o and o[#o]:sub(1, 1) == ">", "the input line is not on the last row")

  -- two rows, then the marker with the count
  local t = obs.two
  r.check(t and trim(t[2]) == "nick01 nick02 nick03 nick04" and
          trim(t[3]) == "nick05 nick06 nick07", "seven nicks are not on two rows")
  r.check(t and not have(t, "alice"), "a fresh NAMES reply was appended, not a replacement")
  r.check(t and not have(t, "(+"), "a marker with every nick shown")
  local m = obs.many
  r.check(m and trim(m[2]) == "nick01 nick02 nick03 nick04" and
          trim(m[3]) == "nick05 nick06 nick07 (+13)", "twenty nicks: '" ..
          tostring(m and m[3]) .. "'")
  r.check(m and not have(m, "nick08"), "a nick shown that the marker also counts")
  -- a replacement is silent: exactly the one announcement, from the join
  local ann = 0
  for _ in joined(obs.many or {}):gmatch("users %(%^U/L") do ann = ann + 1 end
  r.check(ann == 1, "the user count was announced " .. ann .. " times")

  -- a list over two 353s
  local s = obs.split
  r.check(s and trim(s[2]) == "bot01 bot02 bot03 bot04 bot05" and
          trim(s[3]) == "bot06 bot07 bot08 bot09 (+3)",
          "a NAMES reply in two parts: '" .. tostring(s and s[3]) .. "'")
  r.check(s and not have(s, "nick"), "the old list survived a fresh NAMES reply")

  -- the full list
  local l = obs.list
  r.check(l and l[2]:find("users #snos", 1, true) and l[2]:find("esc closes", 1, true),
          "^U did not open the user list")
  r.check(l and trim(l[3]) == " usr01" and trim(l[4]) == " usr02",
          "the list is not one name per row: '" .. tostring(l and l[3]) .. "'")
  r.check(l and not have(l, "(+"), "the strip shows through the list")
  r.check(obs.list_down and trim(obs.list_down[3]) == " usr02", "down did not scroll the list")
  r.check(obs.list_page and trim(obs.list_page[3]) == " usr27",
          "page down did not move a page: '" .. tostring(obs.list_page and obs.list_page[3]) .. "'")
  local c = obs.list_closed
  r.check(c and trim(c[2]):find("^usr01") and not have(c, "esc closes"),
          "L on the controller did not close the list")

  -- wrapping
  local w = obs.wrap
  r.check(w and have(w, "<alice> the quick brown fox"), "the message is not shown")
  local ok_indent = true
  local rowsw = {}
  for i = 1, #(w or {}) do
    if w[i]:find("<alice>", 1, true) then
      local k = i + 1
      while w[k] and w[k]:sub(1, 1) == " " and trim(w[k]) ~= "" do k = k + 1 end
      rowsw = { table.unpack(w, i, k - 1) }
      ok_indent = (#rowsw >= 3) and rowsw[2]:sub(2, 2) ~= " "
    end
  end
  local words = {}
  for _, x in ipairs(rowsw) do words[#words + 1] = trim(x):gsub("^%s+", "") end
  r.check(ok_indent, "the message did not wrap onto indented rows")
  r.check(table.concat(words, " ") == "<alice> " .. LONGMSG,
          "wrapped text lost or moved words: '" .. table.concat(words, " ") .. "'")
  -- a word longer than a row is split, each piece indented
  r.check(w and have(w, "<bob>\n " .. string.rep("x", 31) .. "\n " ..
                        string.rep("x", 31) .. "\n " .. string.rep("x", 8)),
          "a 70-letter word was not split across indented rows")

  -- windows with no strip
  r.check(obs.pm and trim(obs.pm[2]) == "<dave> psst", "a private-message window has a strip")
  r.check(obs.pm and obs.pm[1]:find("3dave", 1, true), "the private window is not the third tab")
  r.check(obs.status and not have({ obs.status[2], obs.status[3] }, "bot01"),
          "the status window has a strip")

  -- backlog
  r.check(obs.live and have(obs.live, "msg12") and not have(obs.live, "msg01"),
          "the live view is not the newest rows")
  r.check(obs.up1 and obs.up1[1]:find("^ back", 1, true),
          "scrolling back is not shown in the tab bar")
  r.check(obs.up1 and not have(obs.up1, "msg12"), "page up did not move the view")
  r.check(obs.top and have(obs.top, "msg01"), "page up never reached the first message")
  r.check(obs.top and joined(obs.top):find("\n %a"),
          "a continuation row lost its indent in the backlog")
  r.check(obs.back_live and have(obs.back_live, "msg12") and
          not have(obs.back_live, "^ back"), "page down did not return to live")

  -- the input line
  local ty = obs.typed
  r.check(ty and trim(ty[#ty]) == "> " .. TYPED:sub(-29),
          "the input line is not the tail of what was typed: '" ..
          tostring(ty and trim(ty[#ty])) .. "'")
  local got = table.concat(ircd.privmsgs, "\n")
  r.check(got:find("#snos :" .. TYPED, 1, true) ~= nil,
          "the whole long line was not sent: '" .. got .. "'")
  r.check(obs.sent and trim(obs.sent[#obs.sent]) == ">", "the input line was not cleared")
  r.check(have(obs.sent, "<snesbot>\n " .. TYPED:sub(1, 31) .. "\n " .. TYPED:sub(32)),
          "the sent line was not echoed, wrapped")

  -- the keyboard
  local k = obs.osk
  r.check(obs.osk_rows == 22, "the text area is not 22 rows with the keyboard up")
  r.check(k and #k == 22 and k[1]:find("2#snos", 1, true), "the tab bar is not on top")
  r.check(k and trim(k[2]) == "bot01 bot02 bot03 bot04 bot05", "the strip moved")
  r.check(k and trim(k[22]):sub(1, 1) == ">", "the input line is not on the last text row")
  r.check(k and k[4] ~= nil and not k[4]:find("^>"), "messages are missing above the input")

  -- leaving
  r.check(obs.cols_after == 32, "the shell did not come back at 32 columns")
  r.check(ircd.quit ~= nil and ircd.quit:find("bye", 1, true), "QUIT did not arrive")
  r.check(text:find("[irc exited 0]", 1, true) ~= nil, "irc did not exit cleanly")
  r.check(have(obs.after, "irc: left"), "the exit line is not on the screen")

  r.finish()
end, emu.eventType.endFrame)
