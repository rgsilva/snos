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
-- $2100  INIDISP. See below: WHERE it is written matters, not whether.
local WATCH = { [0x420C] = "$420C HDMA-enable", [0x2107] = "$2107 BG1SC",
                [0x2115] = "$2115 VMAIN" }
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

-- $2100 gets its own rule, because "it changed" stopped being the interesting
-- fact.
--
-- An application on this machine has NO VBLANK to write VRAM or OAM in — NMI
-- moves a link frame on every frame while an app owns the PPU, and at -O0 that
-- runs from the top of the window into the middle of the next frame. So `vnc`
-- writes under a deliberate forced blank on the scanlines BELOW its picture,
-- which are black anyway, and toggles $2100 several times a second on purpose.
-- Logging every change buries the log; logging only the ones that fall where the
-- picture is displayed turns this into a direct detector for the flicker that
-- used to be there. Nothing should ever be printed here after the app's display
-- comes on.
local STRIP_TOP, STRIP_END = 192, 225
local blank_at = nil

emu.addMemoryCallback(function(_, value)
  local ok, st = pcall(emu.getState)
  local sl = (ok and st and st["ppu.scanline"]) or -1
  if value >= 0x80 then
    blank_at = sl
  elseif blank_at then
    if blank_at < STRIP_TOP or blank_at >= STRIP_END or
       sl < STRIP_TOP or sl >= STRIP_END then
      emu.log(string.format(
        "$2100 forced blank over scanlines %d..%d - THE PICTURE IS BLANKED",
        blank_at, sl))
    end
    blank_at = nil
  end
end, emu.callbackType.write, 0x2100, 0x2100, emu.cpuType.snes)

emu.log("vramwatch: watching $420C $2107 $2115 for changes, and $2100 for a " ..
        "forced blank outside scanlines 192-224")
