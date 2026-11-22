-- The second half of make SA1=1 test-sa1: a power-on from the battery save
-- Mesen wrote when test/sa1.lua ended. The files are still there, and so is
-- the pattern in the SA-1's 64 KB, which this boot did not touch either.
local L = dofile("test/lib.lua")

local SRAM_KB = tonumber("@SRAM_KB@")
local S = emu.memType.snesSaveRam
local LIMIT = SRAM_KB * 1024
local BWRAM = emu.getMemorySize(S)

local r = L.new_report("sa1persist")
local frame, step, at = 0, 1, nil

local function pattern(i) return (i * 37 + (i >> 8) + 0x5B) & 0xFF end

local function contains(needle) return L.ring_text():find(needle, 1, true) ~= nil end

emu.addEventCallback(function()
  frame = frame + 1
  if frame > 1500 or L.status() == L.STATUS_PANIC then
    print(L.ring_text())
    r.check(false, "timeout or kernel panic")
    r.finish()
    return
  end
  if L.magic() ~= 0xCAFE then return end
  if at == nil then at = frame + 150 end
  if frame < at then return end

  if step == 1 then
    L.feed("cat SA1.TXT\ncat EDGE.TXT\n")
    at = frame + 260
    step = 2
    return
  end

  print(L.ring_text())
  local st = emu.getState()
  r.check(contains(SRAM_KB .. "K SRAM"), "boot line does not say " .. SRAM_KB .. "K")
  r.check(contains("kept on BW-RAM"), "SA1.TXT did not survive the power-on")
  r.check(contains("abcde"), "EDGE.TXT did not survive the power-on")
  local bad = nil
  for i = LIMIT, BWRAM - 1 do
    if not bad and emu.read(i, S) ~= pattern(i) then
      bad = string.format("offset $%05X", i)
    end
  end
  r.check(bad == nil, "the SA-1's work area changed across the power-on: " .. tostring(bad))
  r.check(st["cart.coprocessor.sa1Reset"] == true, "the SA-1 is not in reset")
  r.finish()
end, emu.eventType.endFrame)
