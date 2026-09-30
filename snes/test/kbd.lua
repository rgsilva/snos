-- LINK_CH_KBD -- keystrokes that arrive over the link (docs/pico-adapter.md).
--
-- The hardware adapter carries the keyboard as one more record channel on the
-- same frames the network rides. This drives that path hermetically: the gwip
-- peer pushes a KBD record holding a shell command, and the verdict is that
-- the shell ran it -- the command echo and the prompt after it both appear in
-- the mailbox transcript, which only happens if the bytes reached the input
-- ring through net.c's record dispatch.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local CH_KBD = 0x11
local gw = G.new{}

local frame, at = 0, nil
local claimed, pushed = false, false

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL kbd - no mailbox magic"); emu.stop(1) end
    return
  end
  if not claimed then
    if frame < 200 then return end
    gw:claim()
    claimed = true
    at = frame + 120 -- let HELLO settle so the frames carry data
    return
  end
  gw:service()
  if at and frame < at then return end

  if not pushed then
    gw.push(CH_KBD, "help\n")
    pushed = true
    at = frame + 180
    return
  end

  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  local r = L.new_report("kbd")
  local echo_at = text:find("> help\n", 1, true)
  r.check(echo_at ~= nil, "the KBD record's bytes never reached the shell (no '> help' echo)")
  if echo_at then
    r.check(text:find("> ", echo_at + 6, true) ~= nil,
            "the shell did not return to its prompt after the injected command")
  end
  r.finish()
end, emu.eventType.endFrame)
