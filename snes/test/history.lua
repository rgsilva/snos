-- Command history: arrow-up recalls, arrow-down walks forward.
--
-- Arrows arrive as SNOS_KEY_UP..RIGHT ($1C-$1F), not ANSI escapes, so the test
-- can feed them as plain bytes through the same mailbox ring a host keyboard
-- uses. That is the point of choosing unambiguous codes.
--
-- Counting trick: the shell echoes what you type, so "alpha" appears both in
-- the typed line and in the output of `echo alpha`. Matching "\nalpha\n" picks
-- out only the OUTPUT lines, which is what actually proves a command ran.
local L = dofile("test/lib.lua")

local UP, DOWN = "\28", "\29"

local SCRIPT = {
  { "echo alpha\n", 180 },
  { "echo beta\n", 180 },
  { UP .. "\n", 200 },              -- recall `echo beta`, run it
  { UP .. UP .. "\n", 200 },        -- walk back two, run `echo alpha`
  { UP .. UP .. DOWN .. "\n", 220 }, -- back two then forward one -> `echo alpha`
  { UP .. UP, 220 },                 -- recall WITHOUT enter, to inspect the screen
}

local frame, step, at = 0, 1, nil

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL history - no mailbox magic"); emu.stop(1) end
    return
  end
  if at == nil then at = frame + 150 end
  if frame < at then return end

  if step <= #SCRIPT then
    L.feed(SCRIPT[step][1])
    at = frame + SCRIPT[step][2]
    step = step + 1
    return
  end

  local r = L.new_report("history")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  local _, alpha = text:gsub("\nalpha\n", "")
  local _, beta = text:gsub("\nbeta\n", "")
  print(string.format("alpha ran %d times, beta ran %d times", alpha, beta))

  -- typed once, then recalled twice (steps 4 and 5)
  r.check(alpha == 3,
          "arrow-up did not recall `echo alpha` correctly (ran " .. alpha .. " times, expected 3)")
  -- typed once, then recalled once (step 3)
  r.check(beta == 2,
          "arrow-up did not recall `echo beta` correctly (ran " .. beta .. " times, expected 2)")

  -- Recalling must REPLACE the line, not append to it. If the erase were
  -- broken the shell would see `echo betaecho alpha` and reject it.
  r.check(text:find("unknown command", 1, true) == nil,
          "recall appended instead of replacing - the line was not erased")

  -- And check the SCREEN, not just the byte log. The mailbox records the raw
  -- backspace-space-backspace of an erase, so a completely broken erase still
  -- looks plausible there; only the rendered display shows the truth. The last
  -- script step recalled without pressing enter, so a line is sitting there.
  local screen = L.display_text(0, 21)
  local residue = nil
  for line in screen:gmatch("[^\n]+") do
    if line:find("alpha", 1, true) and line:find("beta", 1, true) then residue = line end
  end
  r.check(residue == nil,
          "erase left residue on screen: [" .. tostring(residue) .. "]")
  r.check(screen:find("> echo alpha", 1, true) ~= nil,
          "the recalled line is not on screen")

  r.finish()
end, emu.eventType.endFrame)
