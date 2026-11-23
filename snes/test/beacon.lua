-- The boot beacon (make SA1=1 BEACON=1 test-beacon, docs/sa1-notes.md).
--
-- Every write to CGRAM entry 0 is logged, with the bank it was written from,
-- alongside the boot steps each checkpoint stands after: the SA-1 control
-- and Super MMC writes, SBWE and SIWP, MEMSEL, the far call. The eight
-- colours must come in the order of kernel/beacon.h, each after its step and
-- before the next, then con_init's black backdrop; then the console line of
-- word checkpoints and the shell prompt. BEACON_SAFE: no CXB-FXB write and no
-- MEMSEL write at all.
local L = dofile("test/lib.lua")

local SAFE = @BEACON_SAFE@
local BEACON_FAR = @BEACON_FAR@

local RED, GREEN, BLUE, YELLOW = 0x001F, 0x03E0, 0x7C00, 0x03FF
local MAGENTA, CYAN, WHITE, ORANGE = 0x7C1F, 0x7FE0, 0x7FFF, 0x019F
local ORDER = { RED, GREEN, BLUE, YELLOW, MAGENTA, CYAN, WHITE, ORANGE }
local NAME = { [RED] = "red", [GREEN] = "green", [BLUE] = "blue", [YELLOW] = "yellow",
               [MAGENTA] = "magenta", [CYAN] = "cyan", [WHITE] = "white",
               [ORANGE] = "orange", [0] = "black" }

local r = L.new_report("beacon")
local frame = 0
local log = {}          -- { kind, value, pb, frame }
local colours = {}      -- indices into log of entry-0 writes

local function pb() return emu.getState()["cpu.k"] end
local function event(kind, value)
  log[#log + 1] = { kind = kind, value = value, pb = pb(), frame = frame,
                    magic = L.magic() }
  return #log
end

