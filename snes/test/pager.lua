-- `cat` pagination.
--
-- Before this, `cat README.TXT` scrolled most of itself off the top — the one
-- document explaining the system was the one you could not read.
--
-- NET.TXT is used rather than README.TXT because README fits inside one screen
-- and so correctly produces no prompt at all. A pager test needs a file that
-- actually overflows, which is worth stating: the first version of this test
-- passed against an unpaginated build for exactly that reason.
local L = dofile("test/lib.lua")

local frame, step, at = 0, 1, nil
local page1 = nil

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL pager - no mailbox magic"); emu.stop(1) end
    return
  end
  if at == nil then at = frame + 150 end
  if frame < at then return end

  if step == 1 then L.feed("cat NET.TXT\n"); at = frame + 250; step = 2; return end
  if step == 2 then
    -- Captured mid-file: the prompt must be on screen and the tail must not be.
    page1 = L.display_text(0, 27)
    L.feed(" ")
    at = frame + 250
    step = 3
    return
  end
  if step == 3 then L.feed(" "); at = frame + 250; step = 4; return end
  if step == 4 then L.feed(" "); at = frame + 250; step = 5; return end
  if step == 5 then
    -- Second pass, quitting early this time.
    L.feed("cat NET.TXT\n")
    at = frame + 250
    step = 6
    return
  end
  if step == 6 then L.feed("q"); at = frame + 200; step = 7; return end

  local r = L.new_report("pager")
  local text = L.ring_text()
  local screen = L.display_text(0, 27)

  print("--- page 1 (on screen, mid-file) ---"); print(page1)
  print("--- final screen ---"); print(screen)

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  -- It paused at all.
  r.check(page1 ~= nil and page1:find("-- more --", 1, true) ~= nil,
          "no pager prompt - the file printed straight through")
  -- And it really was mid-file, not a prompt printed after everything.
  r.check(page1:find("SPEED", 1, true) == nil,
          "the whole file was already on screen when the prompt appeared")

  -- Advancing delivers the rest.
  r.check(text:find("Roughly 1.7 KB/s", 1, true) ~= nil,
          "the end of the file never arrived after advancing")

  -- The prompt does not litter the output it interrupts.
  r.check(screen:find("-- more --", 1, true) == nil,
          "pager prompt left on screen after the last page")

  -- q stops early: the second cat must not reach the tail again.
  local _, tails = text:gsub("Roughly 1%.7 KB/s", "")
  r.check(tails == 1,
          "q did not quit - the file was printed to the end " .. tails .. " times")

  r.finish()
end, emu.eventType.endFrame)
