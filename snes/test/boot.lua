-- M0 boot test.
-- Asserts three independent things:
--   1. the kernel reaches the end of main (mailbox status PASS),
--   2. the banner is actually ON SCREEN (read back out of the VRAM tilemap,
--      not just echoed to the mailbox — this is what proves the Mode 5
--      two-layer console really works),
--   3. the kernel stack stayed inside the slot linker.scm gave it.
local L = dofile("test/lib.lua")

local KSTACK_LO, KSTACK_HI = 0x0100, 0x0BFF
local frame = 0

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then
      print(string.format("RESULT FAIL boot - mailbox magic never appeared (got %04X)",
                          L.magic()))
      emu.stop(1)
    end
    return
  end

  local status = L.status()
  if status == L.STATUS_RUNNING then
    if frame > 1800 then
      print("RESULT FAIL boot - ran 30s without finishing")
      print(L.ring_text())
      emu.stop(1)
    end
    return
  end

  local r = L.new_report("boot")
  local text = L.ring_text()
  local screen = L.screen_text(6)

  print("--- mailbox ---"); print(text)
  print("--- screen (from VRAM) ---"); print(screen)
  print("--------------------------")

  r.check(status == L.STATUS_PASS,
          string.format("status is %04X, expected PASS", status))
  r.check(screen:find("SNOS 0.1", 1, true) ~= nil,
          "banner not present in VRAM tilemap - console did not render")
  r.check(screen:find("mode 5", 1, true) ~= nil,
          "second banner line missing from VRAM - only one row got flushed?")

  local low = text:match("STACKLOW (%x+)")
  if low == nil then
    r.check(false, "no STACKLOW report")
  else
    local v = tonumber(low, 16)
    print(string.format("kernel stack low-water: $%04X (used %d of %d bytes)",
                        v, KSTACK_HI + 1 - v, KSTACK_HI + 1 - KSTACK_LO))
    r.check(v >= KSTACK_LO, string.format(
      "kernel stack ran out of its slot: $%04X < $%04X, it is into the app " ..
      "region that M3 hands to applications", v, KSTACK_LO))
    r.check(v <= KSTACK_HI, string.format("nonsense stack low-water $%04X", v))
  end

  r.finish()
end, emu.eventType.endFrame)
