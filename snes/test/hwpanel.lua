-- Does the keyboard panel paint while the cable is busy? The LINK_HW ROM
-- through the port-2 adapter model (test/hwport.lua), where the link is up
-- and the NMI clocks an exchange through every vblank, as on the hardware
-- with the adapter plugged in:
--
--   1. SELECT during a `net speedtest` download opens the keyboard, and all
--      six panel rows reach VRAM -- frame, keys, the selected key;
--   2. RIGHT moves the selection, and the moved highlight reaches VRAM;
--   3. the speedtest still finishes all three phases with the panel open.
--
-- The panel used to starve here. Its row DMA waits for vblank, but the NMI
-- spent every vblank on the link unless the TEXT console owed VRAM, so the
-- DMA landed in active display, was ignored and was retried, forever. The
-- emulator ROM's mailbox link never showed it, and nor did the hardware
-- without an adapter, which is probed once a second.
-- `make LINK_HW=1 test-hwpanel`; at either width (CON_COLS).
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")
local P = dofile("test/hwport.lua")

local gw = G.new{addr = "10.0.2.99", frame = 64, mss = 1460, rto_polls = 150}
local port = P.new(gw, {})
local r = L.new_report("hwpanel")

-- Six frames on a sixty-frame clock: one row costs two (the deferred NMI's
-- vblank, then the DMA's), so this is generous, and a starved panel never
-- gets there at all.
local PAINT_FRAMES = 60

local sockets = {}
gw.on_connect = function(t) sockets[#sockets + 1] = t end
local frame = 0
local function serve()
  for _, t in ipairs(sockets) do
    if not t.reset and not t.done then
      if not t.kind then
        local k, n, rest = t.rx:match("^([DU]) (%d+)\n(.*)$")
        if k then t.kind, t.frames, t.rx = k, tonumber(n), rest; t:send("OK\n") end
      elseif not t.start then
        if t.rx:sub(1, 3) == "GO\n" then t.rx = t.rx:sub(4); t.start, t.count = frame, 0 end
      elseif t.kind == "D" then
        if not t.closing and #t.txq < 4096 then t:send(string.rep("x", 4096)) end
      elseif not t.reported then
        if frame - t.start >= t.frames then
          t.reported = true
          t:send(tostring(t.count) .. "\n")
        else
          t.count, t.rx = t.count + #t.rx, ""
        end
      end
    end
  end
end

local function downloading()
  for _, t in ipairs(sockets) do
    if t.kind == "D" and t.start and not t.done and not t.reset then return true end
  end
  return false
end

-- Every row as VRAM has it: the frame's corners and sides, the key grid.
local function panel_painted()
  local rows = {}
  for i = 0, L.PANEL_ROWS - 1 do rows[i] = L.panel_row(L.PANEL_TOP + i) end
  if rows[0]:byte(1) ~= 3 or rows[L.PANEL_ROWS - 1]:byte(1) ~= 5 then return false end
  for i = 1, L.PANEL_ROWS - 2 do
    if rows[i]:byte(1) ~= 2 then return false end
  end
  return L.panel_text_plain():find("q w e r t y u i o p", 1, true) ~= nil
end

-- The highlighted key, as VRAM has it, or nil.
local function selected()
  for i = 0, L.PANEL_ROWS - 1 do
    for _, b in ipairs({ L.panel_row(L.PANEL_TOP + i):byte(1, -1) }) do
      if b >= 0x80 then return string.char(b & 0x7F) end
    end
  end
  return nil
end

local press, press_from = nil, nil
emu.addEventCallback(function()
  if press and frame >= press_from and frame < press_from + 4 then
    emu.setInput({ [press] = true }, 0)
  end
end, emu.eventType.inputPolled)
local function tap(button)
  press, press_from = button, frame + 1
end

local step, at, mark = 1, 0, 1
local function since() return L.ring_text():sub(mark) end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then r.check(false, "no boot"); r.finish() end
    return
  end
  serve()
  if frame > 6000 then
    print(L.ring_text()); r.check(false, "test deadline at step " .. step); r.finish(); return
  end
  if step == 1 then
    if not gw.hello_seen or frame < 300 then return end
    r.check(selected() == nil and not panel_painted(), "the panel was in VRAM before SELECT")
    mark = #L.ring_text() + 1
    L.feed("net speedtest - 4\n")
    step = 2
  elseif step == 2 then
    -- A second into the download, with the cable running flat out.
    if not downloading() then return end
    if at == 0 then at = frame + 60 end
    if frame < at then return end
    tap("select")
    at, step = frame, 3
  elseif step == 3 then
    if panel_painted() and selected() == "q" then
      local took = frame - at
      print("hwpanel: SELECT to all six panel rows in VRAM: " .. took .. " frames")
      r.check(took <= PAINT_FRAMES, "the panel took " .. took .. " frames to paint")
      tap("right")
      at, step = frame, 4
    elseif frame - at > 300 then
      print("--- panel ---"); print(L.panel_text_plain())
      r.check(false, "the keyboard panel never painted while the link was busy")
      r.finish()
    end
  elseif step == 4 then
    if selected() == "w" then
      local took = frame - at
      print("hwpanel: RIGHT to the moved highlight in VRAM: " .. took .. " frames")
      r.check(took <= PAINT_FRAMES, "the highlight took " .. took .. " frames to move")
      step = 5
    elseif frame - at > 300 then
      r.check(false, "the moved highlight never reached VRAM (selected: " ..
        tostring(selected()) .. ")")
      step = 5
    end
  elseif step == 5 then
    local text = since()
    if not text:find("refused ", 1, true) then return end
    print("--- console ---"); print(L.ring_text())
    for _, label in ipairs({"download", "upload", "both download", "both upload"}) do
      r.check(text:match(label .. ": (%d+) B/s") ~= nil, "no " .. label .. " result")
    end
    r.check(text:match("bad sums (%d+)") == "0", "checksum failures on the modelled wire")
    r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
    r.check(panel_painted(), "the panel did not survive the speedtest")
    r.check(port.collisions == 0, port.collisions .. " exchanges were still running at vblank")
    port.mask_report(r, "hwpanel", true)
    r.finish()
  end
end, emu.eventType.endFrame)
