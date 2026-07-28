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
-- ------------------------------------------------------- if a key does nothing
-- CHECK MESEN'S OWN INPUT BINDINGS FIRST. Mesen consumes a key that is bound to
-- a controller button or a hotkey before any script sees it, so the key simply
-- never arrives here and the script cannot tell the difference between that and
-- a wrong name. Backspace turned out to be bound by default; unbinding it in
-- Mesen's settings fixed it instantly.
--
-- Only if the binding is clear and the key still does nothing is it a naming
-- problem — the startup report below prints what each named key resolved to,
-- and every name this build accepts.

local LEARN = false     -- also print the name of every key you press
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

  -- Arrows send SNOS_KEY_UP..RIGHT ($11-$14), not ANSI escape sequences: ESC
  -- already means "open the menu" in the editor, and telling `ESC [ A` from a
  -- bare ESC would need a timeout. See kernel/snos_abi.h.
  { "\17", "\17", "Up", "Up Arrow", "UpArrow", "ArrowUp" },
  { "\18", "\18", "Down", "Down Arrow", "DownArrow", "ArrowDown" },
  { "\19", "\19", "Left", "Left Arrow", "LeftArrow", "ArrowLeft" },
  { "\20", "\20", "Right", "Right Arrow", "RightArrow", "ArrowRight" },

  -- Escape sends $1B. The editor accepts it as an alternative to Ctrl+L for
  -- opening the menu, which matters because Ctrl combinations are exactly the
  -- ones a host is most likely to have bound to something else.
  { "\27", "\27", "Escape", "Esc" },
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
local CTRL_NAMES = {
  "Left Ctrl", "Right Ctrl", "LeftControl", "RightControl",
  "Ctrl Left", "Ctrl Right", "LCtrl", "RCtrl", "Ctrl", "Control",
}

local CAPS_NAMES = { "Caps Lock", "CapsLock", "Caps" }

local SHIFT_NAMES = {
  "Left Shift", "Right Shift", "LeftShift", "RightShift",
  "Shift Left", "Shift Right", "LShift", "RShift", "Shift",
}

-- A wide net of plausible spellings, used for two things: binding keys, and
-- reporting what this Mesen build actually accepts. The report is the important
-- part — if a key is missing, the accepted-names list says what vocabulary the
-- platform uses, without anyone having to guess twice.
local EXTRA_VOCAB = {
  "Backspace", "BackSpace", "Back Space", "Back", "Delete", "Del", "DEL",
  "Forward Delete", "ForwardDelete", "Clear", "Erase",
  "Return", "Enter", "Keypad Enter", "KeypadEnter", "Num Enter",
  "Space", "Spacebar", "Space Bar", "SpaceBar",
  "Tab", "Escape", "Esc", "Caps Lock", "CapsLock",
  "Left Shift", "Right Shift", "LeftShift", "RightShift",
  "Shift Left", "Shift Right", "LShift", "RShift", "Shift",
  "Left Ctrl", "Right Ctrl", "LeftControl", "Control", "Ctrl",
  "Left Alt", "Right Alt", "Alt", "Option", "Left Option",
  "Left Cmd", "Right Cmd", "Command", "Meta", "Left GUI", "Right GUI", "Super",
  "Up", "Down", "Left", "Right", "Up Arrow", "Down Arrow",
  "Minus", "Hyphen", "Equal", "Equals", "Plus", "Comma", "Period", "Dot",
  "Slash", "Backslash", "Semicolon", "Quote", "Apostrophe", "Grave", "Tilde",
  "Left Bracket", "Right Bracket", "LeftBracket", "RightBracket",
  "F1", "F2", "F3", "F4", "F5", "F6",
}

local function valid(name)
  return (pcall(function() return emu.isKeyPressed(name) end))
end

