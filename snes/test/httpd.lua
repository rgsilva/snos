-- httpd, tested against a Lua client speaking real TCP to the console's own
-- stack (the uIP cutover: there is no stream gateway to ride any more).
--
-- The hermetic slirp box (test/gwip.lua) answers the console's HELLO with
-- version 2, records the port forward httpd asks for, and then DIALS the
-- console four times — one connection per branch of the server: a 200 with
-- the right Content-Type and the file's actual bytes, a 404, a 400 for a
-- path that tries to leave the flat volume, and a 405 for a method it does
-- not serve. Each connection ends with the server's own hangup — uIP's FIN
-- now, not a control record — which is what returns the channel to LISTEN
-- and lets the next one in.
--
-- Assertions are anchored on the responses the "client" actually received
-- (sock.rx at the server's FIN), not on the console transcript: the console
-- says what the server THINKS it did, the byte stream is what a browser
-- would have gotten.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}

local responses = {}
local sock = nil

local function connect_and_request(req)
  sock = gw:dial(gw.fwd_port)
  sock.on_established = function(t)
    t:send(req)
  end
  sock.on_fin = function(t)
    -- The server hung up: what we hold at this moment IS the response,
    -- whole — ip_hangup refuses to FIN while bytes are unacknowledged.
    responses[#responses + 1] = t.rx
    t:close()
  end
end

-- ------------------------------------------------------------------ test
local frame, step, at = 0, 1, nil
local deadline_guard = 24000
local claimed = false

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL httpd - no mailbox magic"); emu.stop(1) end
    return
  end

  if not claimed then
    if frame < 200 then return end
    gw:claim()
    claimed = true
    at = frame + 60
    return
  end

  gw:service()
  if at and frame < at then return end

  if frame > deadline_guard then
    print("RESULT FAIL httpd - wedged at step " .. tostring(step) ..
          " served=" .. #responses)
    emu.stop(1)
    return
  end

  if step == 1 then
    L.feed("httpd 8081\n")
    at = frame + 240
    step = 2
    return
  end

  -- Wait for the port forward, then run the four connections, each gated on
  -- the server finishing the one before — which is also what proves the
  -- server came back to accept again rather than serving once and wedging.
  if step == 2 then
    if gw.fwd_port then
      connect_and_request("GET /MOTD.TXT HTTP/1.1\r\nHost: snes\r\n\r\n")
      step = 3
    end
    at = frame + 30
    return
  end

  if step == 3 then
    if #responses >= 1 then
      connect_and_request("GET /NOPE.TXT HTTP/1.1\r\n\r\n")
      step = 4
    end
    at = frame + 30
    return
  end

  if step == 4 then
    if #responses >= 2 then
      connect_and_request("GET /../ETC HTTP/1.1\r\n\r\n")
      step = 5
    end
    at = frame + 30
    return
  end

  if step == 5 then
    if #responses >= 3 then
      connect_and_request("POST / HTTP/1.1\r\n\r\n")
      step = 6
    end
    at = frame + 30
    return
  end

  if step == 6 then
    if #responses >= 4 then
      L.feed("q")            -- any key stops the server
      at = frame + 240
      step = 7
    else
      at = frame + 30
    end
    return
  end

  local r = L.new_report("httpd")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  for i, resp in ipairs(responses) do
    print(string.format("--- response %d (%d bytes) ---", i, #resp))
    print(resp:sub(1, 200))
  end

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.fwd_port == 8081,
          "the port from the command line did not reach the forward " ..
          "(got " .. tostring(gw.fwd_port) .. ")")
  r.check(gw.tcp_bad_csum == 0,
          "the console sent " .. gw.tcp_bad_csum ..
          " TCP segments with a bad checksum")

  -- Four connections, four responses: the count is the non-vacuous part — a
  -- server that wedged after one request would leave the later responses
  -- missing, not merely wrong.
  r.check(#responses == 4,
          "expected 4 served connections, got " .. #responses)

  local ok = responses[1] or ""
  r.check(ok:find("HTTP/1.1 200 OK", 1, true) ~= nil,
          "MOTD.TXT did not come back 200")
  r.check(ok:find("Content%-Type: text/plain") ~= nil,
          "a .TXT file was not served as text/plain")
  r.check(ok:find("Performance does not matter", 1, true) ~= nil,
          "the response body is not the file's bytes")

  r.check((responses[2] or ""):find("404", 1, true) ~= nil,
          "a missing file did not come back 404")
  r.check((responses[3] or ""):find("400", 1, true) ~= nil,
          "a path escape attempt did not come back 400")
  r.check((responses[4] or ""):find("405", 1, true) ~= nil,
          "a non-GET did not come back 405")

  -- The forward must be withdrawn when the server stops: the daemon's
  -- socket outlives the app otherwise, and the next run finds its own port
  -- taken by its previous life.
  r.check(gw.unfwds >= 1, "stopping httpd never withdrew the port forward")

  r.finish()
end, emu.eventType.endFrame)
