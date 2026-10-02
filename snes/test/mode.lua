-- `mode`, and the 32-column console.
--
-- The emulator ROM powers on at 64 columns in green, so this switches away
-- and back, reading the screen out of VRAM through lib.lua's mode-aware
-- decode (BG2 alone at 32, BG2+BG1 at 64):
--
--   mode / mode 32    the mode is reported, and the prompt redraws at 32
--   echo 40 chars     wraps at 32, not 64; BG1's map stays blank
--   backspace         crosses the wrap back onto the row above
--   SELECT            the 32-column keyboard panel, 22 text rows
--   mode 64           with the panel up: it comes back laid out for 64
--   mode <colour>     CGRAM 1 and 33 change, and the text stays put
--   apps / edit       a 64-column app runs at 64 and gives 32 back on exit
--   run edit & / fg   ...including when it was started in the background
--   view + break      a picture killed holding the PPU: the console comes
--                     back (it used to stay frozen), in the mode it had
--
-- The kill is the keyboard break, not START+SELECT: view leaves on any
-- button, so the chord only ever ends it politely, through its own release.
-- The break unwinds it through app_return like the chord does for anything
-- that ignores the pad.
--
-- The link is claimed only for the download at the end. With it up and idle
-- the NMI's network poll takes enough of each vblank that the keyboard
-- panel's row DMAs miss their window; the text console defers the poll while
-- it is dirty (con_dirty_pending), the panel does not.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local CGRAM = emu.memType.snesCgRam
local CTRL_Q = "\017"
local LONG = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcd" -- 40 columns
local TYPED = LONG:sub(1, 30)                           -- wraps after 25
local KEPT = LONG:sub(1, 22) .. "!"                     -- 8 backspaces later

