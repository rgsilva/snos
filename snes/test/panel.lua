-- Regression test for the keyboard show/hide behaviour.
--
-- Two bugs found by hand, both from the console failing to reconcile the rows
-- that move in or out of view when the text area is resized:
--
--   A. Scroll with the keyboard OPEN, then close it. The rows coming back into
--      view still held text from many screens ago — never cleared, because they
--      were never visible — so ancient output appeared at the bottom.
--   B. Scroll to the bottom with the keyboard CLOSED, then open it. The lines
--      below the new text area were stranded behind the panel instead of
--      scrolling up out of its way.
local L = dofile("test/lib.lua")

local PANEL_TOP = 22
local frame, step, at = 0, 1, nil
local sel_from, sel_to = nil, nil
local after_close, after_open = nil, nil

emu.addEventCallback(function()
  if sel_from and frame >= sel_from and frame < sel_to then
    emu.setInput({ select = true }, 0)
  end
end, emu.eventType.inputPolled)

local function press_select()
  sel_from = frame + 2
  sel_to = sel_from + 6
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL panel - no mailbox magic"); emu.stop(1) end
    return
  end
  if at == nil then at = frame + 150 end
  if frame < at then return end

  -- A: scroll hard with the keyboard open, then close it.
  if step == 1 then L.feed("help\n"); at = frame + 400; step = 2; return end
  if step == 2 then
    for _ = 1, 12 do L.feed("\n") end
    at = frame + 400; step = 3; return
  end
  if step == 3 then press_select(); at = frame + 200; step = 4; return end
  if step == 4 then
    after_close = L.display_text(PANEL_TOP, 27)
    -- B: with the keyboard closed, put a marker on the very bottom row.
    for _ = 1, 6 do L.feed("\n") end
    at = frame + 300; step = 5; return
  end
  if step == 5 then L.feed("echo MARKERTEXT\n"); at = frame + 250; step = 6; return end
  if step == 6 then press_select(); at = frame + 250; step = 7; return end

  local r = L.new_report("panel")
  local top, rows = L.con_state()
  after_open = L.display_text(0, PANEL_TOP - 1)

  print("--- rows " .. PANEL_TOP .. "-27 after closing the keyboard ---")
  print("[" .. after_close .. "]")
  print("--- text area after reopening it (top=" .. top .. " rows=" .. rows .. ") ---")
  print(after_open)

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  -- A. Closing must reveal blank rows, not resurrected scrollback.
  r.check(after_close:match("^%s*$") ~= nil,
          "closing the keyboard revealed stale text instead of blank rows")

  -- B. Reopening must scroll the text up so the last line stays visible.
  r.check(after_open:find("MARKERTEXT", 1, true) ~= nil,
          "opening the keyboard hid the last line behind the panel instead of " ..
          "scrolling it up")
  r.check(rows == PANEL_TOP,
          "text area is " .. rows .. " rows with the keyboard open, expected " .. PANEL_TOP)

  -- And the keyboard itself is intact and in its own tilemap.
  local panel = L.panel_text_plain()
  r.check(panel:find("q w e r t y u i o p", 1, true) ~= nil, "keyboard not drawn")
  r.check(panel:find("A type key", 1, true) ~= nil, "controller mapping missing")

  r.finish()
end, emu.eventType.endFrame)
