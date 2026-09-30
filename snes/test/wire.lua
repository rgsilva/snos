-- apps/wire under the emulator: no adapter, so every reply is zeros and every
-- frame counts as bad -- which is exactly the path worth checking here. The
-- verdict is that the app runs its exchange loop off the vblank tick without
-- wedging the machine, prints its once-a-second line with the counts, and
-- hands the shell back when B is pressed.
local L = dofile("test/lib.lua")

local frame, at, step = 0, nil, 1
local b_from, b_to = nil, nil

-- Buttons must be set from inputPolled, not endFrame: set there they are
-- consumed by the poll that follows and the auto-joypad read never sees them.
emu.addEventCallback(function()
  if b_from and frame >= b_from and frame < b_to then
    emu.setInput({ b = true }, 0)
  end
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL wire - no mailbox magic"); emu.stop(1) end
    return
  end
  if at and frame < at then return end

  if step == 1 then
    L.feed("wire\n")
    at = frame + 200   -- three of its once-a-second lines
    step = 2
  elseif step == 2 then
    b_from, b_to = frame, frame + 10
    at = frame + 90
    step = 3
  else
    local text = L.ring_text()
    print("--- mailbox ---"); print(text); print("---------------")
    local r = L.new_report("wire")
    r.check(text:find("wire: port 2", 1, true) ~= nil, "wire did not start")
    -- "frames ok bad pico# reply": with zeros on the wire, ok stays 0 and
    -- bad tracks frames. Match the second line so the counts are settled.
    local f2, ok2, bad2 = text:match("\n%s*%d+%s+%d+%s+%d+%s+%d+ [^\n]*\n%s*(%d+)%s+(%d+)%s+(%d+)")
    r.check(f2 ~= nil, "no stats lines printed")
    if f2 then
      r.check(tonumber(ok2) == 0 and tonumber(bad2) == tonumber(f2),
              "with no adapter every frame must count as bad (" .. f2 .. "/" .. ok2 .. "/" .. bad2 .. ")")
      r.check(tonumber(f2) >= 100, "exchanges are not running once per vblank (" .. f2 .. " after ~2 s)")
    end
    r.check(text:find("wire: done", 1, true) ~= nil, "B did not exit the app")
    r.check(text:match("wire: done\n[^\n]*\n> ") ~= nil, "shell prompt did not return after wire")
    r.finish()
  end
end, emu.eventType.endFrame)
