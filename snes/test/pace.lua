-- How fast does the console feel? Boot to prompt, `help` and `cat NET.TXT`
-- painted, key to echo, and how the console's flushes and the NMI share the
-- vblank while they run. Prints numbers for docs/sa1-notes.md and
-- docs/console-notes.md; the verdict only says the run completed.
--
-- On the LINK_HW ROM the adapter model (test/hwport.lua) answers on port 2
-- from power-on, as a plugged-in Pico does, so the NMI's cable exchange is
-- part of every frame; on the emulator ROM the link is idle. `make test-pace`
-- (any image); PACE="adapter=false" leaves port 2 empty.
--
-- Times are frames from power-on (Mesen's endFrame, the start of vblank).
-- "Painted" means on screen, read out of VRAM, with no row still dirty.
-- Row DMAs are the console's VRAM transfers ($420B writes after boot's font
-- upload): one per row at 32 columns, two at 64. One started at line 225 or
-- later landed; one started in active display painted nothing and its row
-- stays dirty for the next flush.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")
local P = dofile("test/hwport.lua")

local CONFIG = {}
-- @CONFIG@ (the Makefile inserts `CONFIG = {...}` here)
local A = CONFIG.addr
local adapter = CONFIG.linkhw and CONFIG.adapter ~= false
local gw = G.new{addr = "10.0.2.99", frame = 64, mss = 1460, rto_polls = 150}
local port = adapter and P.new(gw, {}) or { exchanges = 0 }
local r = L.new_report("pace")
local LINE = 1364

local function clock() return emu.getState()["masterClock"] end
local function scanline() return emu.getState()["ppu.scanline"] end

-- ------------------------------------------------------------- the NMI
-- pad_latch: from the NMI to its entry, and its own length. Entered after
-- the auto-joypad read has finished, its first spin burns its whole guard.
local nmi = { t = nil, n = 0, sum = 0, max = 0, over = 0, exch = 0, exch_dirty = 0 }
local pad = { n = 0, entry_max = 0, len_max = 0, len_sum = 0, t = nil }
emu.addMemoryCallback(function()
  if not nmi.t then return end
  pad.t = clock()
  local e = (pad.t - nmi.t) / LINE
  if e > pad.entry_max then pad.entry_max = e end
end, emu.callbackType.exec, A.pad_latch[1])
for i = 2, #A.pad_latch do
  emu.addMemoryCallback(function()
    if not pad.t then return end
    local d = (clock() - pad.t) / LINE
    pad.t = nil
    pad.n, pad.len_sum = pad.n + 1, pad.len_sum + d
    if d > pad.len_max then pad.len_max = d end
  end, emu.callbackType.exec, A.pad_latch[i])
end

-- From the NMI event to snos_nmi_c's rtl: the handler's length.
local function owes_vram()
  for i = 0, 31 do
    if emu.read(A.dirty[1] + i, emu.memType.snesMemory) ~= 0 then return true end
  end
  return emu.read(A.con_panel_dirty[1], emu.memType.snesMemory) ~= 0
end
local nmi_owed, nmi_exch0 = false, 0
emu.addEventCallback(function()
  nmi.t = clock()
  nmi_owed = owes_vram()
  nmi_exch0 = port.exchanges
end, emu.eventType.nmi)
for i = 2, #A.snos_nmi_c do
  emu.addMemoryCallback(function()
    if not nmi.t then return end
    local d = (clock() - nmi.t) / LINE
    nmi.t = nil
    nmi.n, nmi.sum = nmi.n + 1, nmi.sum + d
    if d > nmi.max then nmi.max = d end
    if d > 262 - 225 then nmi.over = nmi.over + 1 end
    if port.exchanges > nmi_exch0 then
      nmi.exch = nmi.exch + 1
      if nmi_owed then nmi.exch_dirty = nmi.exch_dirty + 1 end
    end
  end, emu.callbackType.exec, A.snos_nmi_c[i])
end

