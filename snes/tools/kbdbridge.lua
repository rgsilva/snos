-- Type into SNOS with your computer's keyboard.
--
-- WHY THIS WORKS AT ALL: the kernel's input layer (kernel/input.h) has always
-- had interchangeable backends behind kbd_getchar(), and one of them is a byte
-- ring in the test mailbox. The automated tests type through it. So does this.
-- Nothing in the kernel knows or cares that a human is at the other end.
--
-- HOW TO USE IT
--   1. Open snos.sfc in Mesen (the normal GUI, not --testrunner).
--   2. Debug -> Script Window, load this file, press Run.
--   3. Click the emulator window and type.
--
-- It cannot work under --testrunner: that mode never initialises the input
-- subsystem, so every key name fails to resolve.
--
-- ------------------------------------------------------------------ names
-- Mesen's key names are undocumented and differ by platform, and there is no
-- API to enumerate them. So rather than pick one spelling per key and hope,
-- every candidate spelling that Mesen ACCEPTS is kept, and the key counts as
-- pressed if ANY of them is down. A name being valid does not mean it is the
-- one your platform actually reports — that is exactly how the first version
-- ended up with a "Shift" that resolved fine and never fired.
--
-- If a key still does not work, set LEARN below and press it: the script prints
-- the real name.

local LEARN = false     -- print the name of every key you press, then stop
local REPEAT_DELAY = 24 -- frames held before auto-repeat starts
local REPEAT_RATE = 3   -- frames between repeats

local MB = 0xF000            -- mailbox base, WRAM $7EF000 (kernel/mailbox.h)
local IN_WINDEX = MB + 0x006
local IN_RING = MB + 0xE00
local IN_SIZE = 512
local WRAM = emu.memType.snesWorkRam

local function push(byte)
  local wi = emu.read16(IN_WINDEX, WRAM)
  emu.write(IN_RING + (wi % IN_SIZE), byte, WRAM)
  emu.write16(IN_WINDEX, (wi + 1) % 65536, WRAM)
end

-- { unshifted, shifted, candidate spellings... }
local KEYS = {
  { "a", "A", "A" }, { "b", "B", "B" }, { "c", "C", "C" }, { "d", "D", "D" },
  { "e", "E", "E" }, { "f", "F", "F" }, { "g", "G", "G" }, { "h", "H", "H" },
  { "i", "I", "I" }, { "j", "J", "J" }, { "k", "K", "K" }, { "l", "L", "L" },
  { "m", "M", "M" }, { "n", "N", "N" }, { "o", "O", "O" }, { "p", "P", "P" },
  { "q", "Q", "Q" }, { "r", "R", "R" }, { "s", "S", "S" }, { "t", "T", "T" },
  { "u", "U", "U" }, { "v", "V", "V" }, { "w", "W", "W" }, { "x", "X", "X" },
  { "y", "Y", "Y" }, { "z", "Z", "Z" },

  { "1", "!", "1", "D1", "Number1", "Digit1" },
  { "2", "@", "2", "D2", "Number2", "Digit2" },
  { "3", "#", "3", "D3", "Number3", "Digit3" },
  { "4", "$", "4", "D4", "Number4", "Digit4" },
  { "5", "%", "5", "D5", "Number5", "Digit5" },
  { "6", "^", "6", "D6", "Number6", "Digit6" },
  { "7", "&", "7", "D7", "Number7", "Digit7" },
  { "8", "*", "8", "D8", "Number8", "Digit8" },
  { "9", "(", "9", "D9", "Number9", "Digit9" },
  { "0", ")", "0", "D0", "Number0", "Digit0" },

  { " ", " ", "Space", "Spacebar", "Space Bar", "SpaceBar" },
  { "\n", "\n", "Return", "Enter", "Keypad Enter", "KeypadEnter" },

  -- On a Mac the key labelled "delete" is what everyone else calls Backspace,
  -- and Mesen may report it under either name. Both are accepted; a Mac has no
  -- separate forward-delete to be confused with.
  { "\b", "\b", "Backspace", "BackSpace", "Back Space", "Back", "Delete", "Del" },

  { "-", "_", "Minus", "OemMinus", "-", "Subtract" },
  { "=", "+", "Equal", "Equals", "OemPlus", "=" },
  { "[", "{", "OpenBracket", "LeftBracket", "Left Bracket", "OemOpenBrackets", "[" },
  { "]", "}", "CloseBracket", "RightBracket", "Right Bracket", "Oem6", "]" },
  { ";", ":", "Semicolon", "OemSemicolon", "Oem1", ";" },
  { "'", '"', "Quote", "Apostrophe", "OemQuotes", "Oem7", "'" },
  { ",", "<", "Comma", "OemComma", "," },
  { ".", ">", "Period", "OemPeriod", "." },
  { "/", "?", "Slash", "OemQuestion", "Oem2", "/" },
  { "\\", "|", "Backslash", "Back Slash", "OemPipe", "Oem5", "\\" },
  { "`", "~", "Grave", "Backquote", "Tilde", "OemTilde", "`" },
}

