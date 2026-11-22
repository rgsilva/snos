-- Second run of `make test`, straight after probe.lua, without deleting the
-- save file: the BW-RAM boot counter must have survived as battery RAM.
local MB = 0xF000
local RING, WRAM = MB + 0x010, emu.memType.snesWorkRam
local frame = 0

emu.addEventCallback(function()
  frame = frame + 1
  local done = emu.read16(MB, WRAM) == 0xCAFE and emu.read16(MB + 2, WRAM) == 1
  if not done and frame < 3600 then return end
  local s = {}
  for i = 0, math.min(emu.read16(MB + 4, WRAM), 3552) - 1 do
    s[#s + 1] = string.char(emu.read(RING + i, WRAM))
  end
  local boots = table.concat(s):match("bwboot%s+%-%-%s+boots=(%d+)")
  print("bwboot after a second power-on: boots=" .. tostring(boots))
  if done and tonumber(boots) == 2 then
    print("RESULT PASS persist")
    emu.stop(0)
  else
    print("RESULT FAIL persist - BW-RAM did not persist (want boots=2)")
    emu.stop(1)
  end
end, emu.eventType.endFrame)
