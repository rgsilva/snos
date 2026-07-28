-- M1 input test.
--
-- Exercises both backends of the kbd_getchar() abstraction:
--   line 1 is typed through the mailbox feed (how every later test drives the
--          shell, with no human and no controller),
--   line 2 is typed on the ON-SCREEN KEYBOARD by pressing buttons, which is
--          the path a real user takes and the one nothing else would catch.
--
-- The OSK starts with the selection on row 1 ("qwertyuiop[]") column 0, so
-- A yields 'q'; RIGHT then A yields 'w'; START submits the line.
--
-- NOTE: emu.setInput only has any effect from an `inputPolled` callback. Called
-- from `endFrame` it is silently ignored, which looks exactly like a broken
-- controller driver in the ROM.
local L = dofile("test/lib.lua")

local frame = 0
local phase = 1
local feed_at = nil
local press_base = nil

-- Each press is held several frames: the kernel samples the pad once per vblank
-- through auto-joypad read, so a one-frame press can fall between samples.
local SCRIPT = {
  { 0, "a" },      -- 'q'
  { 12, "right" }, -- move to 'w'
  { 24, "a" },     -- 'w'
  { 40, "start" }, -- submit
}
local HOLD = 5

emu.addEventCallback(function()
  if press_base == nil then return end
  for _, step in ipairs(SCRIPT) do
    local at = press_base + step[1]
    if frame >= at and frame < at + HOLD then
      emu.setInput({ [step[2]] = true }, 0)
    end
  end
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL input - no mailbox magic"); emu.stop(1) end
    return
  end

  if phase == 1 then
    if feed_at == nil then feed_at = frame + 60 end
    if frame == feed_at then
      L.feed("hello world\n")
      phase = 2
      press_base = frame + 120
    end
    return
  end

  if phase == 2 then
    if frame > press_base + 90 then phase = 3 end
    return
  end

  local status = L.status()
  if status == L.STATUS_RUNNING then
    if frame > 3600 then
      print("RESULT FAIL input - ran 60s without finishing")
      print("--- mailbox ---"); print(L.ring_text())
      print("--- screen ---"); print(L.screen_text(28))
      emu.stop(1)
    end
    return
  end

  local r = L.new_report("input")
  local text = L.ring_text()
  local screen = L.screen_text(28)
  local plain = L.screen_text_plain(28)

  print("--- mailbox ---"); print(text)
  print("--- screen (from VRAM) ---"); print(screen)
  print("--------------------------")

  r.check(status == L.STATUS_PASS,
          string.format("status is %04X, expected PASS", status))

  r.check(text:find("got [hello world]", 1, true) ~= nil,
          "mailbox-fed line did not come back from kbd_readline")
  r.check(text:find("got [qw]", 1, true) ~= nil,
          "OSK did not produce 'qw' - navigation or the A-press is broken")
  r.check(plain:find("A:key", 1, true) ~= nil, "OSK hint row not visible in VRAM")
  -- Keys are drawn spaced out and staggered, so match the real layout.
  r.check(plain:find("q w e r t y u i o p", 1, true) ~= nil,
          "OSK key rows not visible in VRAM")
  -- The selected key must actually be highlighted: that is the only cue the
  -- user gets, and it is the whole reason for the inverse-video font half.
  r.check(L.has_highlight(23, 27), "no inverse-video key on the OSK panel")
  r.check(plain:find("SNOS 0.1", 1, true) ~= nil,
          "banner gone - the OSK overlay corrupted the console shadow buffer")

  local low = text:match("STACKLOW (%x+)")
  if low then
    local v = tonumber(low, 16)
    print(string.format("kernel stack low-water: $%04X (used %d bytes)", v, 0x0C00 - v))
    r.check(v >= 0x0100, string.format("kernel stack left its slot: $%04X", v))
  end

  r.finish()
end, emu.eventType.endFrame)