-- Every plausible spelling, because a name that resolves is not necessarily the
-- one the platform reports.
local SHIFT_NAMES = {
  "Left Shift", "Right Shift", "LeftShift", "RightShift",
  "Shift Left", "Shift Right", "LShift", "RShift", "Shift",
}

local function valid(name)
  local ok = pcall(function() return emu.isKeyPressed(name) end)
  return ok
end

-- Keep ALL accepted spellings, not just the first.
local function accepted(names, from)
  local out = {}
  for i = from, #names do
    if valid(names[i]) then out[#out + 1] = names[i] end
  end
  return out
end

local function any_down(names)
  for _, n in ipairs(names) do
    if emu.isKeyPressed(n) then return true end
  end
  return false
end

local bound, missing = {}, {}
for _, k in ipairs(KEYS) do
  local names = accepted(k, 3)
  if #names > 0 then
    bound[#bound + 1] = { lower = k[1], upper = k[2], names = names, held = 0 }
  else
    missing[#missing + 1] = string.format("%q", k[1])
  end
end

local shift_names = accepted(SHIFT_NAMES, 1)

print(string.format("kbdbridge: %d keys bound", #bound))
if #shift_names > 0 then
  print("kbdbridge: shift = " .. table.concat(shift_names, ", "))
else
  print("kbdbridge: NO shift key accepted - uppercase will not work. " ..
        "Set LEARN = true, press shift, and add the name it prints.")
end
if #missing > 0 then
  print("kbdbridge: unresolved -> " .. table.concat(missing, " "))
end
if #bound == 0 then
  print("kbdbridge: nothing bound. This only works in the Mesen GUI; " ..
        "--testrunner never initialises the input subsystem.")
end

-- LEARN mode: poll every spelling this build accepts and report what is down,
-- so an unknown key can be identified in one keypress instead of a guess.
local vocabulary = {}
if LEARN then
  local seen = {}
  for _, k in ipairs(KEYS) do
    for i = 3, #k do
      if not seen[k[i]] and valid(k[i]) then seen[k[i]] = true; vocabulary[#vocabulary + 1] = k[i] end
    end
  end
  for _, n in ipairs(SHIFT_NAMES) do
    if not seen[n] and valid(n) then seen[n] = true; vocabulary[#vocabulary + 1] = n end
  end
  print("kbdbridge: LEARN mode, polling " .. #vocabulary .. " names. Press a key.")
end

local learn_last = ""

emu.addEventCallback(function()
  if LEARN then
    local down = {}
    for _, n in ipairs(vocabulary) do
      if emu.isKeyPressed(n) then down[#down + 1] = n end
    end
    local line = table.concat(down, ", ")
    if line ~= learn_last then
      if line ~= "" then print("kbdbridge: down -> " .. line) end
      learn_last = line
    end
    return
  end

  local shifted = any_down(shift_names)
  for _, b in ipairs(bound) do
    if any_down(b.names) then
      -- Fire on the press, then auto-repeat: holding backspace to delete a
      -- word should not mean tapping it eight times.
      if b.held == 0 or
         (b.held >= REPEAT_DELAY and ((b.held - REPEAT_DELAY) % REPEAT_RATE) == 0) then
        push(string.byte(shifted and b.upper or b.lower))
      end
      b.held = b.held + 1
    else
      b.held = 0
    end
  end
end, emu.eventType.endFrame)
