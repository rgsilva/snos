-- sa1probe under Mesen --testrunner: every check line must be OK, except the
-- ones Mesen is known not to model (EMU_EXPECTED, with the source location
-- that proves it). Then the three screen pages are read back out of VRAM,
-- paged with the A button, and compared with the transcript line for line.
-- First run of `make test`: no save file, so the boot counter must read 1.

local MB = 0xF000                 -- $7EF000 in snesWorkRam
local RING, RING_SIZE = MB + 0x010, 3552
local WRAM, VRAM = emu.memType.snesWorkRam, emu.memType.snesVideoRam

-- Lines Mesen cannot pass, and the exact values it must show instead.
local EMU_EXPECTED = {
  iramwp = { value = "rst=W b3=W",
             why = "no I-RAM write protection: Sa1.h 'TODO: Implement write " ..
                   "protection flags'; Sa1IRamHandler::Write ignores SIWP/CIWP" },
  div    = { value = "4/5 #09=DEBF",
             why = "Sa1::CalculateMathOpResult adds the divisor to a ZERO remainder " ..
                   "for a negative dividend: -25536/3 = -8513 r 3, not -8512 r 0" },
  bwprot = { value = "p0=W u0=W p1=W pc=W",
             why = "no BW-RAM write protection: CpuBwRamHandler::Write ignores " ..
                   "SBWE/CBWE/BWPA" },
}

local function u16(off) return emu.read16(off, WRAM) end

local function transcript()
  local w, out = u16(MB + 4), {}
  for i = 0, math.min(w, RING_SIZE) - 1 do
    out[#out + 1] = string.char(emu.read(RING + i, WRAM))
  end
  return table.concat(out)
end

local function lines_of(text)
  local t = {}
  for l in text:gmatch("([^\n]*)\n") do t[#t + 1] = l end
  return t
end

-- BG2 map at VRAM word 0: entry low byte = character code.
local function screen_rows()
  local rows = {}
  for r = 0, 27 do
    local s = {}
    for c = 0, 31 do
      s[#s + 1] = string.char(emu.read16((r * 32 + c) * 2, VRAM) & 0x7F)
    end
    rows[#rows + 1] = (table.concat(s):gsub("%s+$", ""))
  end
  return rows
end

local report = { failures = {} }
local function check(ok, msg) if not ok then report.failures[#report.failures + 1] = msg end end

local frame, phase, at = 0, "run", 0
local lines, pages, press = nil, {}, 0
local boots_expected = 1

local function expected_pages(all)
  local p, seg, verdict, fails = {}, nil, nil, nil
  for _, l in ipairs(all) do
    if l:find("^SA1PROBE") or l:find("^ROM MAP") or l:find("^SPEED") then
      seg = { l }; p[#p + 1] = seg
    elseif l:find("^verdict") then verdict = l
    elseif l:find("^fails:") then fails = l
    elseif not l:find("^chk ") and seg then
      seg[#seg + 1] = l
    end
  end
  for i, s in ipairs(p) do
    s[#s + 1] = verdict
    if i == 1 then s[#s + 1] = fails end
  end
  return p
end

local function compare_page(n, want)
  local got = {}
  for _, r in ipairs(screen_rows()) do if r ~= "" then got[#got + 1] = r end end
  check(#got == #want, string.format("page %d: %d rows on screen, %d in transcript",
                                     n, #got, #want))
  for i = 1, math.max(#got, #want) do
    if got[i] ~= want[i] then
      check(false, string.format("page %d row %d: screen '%s' transcript '%s'",
                                 n, i, tostring(got[i]), tostring(want[i])))
      break
    end
  end
end

local function judge(all)
  local seen = {}
  for _, l in ipairs(all) do
    local name, st, val = l:match("^(%S+)%s+(%u%u+)%s+(.*)$")
    if not name then name, st, val = l:match("^(%S+)%s+(%-%-)%s*(.*)$") end
    if name and name ~= "verdict" then
      seen[name] = val
      local e = EMU_EXPECTED[name]
      if e then
        check(st == "FAIL" and val == e.value, string.format(
          "%s: expected the emulator's '%s' (%s), got %s %s", name, e.value, e.why, st, val))
      elseif st == "FAIL" then
        check(false, "check failed: " .. l)
      end
    end
    local tag, m1, hi, lo = l:match("^chk (%S+) (%x+) (%x+)(%x%x%x%x)$")
    if tag then
      local p = tonumber(hi, 16) * 65536 + tonumber(lo, 16)
      check(p == tonumber(m1, 16) * 0x1234, "multiply result wrong: " .. l)
    end
  end
  for _, need in ipairs({ "iram", "start", "s>a", "a>s", "mul", "div", "sum", "bwram",
                          "bwwin", "bwboot", "00:8000", "C0:8000", "fxb=80", "sa1rom",
                          "cc8", "cc4", "ccrate", "a:S", "d:Bb", "a:F" }) do
    check(seen[need] ~= nil, "no line for " .. need)
  end
  local boots = seen.bwboot and seen.bwboot:match("boots=(%d+)")
  check(tonumber(boots) == boots_expected, string.format(
    "boot counter %s, want %d", tostring(boots), boots_expected))
  local nfail = 0
  for _ in pairs(EMU_EXPECTED) do nfail = nfail + 1 end
  local v = nil
  for _, l in ipairs(all) do if l:find("^verdict") then v = l end end
  check(v ~= nil and v:find(" " .. nfail .. " fail$") ~= nil,
        "verdict should count exactly the emulator's known failures: " .. tostring(v))
end

local function finish()
  print("--- transcript ---")
  for _, l in ipairs(lines) do print(l) end
  print("------------------")
  if #report.failures == 0 then
    print("RESULT PASS sa1probe")
    emu.stop(0)
  else
    for _, f in ipairs(report.failures) do print("  ! " .. f) end
    print("RESULT FAIL sa1probe")
    emu.stop(1)
  end
end

emu.addEventCallback(function()
  if press > 0 then
    emu.setInput({ a = true }, 0)
    press = press - 1
  end
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  if phase == "run" then
    if u16(MB) == 0xCAFE and u16(MB + 2) == 1 then
      lines = lines_of(transcript())
      pages = expected_pages(lines)
      judge(lines)
      check(#pages == 3, "expected 3 pages in the transcript, got " .. #pages)
      phase, at = "page1", frame + 10
    elseif frame > 3600 then
      lines = lines_of(transcript())
      check(false, "ROM never finished (mailbox status " .. u16(MB + 2) .. ")")
      finish()
    end
  elseif frame >= at then
    if phase == "page1" then
      compare_page(1, pages[1] or {})
      press, phase, at = 3, "page2", frame + 20
    elseif phase == "page2" then
      compare_page(2, pages[2] or {})
      -- the picture: BG1 tiles 1-32 at VRAM word $4020 are what the second
      -- conversion DMA'd; the first one, checked by the ROM, is at $7E4000
      local bad = 0
      for i = 0, 2047 do
        if emu.read(0x8040 + i, VRAM) ~= emu.read(0x4000 + i, WRAM) then bad = bad + 1 end
      end
      check(bad == 0, bad .. " picture bytes in VRAM differ from the verified conversion")
      press, phase, at = 3, "page3", frame + 20
    elseif phase == "page3" then
      compare_page(3, pages[3] or {})
      finish()
    end
  end
end, emu.eventType.endFrame)
