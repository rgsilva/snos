-- The SA-1 image (make SA1=1 test-sa1), phase 1: the kernel on the SA-1
-- memory map, with the chip held in reset.
--
-- The filesystem is the first @SRAM_KB@ KB of BW-RAM; the 64 KB above it are
-- the SA-1's and the kernel must never write them. They are filled with a
-- pattern before the first boot, and every boot after that (power-on, an
-- emulator reset, the shell's `restart`) and every write, including one that
-- runs into the limit, must leave the pattern whole. The file written here is
-- read back by test/sa1persist.lua after Mesen has saved and reloaded the
-- battery RAM.
local L = dofile("test/lib.lua")

local SRAM_KB = tonumber("@SRAM_KB@")
local S = emu.memType.snesSaveRam
local LIMIT = SRAM_KB * 1024
local BWRAM = emu.getMemorySize(S)

local r = L.new_report("sa1")
local frame, step, at = 0, 0, nil
local transcripts = {}
local sa1_samples, sa1_bad = 0, nil

local function pattern(i) return (i * 37 + (i >> 8) + 0x5B) & 0xFF end

local function fill_reserved()
  for i = LIMIT, BWRAM - 1 do emu.write(i, pattern(i), S) end
end

local function reserved_damage()
  for i = LIMIT, BWRAM - 1 do
    if emu.read(i, S) ~= pattern(i) then
      return string.format("BW-RAM offset $%05X reads %02X, not %02X",
                           i, emu.read(i, S), pattern(i))
    end
  end
  return nil
end

-- What Mesen says about the chip: held in reset, and never ran.
local function sample_sa1()
  local st = emu.getState()
  sa1_samples = sa1_samples + 1
  if st["cart.coprocessor.sa1Reset"] ~= true or st["cart.coprocessor.cpu.pc"] ~= 0 then
    sa1_bad = sa1_bad or string.format("frame %d: sa1Reset=%s pc=%s", frame,
      tostring(st["cart.coprocessor.sa1Reset"]), tostring(st["cart.coprocessor.cpu.pc"]))
  end
  return st
end

-- The final state cannot show a write that was put back (sram_probe restores
-- what it finds), so the bus is watched as well: no S-CPU write to the work
-- area at $40:0000 + LIMIT or its mirrors up to $4F, and no access at all to
-- the $6000-$7FFF BW-RAM window, the stock image's SRAM path.
local bus = { work = 0, window = 0, first = nil }
local function on_bus(kind)
  return function(addr)
    bus[kind] = bus[kind] + 1
    bus.first = bus.first or string.format("%s $%06X at frame %d", kind, addr, frame)
  end
end
for m = 0, 0xC, 4 do
  local lo = 0x400000 + m * 0x10000 + LIMIT
  local hi = 0x400000 + m * 0x10000 + BWRAM - 1
  emu.addMemoryCallback(on_bus("work"), emu.callbackType.write, lo, hi)
end
for _, half in ipairs({ 0x00, 0x80 }) do
  for b = half, half + 0x3F do
    local lo, hi = (b << 16) | 0x6000, (b << 16) | 0x7FFF
    emu.addMemoryCallback(on_bus("window"), emu.callbackType.write, lo, hi)
    emu.addMemoryCallback(on_bus("window"), emu.callbackType.read, lo, hi)
  end
end

local function set_alloc(v)
  for i = 0, 3 do emu.write(8 + i, (v >> (8 * i)) & 0xFF, S) end
end

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

local function contains(needle) return L.ring_text():find(needle, 1, true) ~= nil end

local function keep_transcript() transcripts[#transcripts + 1] = L.ring_text() end

fill_reserved()

emu.addEventCallback(function()
  frame = frame + 1
  if frame % 30 == 0 then sample_sa1() end
  if frame > 4000 or L.status() == L.STATUS_PANIC then
    print(L.ring_text())
    r.check(false, "timeout or kernel panic at step " .. step)
    r.finish()
    return
  end
  -- The script may start after the first instructions ran: fill again and
  -- reset, so that every boot this test sees comes after the fill.
  if step == 0 then
    fill_reserved()
    emu.reset()
    step = 1
    return
  end
  if L.magic() ~= 0xCAFE then return end
  if at == nil then at = frame + 150 end
  if frame < at then return end

  if step == 1 then
    r.check(contains(string.format("SRAMKB %08x", SRAM_KB)),
            "the probe did not stop at the " .. SRAM_KB .. " KB filesystem limit")
    r.check(contains(SRAM_KB .. "K SRAM"), "boot line does not say " .. SRAM_KB .. "K")
    send("mem\ndebug ver\n", 260)
    step = 2; return
  end
  if step == 2 then
    local text = L.ring_text()
    r.check(contains("cart SRAM " .. SRAM_KB .. " KB"), "mem: volume is not " .. SRAM_KB .. " KB")
    r.check(contains("SA-1 yes, held in reset\n"), "mem: no SA-1 line, or I-RAM did not answer")
    r.check(contains(string.format("BW-RAM files %dK at $400000\n", SRAM_KB)),
            "mem: no filesystem line")
    r.check(contains(string.format("BW-RAM SA-1  %3dK at $%06X\n", (BWRAM - LIMIT) // 1024,
                                   0x400000 + LIMIT)), "mem: no SA-1 work area line")
    for line in text:gmatch("[^\n]+") do
      if line:find("^SA%-1 ") or line:find("^BW%-RAM ") then
        r.check(#line <= 31, "mem line wider than 31 columns: '" .. line .. "'")
      end
    end
    r.check(contains("SA-1 cart"), "debug ver does not name the SA-1 cartridge")
    send("write SA1.TXT kept on BW-RAM\n", 260)
    step = 3; return
  end
  if step == 3 then
    r.check(contains("wrote SA1.TXT"), "write failed")
    keep_transcript()
    emu.reset()
    at = frame + 300
    step = 4; return
  end
  if step == 4 then send("cat SA1.TXT\n", 240); step = 5; return end
  if step == 5 then
    r.check(contains("kept on BW-RAM"), "file lost across an emulator reset")
    keep_transcript()
    send("restart\n", 360)
    step = 6; return
  end
  if step == 6 then
    sample_sa1()
    r.check(contains("type 'help' for commands"), "restart did not come back to the shell")
    send("cat SA1.TXT\n", 240)
    step = 7; return
  end
  if step == 7 then
    r.check(contains("kept on BW-RAM"), "file lost across the shell's restart")
    -- A write that runs into the limit: three bytes fit, the fourth is refused.
    set_alloc(LIMIT - 3)
    send("write OVER.TXT abcde\n", 260)
    step = 8; return
  end
  if step == 8 then
    r.check(contains("write failed: volume full, nothing written"),
            "a write past the limit was not refused")
    -- One that ends exactly on it.
    set_alloc(LIMIT - 6)
    send("write EDGE.TXT abcde\n", 260)
    step = 9; return
  end
  if step == 9 then
    r.check(contains("wrote EDGE.TXT"), "a write ending on the limit was refused")
    send("cat EDGE.TXT\nwrite MORE.TXT x\n", 300)
    step = 10; return
  end

  local st = sample_sa1()
  r.check(contains("abcde"), "EDGE.TXT does not read back")
  r.check(contains("cannot create MORE.TXT"), "a full volume accepted a new file")
  local edge = ""
  for i = LIMIT - 6, LIMIT - 1 do edge = edge .. string.char(emu.read(i, S)) end
  r.check(edge == "abcde\n", "the last six bytes below the limit are not EDGE.TXT's")
  local damage = reserved_damage()
  r.check(damage == nil, "the SA-1's work area was written: " .. tostring(damage))
  r.check(bus.work == 0 and bus.window == 0, string.format(
    "%d writes to the SA-1's work area, %d window accesses; first: %s",
    bus.work, bus.window, tostring(bus.first)))
  r.check(sa1_bad == nil, "the SA-1 left reset: " .. tostring(sa1_bad))
  r.check(sa1_samples > 50, "too few SA-1 samples (" .. sa1_samples .. ")")
  -- The registers sa1_hold() writes, as Mesen latched them.
  r.check(st["cart.coprocessor.cpuIrqEnabled"] == false, "SIE: IRQ from the SA-1 enabled")
  r.check(st["cart.coprocessor.cpuBwWriteEnabled"] == true, "SBWE not set")
  r.check(st["cart.coprocessor.cpuIRamWriteProtect"] == 0xFF, "SIWP not $FF")
  for i = 0, 3 do
    r.check(st["cart.coprocessor.banks[" .. i .. "]"] == i, "Super MMC bank " .. i .. " moved")
  end
  keep_transcript()
  for i, text in ipairs(transcripts) do
    print("--- session " .. i .. " ---"); print(text)
  end
  r.finish()
end, emu.eventType.endFrame)
