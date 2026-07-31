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
local jrnl_after = nil
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
  if step == 8 then send("ls\n", 220); step = 8.2; return end
  -- A CORRUPT COMPACT JOURNAL MUST NOT BRICK THE MACHINE. Battery SRAM can
  -- hold anything -- a stale journal from an older build, a half-written
  -- word -- and recovery's only tool is a copy loop bounded by a size field
  -- read from that same volume. Trusting it hung the console at boot,
  -- before the shell, unrecoverable without wiping the volume. Arm garbage
  -- and demand the kernel refuse it and come up anyway.
  if step == 8.2 then
    local S = emu.memType.snesSaveRam
    local J = 16 + 48 * 32                     -- fs.c's CJ_SLOT
    emu.write(J, 3, S); emu.write(J + 1, 0, S) -- slot 2+1, armed
    emu.write(J + 2, 0, S); emu.write(J + 3, 0, S)
    emu.write(J + 4, 0, S); emu.write(J + 5, 8, S)  -- dest = 2048
    emu.write(J + 6, 0, S); emu.write(J + 7, 0, S)
    pre_text = pre_text .. L.ring_text()
    emu.reset()
    at = frame + 400
    step = 8.5
    return
  end
  if step == 8.5 then
    local S = emu.memType.snesSaveRam
    local J = 16 + 48 * 32
    jrnl_after = emu.read(J, S) | (emu.read(J + 1, S) << 8) |
                 (emu.read(J + 2, S) << 16) | (emu.read(J + 3, S) << 24)
    send("debug ver\n", 260)
    step = 9
    return
  end

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
  r.check(text:find("compact reclaimed and survived recovery", 1, true) ~= nil,
          "compaction lost data or failed to reclaim")
  r.check(text:find("COMPACT FAILED", 1, true) == nil, "compact self-test failed")
  r.check(text:find("full-volume overwrite refused, file intact", 1, true) ~= nil,
          "overwriting on a full volume must refuse up front, not destroy the old copy")

  -- Two `cat NOTES.TXT` outputs must appear: one before the reset, one after.
  local _, hits = text:gsub("persisted ok", "")
  r.check(hits >= 2,
          "file did not survive the reset (" .. hits ..
          " reads) - SRAM is not persisting, or the volume is reformatted on boot")

  r.check(text:find("deleted NOTES.TXT", 1, true) ~= nil, "rm failed")

  r.check(jrnl_after == 0,
          "a corrupt compact journal survived the boot (" ..
          tostring(jrnl_after) .. ") - recovery must refuse it")
  r.check(L.ring_text():find("kernel ABI ", 1, true) ~= nil,
          "the console did not come up after a corrupt journal - " ..
          "an unvalidated redo hangs before the shell exists")

  r.finish()
end, emu.eventType.endFrame)