-- CGRAM: $2121 sets the word address and restarts the byte pair; the second
-- $2122 write stores the word and moves to the next.
local cg_word, cg_high, cg_lo = 0, false, 0
emu.addMemoryCallback(function(addr, value)
  cg_word, cg_high = value, false
end, emu.callbackType.write, 0x002121, 0x002121)
emu.addMemoryCallback(function(addr, value)
  if not cg_high then
    cg_lo, cg_high = value, true
    return
  end
  if cg_word == 0 then
    colours[#colours + 1] = event("colour", cg_lo | (value << 8))
  end
  cg_word, cg_high = (cg_word + 1) & 0xFF, false
end, emu.callbackType.write, 0x002122, 0x002122)

for _, a in ipairs({ 0x2200, 0x2201, 0x2202, 0x2220, 0x2221, 0x2222, 0x2223,
                     0x2226, 0x2229, 0x420D }) do
  emu.addMemoryCallback(function(addr, value) event(string.format("%04X", a), value) end,
                        emu.callbackType.write, a, a)
end
emu.addMemoryCallback(function() event("far", 0) end,
                      emu.callbackType.exec, BEACON_FAR, BEACON_FAR)

-- Index of the first event of `kind` (after `from`), or nil.
local function first(kind, from)
  for i = (from or 0) + 1, #log do
    if log[i].kind == kind then return i end
  end
  return nil
end
local function count(kind)
  local n = 0
  for _, e in ipairs(log) do if e.kind == kind then n = n + 1 end end
  return n
end
-- `kind` happens strictly between log indices lo and hi.
local function between(kind, lo, hi)
  local i = first(kind, lo)
  return i ~= nil and i < hi
end

local function prompt(text)
  local at = text:find("SELECT opens the keyboard\n", 1, true)
  return at ~= nil and text:find("\n> ", at, true) ~= nil
end

local function judge()
  local text = L.ring_text()
  local screen = L.screen_text_plain(6)
  print("--- colours ---")
  for n, i in ipairs(colours) do
    local e = log[i]
    print(string.format("%2d %-7s %04X from $%02X at frame %d", n,
                        NAME[e.value] or "?", e.value, e.pb, e.frame))
  end
  print("--- screen ---"); print(screen); print("--------------")

  r.check(#colours >= 9, string.format("%d writes to CGRAM 0, not 9 or more", #colours))
  for n, want in ipairs(ORDER) do
    local i = colours[n]
    r.check(i ~= nil and log[i].value == want, string.format(
      "checkpoint %d is %s, not %s", n, i and string.format("%04X", log[i].value) or "missing",
      NAME[want]))
  end
  if #colours < 9 then r.finish() return end
  r.check(log[colours[9]].value == 0, "con_init did not follow orange with its black backdrop")
  local c = {}
  for n = 1, 9 do c[n] = colours[n] end

  -- 1 red: from bank $00, before any other step.
  r.check(c[1] == 1, "something was logged before the reset stub's red")
  r.check(log[c[1]].pb == 0x00, string.format("red written from $%02X", log[c[1]].pb))
  -- 2 green: the first code from $C0 and up.
  r.check(log[c[2]].pb >= 0xC0, string.format("green written from $%02X", log[c[2]].pb))
  -- 3 blue: main, nothing of sa1_hold yet; its bank is main's.
  local main_pb = log[c[3]].pb
  r.check((first("2200") or 0) > c[3], "CCNT not written, or written before main's blue")
  -- 4 yellow after CCNT, SIE, SIC; 5 magenta after CXB-FXB.
  for _, k in ipairs({ "2200", "2201", "2202" }) do
    r.check(between(k, c[3], c[4]), k .. " not written between blue and yellow")
  end
  for _, k in ipairs({ "2220", "2221", "2222", "2223" }) do
    if SAFE then
      r.check(count(k) == 0, k .. " written by the safe image")
    else
      r.check(between(k, c[4], c[5]), k .. " not written between yellow and magenta")
    end
  end
  -- 6 cyan after SBWE and SIWP.
  for _, k in ipairs({ "2226", "2229" }) do
    r.check(between(k, c[5], c[6]), k .. " not written between magenta and cyan")
  end
  -- 7 white after MEMSEL = 1 (or none) and the far call, from another bank.
  if SAFE then
    r.check(count("420D") == 0, "the safe image wrote MEMSEL")
  else
    local m = first("420D", c[6])
    r.check(m ~= nil and m < c[7] and log[m].value == 1, "MEMSEL = 1 not between cyan and white")
    r.check(m ~= nil and (first("far", c[6]) or 0) > m, "the far call came before MEMSEL")
  end
  local f = first("far", c[6])
  r.check(f ~= nil and f < c[7], "beacon_far did not run between cyan and white")
  if f then
    r.check(log[f].pb ~= main_pb and log[f].pb >= 0xC0, string.format(
      "beacon_far ran from $%02X, main from $%02X", log[f].pb, main_pb))
  end
  -- 8 orange after mb_init: the mailbox is up.
  r.check(log[c[7]].magic ~= 0xCAFE and log[c[8]].magic == 0xCAFE,
          "the mailbox magic did not appear between white and orange")

  -- The console checkpoints, and the boot carried on to the prompt.
  r.check(screen:find("beacon: con nmi scan sram fs", 1, true) ~= nil,
          "the console line of checkpoints is not on screen")
  r.check(text:match("beacon: con.- nmi scan.- sram fs\n") ~= nil,
          "the console line of checkpoints is not in the transcript")
  r.check(prompt(text), "no shell prompt")
  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.finish()
end

local judged = false
emu.addEventCallback(function()
  frame = frame + 1
  if judged then return end
  if L.magic() == 0xCAFE and prompt(L.ring_text()) then
    judged = true
    judge()
  elseif frame > 1500 then
    print(L.ring_text())
    judged = true
    judge()
  end
end, emu.eventType.endFrame)