-- The picture view is killed holding: net.lua's 2x2 fixture, served by the
-- same hermetic peer.
local gw = G.new{ dns = { ["snos.test"] = { 10, 0, 2, 80 } } }
local BODY_IMG
do
  local t = { "SIMG", string.char(1, 0), string.char(3), string.char(0),
              string.char(2, 0), string.char(2, 0), string.char(0, 0, 0, 0) }
  for i = 0, 255 do t[#t + 1] = string.char(i, 0) end
  for j = 0, 2 * 2 * 64 - 1 do t[#t + 1] = string.char((j * 7) & 0xFF) end
  BODY_IMG = table.concat(t)
end
local webconns = {}
gw.on_connect = function(sock, port)
  if port == 80 then webconns[#webconns + 1] = sock end
end
local function web_service()
  for i = #webconns, 1, -1 do
    local sock = webconns[i]
    if sock.reset or sock.done then
      table.remove(webconns, i)
    elseif sock.rx:find("\r\n\r\n", 1, true) then
      sock:send("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n" ..
                BODY_IMG)
      sock:close()
      table.remove(webconns, i)
    end
  end
end

-- Screen rows as plain text: inverse bit (the caret) stripped, then trimmed.
local function plain_row(r)
  return (L.display_row(r):gsub("[\128-\255]", function(c)
    return string.char(c:byte() & 0x7F)
  end):gsub("%s+$", ""))
end
local function screen_lines()
  local t = {}
  for r = 0, 27 do t[#t + 1] = plain_row(r) end
  return t
end
-- Does `a` appear on some row with `b` on the row right after it?
local function adjacent(lines, a, b)
  for i = 1, #lines - 1 do
    if lines[i] == a and lines[i + 1] == b then return true end
  end
  return false
end
local function on_screen(s)
  return table.concat(screen_lines(), "\n"):find(s, 1, true) ~= nil
end
local function cgram(i)
  return emu.read(2 * i, CGRAM) | (emu.read(2 * i + 1, CGRAM) << 8)
end
-- At 32 columns nothing may land in BG1's text map: it is not on screen, and
-- a stray write there is a flush still doing 64-column work.
local function bg1_blank()
  for w = L.BG1_MAP, L.BG1_MAP + 32 * 32 - 1 do
    if emu.read16(w * 2, emu.memType.snesVideoRam) ~= 0x2040 then return false end
  end
  return true
end
local function panel_snapshot()
  local _, rows = L.con_state()
  return { top = L.panel_row(L.PANEL_TOP), mid = L.panel_row(L.PANEL_TOP + 1),
           bot = L.panel_row(L.PANEL_TOP + L.PANEL_ROWS - 1),
           text = L.panel_text_plain(), hl = L.panel_has_highlight(), rows = rows }
end

local obs = {}
local frame, at, step = 0, nil, 1
local claimed = false
local select_from

emu.addEventCallback(function()
  if select_from and frame >= select_from and frame < select_from + 6 then
    emu.setInput({ select = true }, 0)
  end
end, emu.eventType.inputPolled)

local function press_select() select_from = frame + 2 end

-- Each step acts, then waits its frame count before the next one runs.
local STEPS = {
  { 150, function() L.feed("mode\n") end },
  { 250, function() obs.boot_cols = L.con_cols(); L.feed("mode 32\n") end },
  { 200, function()
      obs.m32 = { cols = L.con_cols(), lines = screen_lines() }
      L.feed("echo " .. LONG .. "\n")
    end },
  { 150, function()
      obs.wrap = screen_lines()
      obs.bg1 = bg1_blank()
      L.feed("echo " .. TYPED)
    end },
  { 250, function() L.feed(string.rep("\b", 8) .. "!\n") end },
  { 250, function() obs.bs = screen_lines(); press_select() end },
  { 300, function() obs.osk32 = panel_snapshot(); L.feed("mode 64\n") end },
  { 250, function()
      obs.osk64 = panel_snapshot()
      obs.m64 = { cols = L.con_cols(), lines = screen_lines() }
      press_select()
    end },
  { 150, function()
      obs.closed_rows = select(2, L.con_state())
      L.feed("mode white\n")
    end },
  { 150, function()
      obs.white = { c1 = cgram(1), c33 = cgram(33), pub = L.con_colour(),
                    kept = on_screen("64x28 green"), cols = L.con_cols() }
      L.feed("mode mint\n")
    end },
  { 150, function()
      obs.mint = { c1 = cgram(1), c33 = cgram(33), pub = L.con_colour() }
      L.feed("mode amber\n")
    end },
  { 150, function()
      obs.amber = { c1 = cgram(1), c33 = cgram(33), pub = L.con_colour() }
      L.feed("mode purple\n")
    end },
  { 150, function() L.feed("mode green 32\n") end },
  { 250, function()
      obs.both = { cols = L.con_cols(), c1 = cgram(1), c33 = cgram(33) }
      L.feed("apps\n")
    end },
  { 250, function() L.feed("edit\n") end },
  { 300, function()
      obs.in_edit = { cols = L.con_cols(), lines = screen_lines() }
      L.feed(CTRL_Q)
    end },
  { 300, function()
      obs.after_edit = { cols = L.con_cols(), colour = L.con_colour(), lines = screen_lines() }
      L.feed("run edit &\n")
    end },
  { 250, function() obs.bg_cols = L.con_cols(); L.feed("fg 1\n") end },
  { 300, function() obs.fg_cols = L.con_cols(); L.feed(CTRL_Q) end },
  { 180, function()
      obs.after_fg = { cols = L.con_cols(), lines = screen_lines() }
      gw:claim()
      claimed = true
    end },
  { 1200, function() L.feed("get http://snos.test/pic.img TINY.IMG\n") end },
  { 300, function() L.feed("run view TINY.IMG\n") end },
  { 300, function()
      obs.in_view = { cg0 = emu.read(2, CGRAM), cols = L.con_cols() }
      emu.write16(L.MB + 0x00E, 1, emu.memType.snesWorkRam) -- what F12 writes
    end },
  { 400, function()
      obs.after_view = { cols = L.con_cols(), lines = screen_lines() }
    end },
}

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL mode - no mailbox magic"); emu.stop(1) end
    return
  end
  if at == nil then at = frame + 150 end
  if claimed then
    gw:service()
    web_service()
  end
  if frame < at then return end

  if step <= #STEPS then
    local s = STEPS[step]
    s[2]()
    at = frame + s[1]
    step = step + 1
    return
  end

  local r = L.new_report("mode")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  local function show(name, lines)
    print("--- " .. name .. " ---"); print(table.concat(lines or {}, "\n"))
  end
  show("after mode 32", obs.m32 and obs.m32.lines)
  show("after the wraps", obs.bs)
  show("after mode 64", obs.m64 and obs.m64.lines)
  show("inside edit", obs.in_edit and obs.in_edit.lines)
  show("after edit", obs.after_edit and obs.after_edit.lines)
  show("after view was killed", obs.after_view and obs.after_view.lines)
  if obs.osk32 then print("--- panel at 32 ---"); print(obs.osk32.text) end
  if obs.osk64 then print("--- panel at 64 ---"); print(obs.osk64.text) end

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  -- mode, mode 32
  r.check(obs.boot_cols == 64, "the emulator ROM did not power on at 64 columns")
  r.check(text:find("64x28 green\n", 1, true) ~= nil, "`mode` did not report 64x28 green")
  r.check(obs.m32 and obs.m32.cols == 32, "`mode 32` did not publish 32 columns")
  r.check(obs.m32 and obs.m32.lines[1] == "32x28 green" and obs.m32.lines[2] == ">",
          "after `mode 32` the screen is not the report and a fresh prompt")

  -- the wrap, at 32 and not 64
  r.check(obs.wrap and adjacent(obs.wrap, LONG:sub(1, 32), LONG:sub(33)),
          "a 40-column echo did not wrap after 32 columns")
  r.check(obs.wrap and adjacent(obs.wrap, "> echo " .. LONG:sub(1, 25), LONG:sub(26)),
          "the typed command line did not wrap after 32 columns")
  r.check(obs.bg1 == true, "text reached BG1's map at 32 columns")

  -- backspace back across the wrap
  r.check(text:find(KEPT .. "\n", 1, true) ~= nil,
          "the line editor lost track of the backspaced line")
  r.check(obs.bs and adjacent(obs.bs, "> echo " .. KEPT, KEPT),
          "backspace did not cross the wrap: the erased row or the '!' is wrong")

  -- the 32-column keyboard panel
  local p = obs.osk32
  r.check(p and #p.top == 32 and p.top:byte(1) == 3 and p.top:byte(32) == 4,
          "the panel's top corners are not at columns 0 and 31")
  r.check(p and #p.bot == 32 and p.bot:byte(1) == 5 and p.bot:byte(32) == 6,
          "the panel's bottom corners are not at columns 0 and 31")
  r.check(p and p.mid:byte(1) == 2 and p.mid:byte(32) == 2,
          "the panel's sides are not at columns 0 and 31")
  r.check(p and p.text:find("q w e r t y u i o p", 1, true) ~= nil, "no key rows at 32")
  r.check(p and p.top:find("A type", 1, true) ~= nil and p.bot:find("SELECT hide", 1, true),
          "the controller mapping is not in the frame at 32")
  r.check(p and p.hl, "no key drawn inverse at 32")
  r.check(p and p.rows == 22, "text area is not 22 rows with the panel up at 32")

  -- mode 64 with the panel up: back to two layers, panel relaid out
  local q = obs.osk64
  r.check(obs.m64 and obs.m64.cols == 64, "`mode 64` did not publish 64 columns")
  r.check(obs.m64 and obs.m64.lines[1] == "64x28 green",
          "the two-layer decode does not read back `mode 64`'s report")
  r.check(q and #q.top == 64 and q.top:byte(64) == 4,
          "the panel did not come back laid out for 64 columns")
  r.check(q and q.text:find("A type key", 1, true) ~= nil, "64-column hints missing")
  r.check(q and q.rows == 22, "text area is not 22 rows with the panel back up")
  r.check(obs.closed_rows == 28, "closing the panel did not give back 28 rows")

  -- colours: CGRAM 1 (Mode 5) and 33 (Mode 0) both, and no clear
  local function colour_ok(o, v)
    return o and o.c1 == v and o.c33 == v and o.pub == v
  end
  r.check(colour_ok(obs.white, 0x7FFF), "`mode white` did not write 7FFF to CGRAM 1 and 33")
  r.check(colour_ok(obs.mint, 0x33EC), "`mode mint` did not write 33EC")
  r.check(colour_ok(obs.amber, 0x029F), "`mode amber` did not write 029F")
  r.check(obs.white and obs.white.kept and obs.white.cols == 64,
          "a colour change alone cleared the screen or changed the width")
  r.check(text:find("64x28 white\n", 1, true) ~= nil, "`mode white` did not report")
  r.check(text:find("usage: mode [32|64] [colour]", 1, true) ~= nil,
          "`mode purple` was not refused with the usage")
  r.check(obs.both and obs.both.cols == 32 and obs.both.c1 == 0x03E0 and
          obs.both.c33 == 0x03E0, "`mode green 32` did not set both")

  -- 64-column apps
  r.check(text:find("edit  bank $00CB  64 cols\n", 1, true) ~= nil and
          text:find("irc  bank $00D3  64 cols\n", 1, true) ~= nil and
          text:find("hello  bank $00C8\n", 1, true) ~= nil,
          "`apps` does not mark exactly the 64-column apps")
  r.check(obs.in_edit and obs.in_edit.cols == 64, "edit did not get 64 columns")
  r.check(text:find("[64 columns while it runs]", 1, true) ~= nil, "no notice of the switch")
  r.check(obs.after_edit and obs.after_edit.cols == 32, "edit's exit did not restore 32")
  r.check(obs.after_edit and obs.after_edit.colour == 0x03E0,
          "the colour did not survive the switch")
  r.check(obs.after_edit and table.concat(obs.after_edit.lines, "\n")
            :find("[edit exited 0]", 1, true) ~= nil,
          "the shell's exit report is not on the 32-column screen")
  r.check(obs.bg_cols == 32, "a background launch switched the foreground's screen")
  r.check(obs.fg_cols == 64, "`fg` did not give a 64-column job 64 columns")
  r.check(obs.after_fg and obs.after_fg.cols == 32, "leaving `fg` did not restore 32")
  r.check(text:find("[1] edit exited", 1, true) ~= nil, "the foregrounded edit did not exit")

  -- the PPU a killed picture held
  r.check(text:find("saved TINY.IMG", 1, true) ~= nil, "the picture did not download")
  r.check(obs.in_view and obs.in_view.cg0 == 1, "view never put its palette up")
  r.check(text:find("[view killed]", 1, true) ~= nil, "the break did not kill view")
  r.check(obs.after_view and obs.after_view.cols == 32, "the console came back at the wrong width")
  r.check(obs.after_view and table.concat(obs.after_view.lines, "\n")
            :find("[view killed]", 1, true) ~= nil,
          "the screen never repainted after view was killed holding the PPU")

  r.finish()
end, emu.eventType.endFrame)
