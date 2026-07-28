-- M4 filesystem test.
--
-- The interesting assertion is persistence: a file written to cartridge SRAM
-- has to survive a reset, because that is the whole difference between the SRAM
-- volume and a RAM disk, and it is what makes BASIC's SAVE worth anything.
local L = dofile("test/lib.lua")

local frame = 0
local step = 1
local at = nil
local reset_done = false
-- emu.reset() restarts the kernel, which re-initialises the mailbox and wipes
-- everything printed so far. Keep a copy or the pre-reset half of the test is
-- invisible in the report.
local pre_text = ""

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL fs - no mailbox magic"); emu.stop(1) end
    return
  end
  if at == nil then at = frame + 150 end
  if frame < at then return end

  if step == 1 then send("debug fstest\n", 900); step = 2; return end
  if step == 2 then send("ls\n", 220); step = 2.5; return end
  -- lowercase on purpose: the file is MOTD.TXT
  if step == 2.5 then send("cat motd.txt\n", 220); step = 3; return end
  if step == 3 then send("write NOTES.TXT persisted ok\n", 260); step = 4; return end
  if step == 4 then send("cat NOTES.TXT\n", 220); step = 5; return end
  if step == 5 then
    -- Reset the console. SRAM is battery-backed, so the file must still be
    -- there when the kernel comes back up and re-mounts the volume.
    pre_text = L.ring_text()
    emu.reset()
    reset_done = true
    at = frame + 300
    step = 6
    return
  end
  if step == 6 then send("cat NOTES.TXT\n", 240); step = 7; return end
  if step == 7 then send("rm NOTES.TXT\n", 200); step = 8; return end
  if step == 8 then send("ls\n", 220); step = 9; return end

  local r = L.new_report("fs")
  local text = pre_text .. L.ring_text()
  print("--- before reset ---"); print(pre_text)
  print("--- after reset ---"); print(L.ring_text())
  print("--------------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(reset_done, "test never reset the console")

  r.check(text:find("SRAMKB 00000100", 1, true) ~= nil,
          "cart SRAM is not 256 KB - the SRAMFS sizing assumption is wrong")
  r.check(text:find("README.TXT", 1, true) ~= nil, "ROMFS not mounted")
  r.check(text:find("rom", 1, true) ~= nil, "ROM files not flagged read-only")
  r.check(text:find("Performance does not matter", 1, true) ~= nil,
          "cat of a ROM file returned nothing")
  r.check(text:find("wrote NOTES.TXT", 1, true) ~= nil, "write failed")
  r.check(text:find("fstest: 20000 bytes across 3 windows OK", 1, true) ~= nil,
          "SRAM addressing is wrong across window boundaries")

  -- Two `cat NOTES.TXT` outputs must appear: one before the reset, one after.
  local _, hits = text:gsub("persisted ok", "")
  r.check(hits >= 2,
          "file did not survive the reset (" .. hits ..
          " reads) - SRAM is not persisting, or the volume is reformatted on boot")

  r.check(text:find("deleted NOTES.TXT", 1, true) ~= nil, "rm failed")

  r.finish()
end, emu.eventType.endFrame)
