-- M10 step 1: the context-switch primitive.
--
-- `debug mtest` forges two kernel-resident workers and lets the NMI
-- round-robin run all three processes (shell + A + B) for ~4 seconds. The
-- workers are designed to be unschedulable by anything except genuine
-- preemption: they never yield, never trap, never return — each is an
-- infinite counter bump. So the verdict line can only say "both progressed"
-- if the NMI genuinely switched between them, and it can only be PRINTED if
-- process 0 (the shell) got the machine back afterwards.
--
-- The prompt answering `debug ver` at the end is the third assertion in
-- disguise: the shell's own context survived two hundred round trips through
-- the forged-frame switch path.
local L = dofile("test/lib.lua")

local frame = 0
local step = 1
local at = nil

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL mtask - no mailbox magic"); emu.stop(1) end
    return
  end
  if at == nil then at = frame + 150 end
  if frame < at then return end

  -- ~4 s of scheduling plus margin for the report to print.
  if step == 1 then send("debug mtest\n", 400); step = 2; return end
  if step == 2 then send("debug ver\n", 200); step = 3; return end

  local r = L.new_report("mtask")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(text:find("both progressed OK", 1, true) ~= nil,
          "the workers did not both make progress - preemption is broken")
  r.check(text:find("STALLED", 1, true) == nil,
          "the scheduler reported a stalled worker")
  -- A=00000000 would satisfy 'no STALLED' only if the verdict logic lied;
  -- belt and braces: the printed hex counters must not be all zero.
  r.check(text:find("A=00000000", 1, true) == nil, "worker A never ran")
  r.check(text:find("B=00000000", 1, true) == nil, "worker B never ran")
  r.check(text:find("kernel ABI ", 1, true) ~= nil,
          "the shell did not answer after the test - process 0 was lost")

  r.finish()
end, emu.eventType.endFrame)
