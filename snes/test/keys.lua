-- The rest of the keyboard, and the keyboard as a controller, over the link.
--
-- Everything arrives as the Pico adapter sends it: KBD records for the bytes
-- (snos_abi.h's Delete, Home, End, PageUp, PageDown and F-keys among them)
-- and KBDPAD records for the held buttons, pushed through the same gwip peer
-- as test/kbd.lua. What is checked:
--   1. the shell's line editor: Home, End, Left and Delete edit mid-line, and
--      the screen shows the line it ran; keys it has no use for (F1, Insert,
--      PageUp, F12) are dropped, not echoed as inverse glyphs;
--   2. edit: Home, End and Delete on a line, PageUp and PageDown by a screen
--      less one line, Delete at a line's end joining the next; the file is
--      read back with `cat`;
--   3. `pad` (unchanged) shows a KBDPAD press once, a tap whose press and
--      release arrived in one frame once, and the keyboard's START+SELECT is
--      not the kill switch;
--   4. the keyboard's SELECT does not open the on-screen keyboard and its Up
--      does not reach the shell as an arrow from the panel's d-pad, while the
--      controller's SELECT still does open it;
--   5. a key held when the adapter goes away (the mailbox unclaimed here, a
--      pulled cable or a dead link on the hardware) is let go.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local CH_KBD, CH_KBDPAD = 0x11, 0x12
local LEFT, DEL = "\30", "\127"
local F1, F12, INSERT = "\129", "\140", "\141"
local HOME, END, PGUP, PGDN = "\142", "\143", "\144", "\145"
local CTRL_S, CTRL_Q = "\19", "\17"
local B, SELECT, START, UP, A, X = 0x8000, 0x2000, 0x1000, 0x0800, 0x0080, 0x0040

local gw = G.new{}
local r = L.new_report("keys")

local function key(s) gw.push(CH_KBD, s) end
local function pad(mask) gw.push(CH_KBDPAD, string.char(mask & 0xFF, (mask >> 8) & 0xFF)) end
local function status() return L.display_text(0, 0) end
local function text_rows() local _, rows = L.con_state(); return rows end

-- A display row that reads exactly `want`, inverse bit and all, or nil.
local function row_exact(want)
  for row = 0, 27 do
    if L.display_row(row) == want then return row end
  end
  print("--- screen, looking for [" .. want .. "] ---")
  for row = 0, 27 do
    local s = L.display_row(row)
    print(string.format("%2d [%s] %s", row, s:gsub("[\128-\255]", "#"),
                        s:gsub(".", function(c) return string.format("%02x", c:byte()) end)))
  end
  return nil
end

-- The CONTROLLER's buttons, held from inputPolled: name -> frames left.
local hold = {}
local unplugged = false

local STEPS = {
  -- 1. the shell
  { function() key("cho one" .. HOME .. "e" .. END .. "!\n") end, 120 },
  { function()
      r.check(row_exact("> echo one!") ~= nil,
              "Home/End: the screen does not show '> echo one!'")
      key("Xecho two" .. HOME .. DEL .. "\n")
    end, 120 },
  { function() key("echo threeX" .. LEFT .. DEL .. "\n") end, 120 },
  { function() key("echo f" .. F1 .. INSERT .. PGUP .. F12 .. "our\n") end, 120 },
  { function()
      r.check(row_exact("> echo four") ~= nil,
              "keys the line editor has no use for showed up on its line")
      key("echo five" .. HOME)
    end, 60 },
  { function()
      -- The caret sits on the 'e' and shows it inverted, not an inverse space.
      r.check(row_exact("> " .. string.char(0x80 | 0x65) .. "cho five") ~= nil,
              "the caret does not show the character under it")
      key(END .. "\n")
    end, 120 },

  -- 2. edit
  { function() key("run edit\n") end, 300 },
  { function() key("abcdef" .. HOME .. DEL .. END .. "Z") end, 90 },
  { function() key(string.rep("\n", 60)) end, 240 },
  { function()
      r.check(status():find("ln 61/61", 1, true) ~= nil, "edit: 60 Enters did not land")
      key(PGUP)
    end, 90 },
  -- 28 rows: a status bar and 27 of text, so a page is 26 lines.
  { function()
      r.check(status():find("ln 35/61", 1, true) ~= nil, "edit: PageUp from 61 is not 35")
      key(PGUP)
    end, 90 },
  { function()
      r.check(status():find("ln 9/61", 1, true) ~= nil, "edit: PageUp from 35 is not 9")
      key(PGUP)
    end, 90 },
  { function()
      r.check(status():find("ln 1/61", 1, true) ~= nil, "edit: PageUp did not stop at 1")
      key(PGDN)
    end, 90 },
  { function()
      r.check(status():find("ln 27/61", 1, true) ~= nil, "edit: PageDown from 1 is not 27")
      r.check(L.display_text(1, 1):find("bcdefZ", 1, true) == nil,
              "edit: PageDown did not move the view with the cursor")
      key(PGUP)
    end, 90 },
  { function()
      r.check(status():find("ln 1/61", 1, true) ~= nil, "edit: PageUp did not undo PageDown")
      key(END .. DEL) -- the end of line 1: joins the empty line 2
    end, 90 },
  { function()
      local s = status()
      r.check(s:find("ln 1/60", 1, true) ~= nil and s:find("col 7", 1, true) ~= nil,
              "edit: End then Delete did not join the next line [" .. s .. "]")
      key(CTRL_S)
    end, 200 },
  { function() key("KEYS.TXT\n") end, 300 },
  { function() key(CTRL_Q) end, 300 },
  { function() key("cat KEYS.TXT\n") end, 280 },
  { function() key("q") end, 120 },       -- 60 lines: the pager asks

  -- 3. pad
  { function() key("run pad\n") end, 200 },
  { function() pad(A) end, 40 },          -- held for 40 frames: shown once
  { function() pad(0) end, 30 },
  { function() pad(X); pad(0) end, 30 },  -- a tap inside one link frame
  { function() pad(START | SELECT) end, 90 }, -- longer than the kill switch
  { function() pad(0) end, 30 },
  { function() pad(B) end, 30 },
  { function() pad(0) end, 120 },

  -- 4. the on-screen keyboard is the controller's
  { function() pad(SELECT) end, 20 },
  { function() pad(0) end, 60 },
  { function()
      r.check(text_rows() == 28, "the keyboard's SELECT opened the on-screen keyboard")
      pad(UP)
    end, 20 },
  { function() pad(0) end, 20 },
  { function() key("\n") end, 120 },     -- an arrow from the panel would recall `run pad`
  { function() hold.select = 5 end, 90 },
  { function()
      r.check(text_rows() == 22, "the controller's SELECT did not open the on-screen keyboard")
      hold.select = 5
    end, 90 },

  -- 5. the adapter goes away with A held
  { function() key("run pad\n") end, 200 },
  { function() pad(A) end, 40 },
  { function()
      emu.write16(0xE000, 0, emu.memType.snesWorkRam) -- the link's magic
      unplugged = true
    end, 60 },
  { function() hold.b = 5 end, 120 },     -- and the controller ends pad
}

emu.addEventCallback(function()
  local input = {}
  for button, left in pairs(hold) do
    if left > 0 then
      input[button] = true
      hold[button] = left - 1
    end
  end
  if next(input) then emu.setInput(input, 0) end
end, emu.eventType.inputPolled)

local frame, step, at, claimed = 0, 1, nil, false

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL keys - no mailbox magic"); emu.stop(1) end
    return
  end
  if not claimed then
    if frame < 200 then return end
    gw:claim()
    claimed = true
    at = frame + 120 -- let HELLO settle so the frames carry data
    return
  end
  if not unplugged then gw:service() end
  if frame < at then return end

  if step <= #STEPS then
    STEPS[step][1]()
    at = frame + STEPS[step][2]
    step = step + 1
    return
  end

  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  for _, word in ipairs({ "one!", "two", "three", "four", "five" }) do
    r.check(text:find("\n" .. word .. "\n", 1, true) ~= nil,
            "the shell did not run `echo " .. word .. "`")
  end
  r.check(text:find("unknown command", 1, true) == nil, "a line ran garbled")
  r.check(text:find("[\128-\255]") == nil, "a key above $7F was echoed")

  r.check(text:find("\nbcdefZ\n", 1, true) ~= nil, "KEYS.TXT does not read back as bcdefZ")

  local function count(s) local _, n = text:gsub(s:gsub("%p", "%%%0"), ""); return n end
  r.check(text:find("PAD-RAN", 1, true) ~= nil, "pad never started")
  r.check(count("pad=0080\n") == 2, "a held KBDPAD press showed " .. count("pad=0080\n") ..
          " times in two runs, not once in each")
  r.check(count("pad=0040\n") == 1, "a tap within one frame showed " .. count("pad=0040\n") ..
          " times, not once")
  r.check(count("pad=3000\n") == 1, "the keyboard's START+SELECT did not reach pad")
  r.check(text:find("[pad killed]", 1, true) == nil, "the keyboard's START+SELECT killed pad")
  r.check(text:find("pad: clean exit", 1, true) ~= nil, "the keyboard's B did not end pad")
  r.check(count("pad test: B exits") == 2, "the keyboard's Up reached the shell as an arrow")
  local last = text:match(".*PAD%-RAN(.*)") or ""
  r.check(last:find("^\npad=0000\n") ~= nil,
          "pad's first read saw keys pressed at the shell before it ran")
  r.check(last:find("pad=0080\npad=0000\npad=8000\npad: clean exit", 1, true) ~= nil,
          "A was still held after the adapter went away")
  r.finish()
end, emu.eventType.endFrame)