-- Keep ALL accepted spellings, not just the first: a name being accepted does
-- not mean it is the one the platform reports.
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
    bound[#bound + 1] = {
      lower = k[1], upper = k[2], names = names, held = 0,
      letter = k[1]:match("^%a$") ~= nil,
    }
  else
    missing[#missing + 1] = string.format("%q", k[1])
  end
end

local shift_names = accepted(SHIFT_NAMES, 1)
local ctrl_names = accepted(CTRL_NAMES, 1)
local caps_names = accepted(CAPS_NAMES, 1)

-- Everything this build accepts, so the naming scheme is visible.
local vocabulary = {}
do
  local seen = {}
  local function add(n)
    if not seen[n] and valid(n) then seen[n] = true; vocabulary[#vocabulary + 1] = n end
  end
  for _, k in ipairs(KEYS) do for i = 3, #k do add(k[i]) end end
  for _, n in ipairs(SHIFT_NAMES) do add(n) end
  for _, n in ipairs(CTRL_NAMES) do add(n) end
  for _, n in ipairs(CAPS_NAMES) do add(n) end
  for _, n in ipairs(EXTRA_VOCAB) do add(n) end
end

print(string.format("kbdbridge: %d keys bound", #bound))
if #shift_names > 0 then
  print("kbdbridge: shift = " .. table.concat(shift_names, ", "))
else
  print("kbdbridge: NO shift key accepted - uppercase will not work")
end
if #ctrl_names > 0 then
  print("kbdbridge: ctrl = " .. table.concat(ctrl_names, ", ") ..
        "  (ctrl+letter sends a control code, e.g. ctrl+L = $0C)")
else
  print("kbdbridge: NO ctrl key accepted - use Escape for the editor menu")
end
if #caps_names > 0 then
  print("kbdbridge: caps lock = " .. table.concat(caps_names, ", "))
end
if #missing > 0 then
  print("kbdbridge: no accepted name for -> " .. table.concat(missing, " "))
end

-- The named keys are the ones that go wrong; letters and digits never do. Show
-- exactly what each bound to, so "backspace is not working" can be diagnosed
-- from the report instead of from a guess.
for _, b in ipairs(bound) do
  local label = (b.lower == "\n" and "enter") or (b.lower == "\b" and "backspace")
             or (b.lower == " " and "space") or nil
  if label then
    print("kbdbridge: " .. label .. " = " .. table.concat(b.names, ", "))
  end
end

-- Names longer than one character are where the ambiguity lives; the letters
-- and digits are never in doubt. Printing these tells us the vocabulary.
do
  local special = {}
  for _, n in ipairs(vocabulary) do
    if #n > 1 then special[#special + 1] = n end
  end
  table.sort(special)
  print("kbdbridge: accepted special keys (" .. #special .. "): " ..
        table.concat(special, ", "))
  print("kbdbridge: if a key you need is not in that list, tell me what IS.")
end

if #bound == 0 then
  print("kbdbridge: nothing bound. This only works in the Mesen GUI; " ..
        "--testrunner never initialises the input subsystem.")
end
if LEARN then
  print("kbdbridge: LEARN on, polling " .. #vocabulary ..
        " names. Typing still works.")
end

local learn_last = ""
local caps_toggle, caps_held, caps_is_level = false, 0, false

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
    -- deliberately falls through: diagnosing a key should not stop you typing
  end

  -- Caps Lock reports differently depending on the platform: some report a
  -- momentary press, others hold the key down for as long as the light is on.
  -- Rather than guess, learn which: if it is still down well after the press,
  -- it is a level key and its state IS the caps state; otherwise each press
  -- toggles.
  local caps_now = any_down(caps_names)
  if caps_now then
    caps_held = caps_held + 1
    if caps_held == 1 then caps_toggle = not caps_toggle end
    if caps_held > 20 then caps_is_level = true end
  else
    caps_held = 0
  end
  local caps = caps_is_level and caps_now or caps_toggle

  local shifted = any_down(shift_names)
  local ctrl = any_down(ctrl_names)

  for _, b in ipairs(bound) do
    if any_down(b.names) then
      -- Fire on the press, then auto-repeat: holding backspace to delete a
      -- word should not mean tapping it eight times.
      if b.held == 0 or
         (b.held >= REPEAT_DELAY and ((b.held - REPEAT_DELAY) % REPEAT_RATE) == 0) then
        local ch
        if ctrl and b.letter then
          -- ctrl+letter -> control code. This is what makes the editor's ^L,
          -- ^N, ^O, ^S, ^Q reachable from a host keyboard at all.
          ch = string.byte(b.upper) & 0x1F
        elseif b.letter then
          -- Caps affects letters only, never digits or symbols.
          ch = string.byte((shifted ~= caps) and b.upper or b.lower)
        else
          ch = string.byte(shifted and b.upper or b.lower)
        end
        push(ch)
      end
      b.held = b.held + 1
    else
      b.held = 0
    end
  end
end, emu.eventType.endFrame)
