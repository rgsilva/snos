-- ROM/SRAM name collisions.
--
-- ROMFS mounts first, so a first-match lookup always returned the ROM entry and
-- a same-named SRAM file was unreachable: you could write over a stock file,
-- see it in `ls`, and still read the original forever.
--
-- The writable copy now wins, which is what an overlay filesystem does and what
-- someone editing a stock file means. The ROM original is shadowed, not
-- destroyed — deleting the SRAM copy brings it back.
local L = dofile("test/lib.lua")

local ROM_TEXT = "Performance does not matter"
local NEW_TEXT = "SHADOWED"

local SCRIPT = {
  { "cat MOTD.TXT\n", 250 },              -- the ROM original
  { "write MOTD.TXT " .. NEW_TEXT .. "\n", 250 },
  { "cat MOTD.TXT\n", 250 },              -- must now be the SRAM copy
  { "ls\n", 250 },
  { "rm MOTD.TXT\n", 250 },
  { "cat MOTD.TXT\n", 300 },              -- the original must be back
}

local frame, step, at = 0, 1, nil

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL shadow - no mailbox magic"); emu.stop(1) end
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

  local r = L.new_report("shadow")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  -- Read before, after, and after deleting: the ROM text must appear twice
  -- (first and last), the SRAM text once in between.
  local _, romreads = text:gsub(ROM_TEXT, "")
  r.check(romreads == 2,
          "expected to read the ROM original twice (before shadowing and after " ..
          "removing the shadow), got " .. romreads)
  r.check(text:find("cat MOTD.TXT\n" .. NEW_TEXT, 1, true) ~= nil,
          "the SRAM copy did not shadow the ROM file")

  -- `ls` must say which entry is no longer what you would get.
  r.check(text:find("rom (hidden)", 1, true) ~= nil,
          "ls did not mark the shadowed ROM entry")

  -- Deleting must remove the SRAM copy, not fail because a ROM file has the
  -- same name.
  r.check(text:find("deleted MOTD.TXT", 1, true) ~= nil,
          "rm refused to delete the writable copy of a shadowed name")

  r.finish()
end, emu.eventType.endFrame)
