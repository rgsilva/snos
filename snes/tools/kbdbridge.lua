-- Type into SNOS with your computer's keyboard.
--
-- WHY THIS WORKS AT ALL: the kernel's input layer (kernel/input.h) has always
-- had three interchangeable backends behind kbd_getchar(), and one of them is a
-- byte ring in the test mailbox. The automated tests type through it. So does
-- this. Nothing in the kernel knows or cares that a human is at the other end,
-- and nothing had to change to support it.
--
-- HOW TO USE IT
--   1. Open snos.sfc in Mesen (the normal GUI, not --testrunner).
--   2. Debug -> Script Window, load this file, press Run.
--   3. Click the emulator window and type.
--
-- It cannot work under --testrunner: that mode never initialises the input
-- subsystem, so every key name fails to resolve.
--
-- Mesen's key names are not documented anywhere I could find and differ between
-- platforms, so rather than guess, every logical key is probed against a list
-- of plausible spellings at load time and the first one that resolves wins.
-- The report it prints tells you what was found; if something is missing, add
-- the spelling your build uses to the table below.

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

-- Each entry: { unshifted, shifted, candidate key names... }
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

  { " ", " ", "Space", "Spacebar", "SpaceBar" },
  { "\n", "\n", "Enter", "Return", "KeypadEnter" },
  { "\b", "\b", "Backspace", "Back", "BackSpace" },

  { "-", "_", "Minus", "OemMinus", "-", "Subtract" },
  { "=", "+", "Equal", "Equals", "OemPlus", "=" },
  { "[", "{", "OpenBracket", "LeftBracket", "OemOpenBrackets", "[" },
  { "]", "}", "CloseBracket", "RightBracket", "Oem6", "]" },
  { ";", ":", "Semicolon", "OemSemicolon", "Oem1", ";" },
  { "'", '"', "Quote", "Apostrophe", "OemQuotes", "Oem7", "'" },
  { ",", "<", "Comma", "OemComma", "," },
  { ".", ">", "Period", "OemPeriod", "." },
  { "/", "?", "Slash", "OemQuestion", "Oem2", "/" },
}

local SHIFT_NAMES = { "Shift", "LeftShift", "RightShift", "ShiftLeft" }

local function resolve(names, from)
  for i = from, #names do
    local ok, _ = pcall(function() return emu.isKeyPressed(names[i]) end)
    if ok then return names[i] end
  end
  return nil
end

local bound, missing = {}, {}
for _, k in ipairs(KEYS) do
  local name = resolve(k, 3)
  if name then
    bound[#bound + 1] = { lower = k[1], upper = k[2], key = name, down = false }
  else
    missing[#missing + 1] = string.format("%q", k[1])
  end
end

local shift_key = resolve(SHIFT_NAMES, 1)

print(string.format("kbdbridge: %d keys bound%s", #bound,
      shift_key and (", shift = " .. shift_key) or ", NO shift key found"))
if #missing > 0 then
  print("kbdbridge: unresolved -> " .. table.concat(missing, " ") ..
        "  (add the spelling your Mesen build uses to KEYS)")
end
if #bound == 0 then
  print("kbdbridge: nothing bound. This script only works in the Mesen GUI; " ..
        "--testrunner never initialises the input subsystem.")
end

emu.addEventCallback(function()
  -- Only inject when the ring has room, so a burst of typing cannot outrun the
  -- kernel and wrap the buffer on top of unread bytes.
  local shifted = shift_key and emu.isKeyPressed(shift_key) or false
  for _, b in ipairs(bound) do
    local now = emu.isKeyPressed(b.key)
    if now and not b.down then
      push(string.byte(shifted and b.upper or b.lower))
    end
    b.down = now
  end
end, emu.eventType.endFrame)
