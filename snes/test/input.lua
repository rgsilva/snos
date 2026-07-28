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
-- The keyboard starts closed now, so the first press has to summon it.
local SCRIPT = {
  { 0, "select" }, -- open the keyboard
  { 20, "a" },     -- 'q'
  { 32, "right" }, -- move to 'w'
  { 44, "a" },     -- 'w'
  { 60, "start" }, -- submit
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
      L.feed("echo hello world\n")
      phase = 2
      press_base = frame + 120
    end
    return
  end

  -- The shell never exits, so settle for a fixed window rather than waiting
  -- for a PASS status (see boot.lua for the same reasoning).
  if phase == 2 then
    if frame > press_base + 240 then phase = 3 end
    return
  end

  local status = L.status()
  if status == L.STATUS_PANIC then
    print("RESULT FAIL input - kernel panicked")
    print(L.ring_text())
    emu.stop(1)
  end

  local r = L.new_report("input")
  local text = L.ring_text()
  local screen = L.screen_text(28)
  local plain = L.screen_text_plain(28)

  print("--- mailbox ---"); print(text)
  print("--- screen (from VRAM) ---"); print(screen)
  print("--------------------------")

  r.check(status ~= L.STATUS_PANIC, "kernel panicked")

  -- Backend 1: the harness feed drove a real shell command.
  r.check(text:find("hello world", 1, true) ~= nil,
          "mailbox-fed command never reached the shell")
  -- Backend 2: the same shell, typed on the on-screen keyboard. 'qw' is not a
  -- command, so the shell rejecting it by name proves the characters arrived.
  r.check(text:find("unknown command: qw", 1, true) ~= nil,
          "OSK did not produce 'qw' - navigation or the A-press is broken")
  -- The keyboard is read from its own tilemap.
  local panel = L.panel_text_plain()
  print("--- keyboard panel ---"); print(panel)
  r.check(panel:find("1 2 3 4 5 6 7 8 9 0", 1, true) ~= nil, "number row missing")
  r.check(panel:find("q w e r t y u i o p", 1, true) ~= nil, "key rows missing")
  r.check(panel:find("A type key", 1, true) ~= nil, "controller mapping missing")
  r.check(panel:find("SELECT hide", 1, true) ~= nil, "controller mapping truncated")
  r.check(L.panel_has_highlight(), "no key drawn inverse - selection invisible")

  -- The crucial one: after all that scrolling, the keyboard must not have
  -- leaked into the TEXT map. The old version scrolled with the console and
  -- destroyed whatever it drifted over.
  -- Match on something ONLY the panel can produce. "keyboard" alone is not
  -- specific enough any more: the boot message legitimately mentions it when
  -- telling the user that SELECT opens one.
  local textmap = L.screen_text_plain(22)
  r.check(textmap:find("q w e r t y", 1, true) == nil,
          "keyboard key rows have leaked into the scrolling text area")
  r.check(textmap:find("A type key", 1, true) == nil,
          "keyboard hints have leaked into the scrolling text area")

  local low = text:match("STACKLOW (%x+)")
  if low then
    local v = tonumber(low, 16)
    print(string.format("kernel stack low-water: $%04X (used %d bytes)", v, 0x0C00 - v))
    r.check(v >= 0x0100, string.format("kernel stack left its slot: $%04X", v))
  end

  r.finish()
end, emu.eventType.endFrame)