-- --------------------------------------------------------- row DMAs
local booted = false
local dma = { landed = 0, missed = 0, frames = 0, hist = {}, full = {}, first_line_sum = 0 }
local this_frame, this_first, this_owed = 0, nil, 0
local function dirty_rows()
  local n = 0
  for i = 0, 31 do
    if emu.read(A.dirty[1] + i, emu.memType.snesMemory) ~= 0 then n = n + 1 end
  end
  return n
end
emu.addMemoryCallback(function(addr, value)
  if not booted or value == 0 then return end
  local line = scanline()
  if line >= 225 then
    dma.landed = dma.landed + 1
    this_frame = this_frame + 1
    if not this_first then this_first, this_owed = line, dirty_rows() end
  else
    dma.missed = dma.missed + 1
  end
end, emu.callbackType.write, 0x420B)
local function dma_frame_end()
  if this_frame > 0 then
    dma.frames = dma.frames + 1
    dma.hist[this_frame] = (dma.hist[this_frame] or 0) + 1
    -- Full: rows were still owed when the vblank ran out.
    if this_owed * (L.con_cols() == 32 and 1 or 2) > this_frame then
      dma.full[this_frame] = (dma.full[this_frame] or 0) + 1
    end
    dma.first_line_sum = dma.first_line_sum + this_first
  end
  this_frame, this_first = 0, nil
end
local function dma_reset()
  dma = { landed = 0, missed = 0, frames = 0, hist = {}, full = {}, first_line_sum = 0 }
