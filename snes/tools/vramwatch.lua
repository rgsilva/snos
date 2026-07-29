-- Watch the registers that decide whether a Mode 3 picture survives.
--
-- Append this to the bridge and load the result, because Mesen runs one script
-- at a time:
--
--   cat build/snosbridge.lua tools/vramwatch.lua > /tmp/watch.lua
--
-- then load /tmp/watch.lua in the Script Window instead of the bridge. The
-- bridge still works; this only adds logging.
--
-- $420C  HDMA enable. Anything non-zero while an app owns the PPU means the
--        keyboard panel's split is still rewriting the BG tilemap base and
--        scroll registers every scanline, which fetches tiles from the wrong
--        address and fills the screen with flickering noise — over a picture
--        that is perfectly correct in VRAM.
-- $2107  BG1 tilemap base. Should be written once by the app and then left.
-- $2115  VRAM address increment. If this is not $80 when the app writes tile
--        data, every 16-bit write splits across two addresses.
local WATCH = { [0x420C] = "$420C HDMA-enable", [0x2107] = "$2107 BG1SC",
                [0x2115] = "$2115 VMAIN", [0x2100] = "$2100 INIDISP" }
local seen = {}

for addr, name in pairs(WATCH) do
  emu.addMemoryCallback(function(address, value)
    -- Only report CHANGES, or the log is thousands of identical lines a second.
    if seen[address] ~= value then
      seen[address] = value
      emu.log(string.format("%s <- %02X", name, value))
    end
  end, emu.callbackType.write, addr, addr, emu.cpuType.snes)
end

emu.log("vramwatch: watching $420C $2107 $2115 $2100 for changes")
