-- Is the console still answering while the cable runs flat out? The LINK_HW
-- ROM through the port-2 adapter model (test/hwport.lua), with the link at its
-- defaults -- window 4, two exchanges a frame -- and `net speedtest`
-- downloading as fast as it can:
--
--   1. a key typed on the adapter's keyboard (a LINK_CH_KBD record, as
--      pico/kbd.c sends them) cancels the test -- the keyboard reaches an
--      application through the same busy link it is competing with;
--   2. START+SELECT on pad 1 kills it -- the kill switch lives in the NMI,
--      which the foreground exchanges must never hold off;
--   3. afterwards the link still answers (`net status` gets a HELLO back).
--
-- Each is timed in frames from the input to the console's reaction and the
-- verdict has a bound. The keyboard's is 40 frames: a key queues behind
-- whatever is left of the one datagram the peer has on the link, up to a
-- whole 1500-byte segment -- 26 frames on the wire, ~18 video frames at two
-- exchanges a frame (it was ~52 at stop-and-wait). The kill switch's is
-- SNOS_KILL_FRAMES (45) plus 30. `make LINK_HW=1 test-hwresponse`.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")
local P = dofile("test/hwport.lua")

local gw = G.new{addr = "10.0.2.99", frame = 64, mss = 1460, rto_polls = 150}
local port = P.new(gw, {})
local r = L.new_report("hwresponse")

local sockets = {}
gw.on_connect = function(t) sockets[#sockets + 1] = t end
local function serve()
  for _, t in ipairs(sockets) do
    if not t.reset and not t.done then
      if not t.kind then
        local k, n, rest = t.rx:match("^([DU]) (%d+)\n(.*)$")
        if k then t.kind, t.rx = k, rest; t:send("OK\n") end
      elseif not t.start then
        if t.rx:sub(1, 3) == "GO\n" then t.rx = t.rx:sub(4); t.start = true end
      elseif t.kind == "D" and not t.closing and #t.txq < 4096 then
        t:send(string.rep("x", 4096))
      end
    end
  end
end

local frame, step, at, mark, kill_from = 0, 1, 0, 1, nil
local function since() return L.ring_text():sub(mark) end
local function downloading()
  for _, t in ipairs(sockets) do
    if t.kind == "D" and t.start and not t.done and not t.reset then return true end
  end
  return false
end

emu.addEventCallback(function()
  if kill_from and frame >= kill_from and frame < kill_from + 120 then
    emu.setInput({ start = true, select = true }, 0)
  end
end, emu.eventType.inputPolled)

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
    mark = #L.ring_text() + 1
    sockets = {}
    L.feed("net speedtest - 30\n")
    step = 2
  elseif step == 2 then
    -- Two seconds into the download, type a key on the adapter's keyboard.
    if not downloading() then return end
    if at == 0 then at = frame + 120 end
    if frame < at then return end
    gw.push(0x11, "q")
    at = frame
    step = 3
  elseif step == 3 then
    if since():find("speedtest: cancelled", 1, true) then
      local took = frame - at
      print("hwresponse: a key from the adapter cancelled the download in " .. took .. " frames")
      r.check(took <= 40, "keyboard took " .. took .. " frames to reach the application")
      step, at = 4, frame + 120
    elseif frame - at > 300 then
      r.check(false, "the key never cancelled the download")
      step, at = 4, frame + 120
    end
  elseif step == 4 then
    if frame < at then return end
    mark = #L.ring_text() + 1
    sockets = {}
    L.feed("net speedtest - 30\n")
    at, step = 0, 5
  elseif step == 5 then
    if not downloading() then return end
    if at == 0 then at = frame + 120 end
    if frame < at then return end
    kill_from = frame
    step = 6
  elseif step == 6 then
    local text = since()
    if text:find("speedtest exited", 1, true) or text:find("killed", 1, true) then
      local took = frame - kill_from
      print("hwresponse: START+SELECT killed the download in " .. took .. " frames")
      r.check(took <= 45 + 30, "kill switch took " .. took .. " frames")
      step, at = 7, frame + 120
    elseif frame - kill_from > 400 then
      print(text)
      r.check(false, "START+SELECT never killed the download")
      step, at = 7, frame + 120
    end
  elseif step == 7 then
    if frame < at then return end
    mark = #L.ring_text() + 1
    L.feed("net status\n")
    at, step = frame, 8
  elseif step == 8 then
    if since():find("gateway alive", 1, true) and since():find("\n> ", 1, true) then
      print("--- console ---"); print(L.ring_text())
      r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
      r.check(port.collisions == 0, port.collisions .. " exchanges were still running at vblank")
      r.finish()
    elseif frame - at > 600 then
      print(L.ring_text()); r.check(false, "no net status after the kill"); r.finish()
    end
  end
end, emu.eventType.endFrame)