end
local function dma_line(tag)
  local h = {}
  local f = {}
  for k = 1, 32 do
    if dma.hist[k] then h[#h + 1] = k .. "x" .. dma.hist[k] end
    if dma.full[k] then f[#f + 1] = k .. "x" .. dma.full[k] end
  end
  print(string.format("pace: %s: %d row DMAs landed in %d vblanks (per vblank %s; where rows"
    .. " were still owed when it ended %s), first at line %.1f on average; %d started in active"
    .. " display and were lost", tag, dma.landed, dma.frames, table.concat(h, " "),
    #f > 0 and table.concat(f, " ") or "none",
    dma.frames > 0 and dma.first_line_sum / dma.frames or 0, dma.missed))
end

-- ------------------------------------------------------------ screen
local function clean() return not owes_vram() end
local function screen() return L.display_text(0, 27) end
local function last_row_text()
  local _, rows = L.con_state()
  for row = rows - 1, 0, -1 do
    local t = L.display_row(row):gsub("[\128-\255]", function(c)
      return string.char(c:byte() & 0x7F)
    end):gsub("%s+$", "")
    if t ~= "" then return t end
  end
  return ""
end

-- ------------------------------------------------------------- script
local frame, step, at, mark = 0, "boot", 0, 1
local result = {}
local samples, keys = {}, "abcdefghij"
local function since() return L.ring_text():sub(mark) end

emu.addEventCallback(function()
  frame = frame + 1
  dma_frame_end()
  if frame > 60 * 120 then
    print(L.ring_text()); r.check(false, "deadline in step " .. step); r.finish(); return
  end
  if L.magic() ~= 0xCAFE then return end
  if not booted then booted = true; result.mailbox = frame end
  if step == "boot" then
    if not result.text and screen():match("%S") then result.text = frame end
    if not result.prompt_mb and L.ring_text():find("> ", 1, true) then result.prompt_mb = frame end
    if last_row_text() == ">" and clean() then
      result.prompt = frame
      print(string.format("pace: boot: mailbox up at frame %d, first text on screen %d, prompt"
        .. " in the mailbox %d, prompt on screen %d", result.mailbox, result.text or -1,
        result.prompt_mb or -1, result.prompt))
      dma_line("boot")
      step, at = "settle", frame + (adapter and 240 or 60)
    end
  elseif step == "settle" then
    if frame < at or (adapter and not gw.hello_seen) then return end
    nmi = { t = nil, n = 0, sum = 0, max = 0, over = 0, exch = 0, exch_dirty = 0 }
    local e0 = port.exchanges
    step, at, result.e0 = "idle", frame + 120, e0
  elseif step == "idle" then
    if frame < at then return end
    print(string.format("pace: idle prompt, 120 frames: NMI mean %.1f lines, max %.1f, %d over"
      .. " 37; %.2f exchanges a frame", nmi.n > 0 and nmi.sum / nmi.n or 0, nmi.max, nmi.over,
      (port.exchanges - result.e0) / 120))
    print(string.format("pace: pad_latch, %d calls since boot: entered at most %.2f lines after"
      .. " the NMI, %.2f lines long on average, %.2f at most", pad.n, pad.entry_max,
      pad.n > 0 and pad.len_sum / pad.n or 0, pad.len_max))
    nmi = { t = nil, n = 0, sum = 0, max = 0, over = 0, exch = 0, exch_dirty = 0 }
    dma_reset()
    mark = #L.ring_text() + 1
    L.feed("help\n")
    step, at = "help", frame
  elseif step == "help" or step == "cat" then
    -- The pager stops a long file every screenful; any key goes on, once
    -- the page is painted.
    local _, pages = since():gsub("%(q quits%)", "")
    if pages > (result.pages or 0) and clean() then
      result.pages = pages
      L.feed(" ")
    end
    local done = since():find("\n> ", 1, true)
    if done and not result[step .. "_mb"] then result[step .. "_mb"] = frame - at end
    if done and clean() and last_row_text() == ">" then
      local want = step == "help" and "debug       diagnostics" or nil
      if not want or screen():find(want, 1, true) then
        print(string.format("pace: %s: in the mailbox after %d frames, painted after %d"
          .. (step == "cat" and string.format(" (%d pages)", result.pages) or "") .. ";"
          .. " NMI mean %.1f lines, max %.1f, %d with an exchange (%d while the console owed"
          .. " VRAM)", step, result[step .. "_mb"], frame - at, nmi.n > 0 and nmi.sum / nmi.n or 0,
          nmi.max, nmi.exch, nmi.exch_dirty))
        dma_line(step)
        if step == "help" then
          nmi = { t = nil, n = 0, sum = 0, max = 0, over = 0, exch = 0, exch_dirty = 0 }
          dma_reset()
          mark = #L.ring_text() + 1
          L.feed("cat NET.TXT\n")
          step, at, result.pages = "cat", frame, 0
        else
          step, at = "keyfeed", frame + 30
        end
      end
    end
  elseif step == "keyfeed" or step == "keyadapter" then
    -- Ten keys, each typed a different number of frames after the last
    -- echo so they fall on different phases of the frame, then erased.
    local n = #samples + 1
    if not result.pending then
      if frame < at then return end
      local k = keys:sub(n, n)
      if step == "keyfeed" then L.feed(k) else gw.push(0x11, k) end
      result.pending, result.typed = k, frame
      return
    end
    if last_row_text():sub(-1) == result.pending and clean() then
      samples[n] = frame - result.typed
      result.pending = nil
      at = frame + 7 + (n * 3) % 11
      if n == #keys then
        local s, mx = 0, 0
        for _, v in ipairs(samples) do s = s + v; if v > mx then mx = v end end
        print(string.format("pace: key to echo (%s): mean %.1f frames, max %d (%s)",
          step == "keyfeed" and "mailbox stdin" or "adapter keyboard", s / #samples, mx,
          table.concat(samples, " ")))
        samples = {}
        L.feed(string.rep("\b", #keys))
        if step == "keyfeed" and adapter and CONFIG.linkhw then
          step, at = "keyadapter", frame + 30
        else
          step, at = "done", frame + 30
        end
      end
    elseif frame - result.typed > 300 then
      r.check(false, "key " .. result.pending .. " never echoed"); r.finish()
    end
  elseif step == "done" then
    if frame < at then return end
    r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
    r.finish()
  end
end, emu.eventType.endFrame)
