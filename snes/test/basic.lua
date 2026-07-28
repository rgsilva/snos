-- M5 test: BASIC, the first real application.
--
-- The point is not that BASIC works; it is that an application can hold the
-- machine for a while, use the console and the filesystem through system calls,
-- persist something across its own lifetime, and hand control back cleanly.
--
--   type a program -> RUN -> SAVE -> BYE (clean exit)
--   run it again   -> LOAD -> RUN (same answers) -> BYE
local L = dofile("test/lib.lua")

local SCRIPT = {
  { "run basic\n", 240 },
  { "10 FOR I=1 TO 5\n", 60 },
  { "20 PRINT I*I\n", 60 },
  { "30 NEXT\n", 60 },
  { '40 PRINT "DONE"\n', 60 },
  { "RUN\n", 300 },
  { 'SAVE "PROG.BAS"\n', 240 },
  { "BYE\n", 200 },          -- clean exit back to the shell
  { "ls\n", 200 },           -- the saved file is really on the SRAM volume
  { "run basic\n", 240 },
  { 'LOAD "PROG.BAS"\n', 300 },
  { "LIST\n", 300 },
  { "RUN\n", 300 },
  { "PRINT 6*7\n", 200 },    -- immediate mode
  { "BYE\n", 240 },
}

local frame, step, at = 0, 1, nil

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL basic - no mailbox magic"); emu.stop(1) end
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

  local r = L.new_report("basic")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(text:find("SNOS BASIC", 1, true) ~= nil, "BASIC did not start")

  -- FOR/NEXT and arithmetic
  local squares = text:find("1\n4\n9\n16\n25", 1, true)
  r.check(squares ~= nil, "FOR/NEXT with I*I did not produce 1 4 9 16 25")
  r.check(text:find("DONE", 1, true) ~= nil, "PRINT of a string literal failed")

  -- persistence through the SNOS filesystem
  r.check(text:find("saved", 1, true) ~= nil, "SAVE failed")
  r.check(text:find("PROG.BAS", 1, true) ~= nil, "saved file not visible to the shell")
  r.check(text:find("loaded", 1, true) ~= nil, "LOAD failed")

  -- the reloaded program must LIST and RUN identically
  r.check(text:find("10 FOR I=1 TO 5", 1, true) ~= nil, "LIST after LOAD lost the program")
  local _, runs = text:gsub("\n25\n", "")
  r.check(runs >= 2, "reloaded program did not produce the same answers (" .. runs .. " runs)")

  -- immediate mode
  r.check(text:find("42", 1, true) ~= nil, "immediate PRINT 6*7 did not give 42")

  -- and a clean exit, twice
  local _, byes = text:gsub("%[basic exited", "")
  r.check(byes >= 2, "BASIC did not exit cleanly back to the shell twice (" .. byes .. ")")

  r.finish()
end, emu.eventType.endFrame)
