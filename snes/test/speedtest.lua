-- Receiver counts, not send-ring occupancy: a short fixed window on each
-- side, through real TCP. The finite download fits well inside the window,
-- which lets the peer independently know exactly how many bytes must arrive.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{addr = "10.0.2.99"}
local sockets, downloads, uploads = {}, {}, {}
local payload = string.rep("x", 1072)
local frame, started, completed = 0, false, nil
local run, seconds, output_start = 1, 2, 1
local target, port = "10.0.2.99", 7778
local cancelled = false
local overlap = false
local wrong_target = false
local r = L.new_report("speedtest")

gw.on_connect = function(t, dest_port)
  wrong_target = wrong_target or dest_port ~= port or t.peer_ip ~= target
  sockets[#sockets + 1] = t
end

local function service()
  local d_active, u_active = false, false
  for _, t in ipairs(sockets) do
    if (t.reset or t.done) and not t.stop then t.stop = frame end
    if not t.reset and not t.done then
      if not t.kind then
        local k, n, rest = t.rx:match("^([DU]) (%d+)\n(.*)$")
        if k then
          t.kind, t.frames, t.rx = k, tonumber(n), rest
          if run ~= 3 then t:send("OK\n") end
        end
      elseif not t.start then
        if t.rx:sub(1, 3) == "GO\n" then
          t.rx = t.rx:sub(4)
          t.start, t.count = frame, 0
          if t.kind == "D" then
            t:send(payload)
            downloads[#downloads + 1] = t
          else
            uploads[#uploads + 1] = t
          end
        end
      elseif t.kind == "U" and not t.reported then
        if frame - t.start >= t.frames then
          t.reported = true
          t:send(tostring(t.count) .. "\n")
        else
          r.check(t.rx:match("^x*$") ~= nil, "upload payload corrupt")
          t.count = t.count + #t.rx
          t.rx = ""
          u_active = true
        end
      elseif t.kind == "D" then
        d_active = #t.txq > 0 or #t.unacked > 0
      end
    end
  end
  if d_active and u_active then overlap = true end
end

local function verify(text)
  print("--- console ---"); print(text)
  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(text:find("[speedtest exited 0]", 1, true) ~= nil, "command failed or timed out")
  r.check(not wrong_target, "did not use HELLO address and default port")
  r.check(#downloads == 2 and #uploads == 2, "expected four directional connections")
  r.check(overlap, "duplex directions never overlapped")
  for _, t in ipairs(downloads) do
    local elapsed = (t.stop or frame) - t.start
    r.check(elapsed >= t.frames - 12 and elapsed <= t.frames + 40,
            "download did not keep its fixed window: " .. elapsed .. " frames")
  end
  local labels = {"download", "upload", "both download", "both upload"}
  local counts = {#payload, uploads[1] and uploads[1].count or 0,
                  #payload, uploads[2] and uploads[2].count or 0}
  for i, label in ipairs(labels) do
    local rate, count = text:match(label .. ": (%d+) B/s %((%d+) bytes%)")
    rate, count = tonumber(rate or 0), tonumber(count or 0)
    r.check(rate > 0 and rate < 50000, label .. " implausible rate " .. rate)
    r.check(count == counts[i], label .. " console/peer byte counts differ")
    r.check(rate == math.floor(count / seconds), label .. " wrong window duration")
  end
  r.check(text:match("frames %d+, bad sums %d+, resends %d+") ~= nil,
          "missing link counters")
  r.check(text:match("refused %d+, hello %d+, ip drop %d+, desync %d+") ~= nil,
          "missing loss counters")
  r.check(gw.tcp_bad_csum == 0, "bad TCP checksum")
  for _, t in ipairs(sockets) do
    r.check(t.reset or t.done, "connection leaked")
  end
end

local function launch(command)
  output_start = #L.ring_text() + 1
  sockets, downloads, uploads = {}, {}, {}
  overlap, completed = false, nil
  L.feed(command)
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then r.check(false, "no boot"); r.finish() end
    return
  end
  if frame < 200 then return end
  if frame == 200 then gw:claim() end
  gw:service()
  service()
  if frame >= 350 and not started then
    launch("net speedtest - 2\n")
    started = true
  end
  local text = L.ring_text():sub(output_start)
  if run == 3 and #sockets > 0 and sockets[1].kind and not cancelled then
    L.feed("q")
    cancelled = true
  end
  if text:find("refused ", 1, true) and not completed then completed = frame end
  if frame > 2600 then
    print(L.ring_text()); r.check(false, "test deadline"); r.finish(); return
  end
  if not completed or frame - completed < 60 then return end
  if run <= 2 then
    verify(text)
    run = run + 1
    if run == 2 then
      seconds, target, port = 1, "10.0.2.88", 8889
      payload = string.rep("x", 536)
    end
    launch("net speedtest 10.0.2.88:8889 1\n")
  else
    print("--- cancelled setup ---"); print(text)
    r.check(text:find("speedtest: cancelled", 1, true) ~= nil,
            "stalled peer was not cancellable")
    r.check(#sockets == 1 and sockets[1].reset, "cancelled connection leaked")
    r.check(not wrong_target, "host:port override ignored")
    r.finish()
  end
end, emu.eventType.endFrame)
