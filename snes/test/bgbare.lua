-- `NAME ... &` without `run`: a typed application name with a trailing `&`
-- goes to the background exactly as `run NAME ... &` does (bg.lua covers
-- that form end to end; this mirrors its core on the bare one).
--
--   netecho 7777 &   spawned in the background, not run in the foreground
--   jobs             sees it running
--   (the gwip peer dials the forward and the echo comes back)
--   httpd 8080 &     refused by name: it shares netecho's arena region
--   echo builtin &   builtins never take `&`: echo prints it
--   kill 1           reaps the process; its connection gets a RST
--   jobs             nothing left
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}

local echo_sock = nil

local frame, step, at = 0, 1, nil
local claimed = false
local first_fwd_port = nil
local echo_seen = false

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then
      print("RESULT FAIL bgbare - no mailbox magic"); emu.stop(1)
    end
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

  if not echo_seen and echo_sock and echo_sock.rx:find("hello%-bare") then
    echo_seen = true
  end

  if at and frame < at then return end

  if step == 1 then send("netecho 7777 &\n", 400); step = 2; return end
  if step == 2 then send("jobs\n", 200); step = 3; return end
  if step == 3 then
    -- The forward is the proof netecho is running at all; that the shell
    -- answered `jobs` above it is the proof it is not in the foreground.
    if not gw.fwd_port then
      print("--- mailbox at failure ---")
      print(L.ring_text())
      print("RESULT FAIL bgbare - netecho never asked for a port forward")
      emu.stop(1)
      return
    end
    first_fwd_port = gw.fwd_port
    echo_sock = gw:dial(gw.fwd_port)
    echo_sock.on_established = function(t)
      t:send("hello-bare\n")
      -- Left open, so that `kill` has a live connection to reap.
    end
    send("httpd 8080 &\n", 300)
    step = 4
    return
  end
  if step == 4 then send("echo builtin &\n", 200); step = 5; return end
  if step == 5 then send("kill 1\n", 200); step = 6; return end
  if step == 6 then send("jobs\n", 200); step = 7; return end

  local r = L.new_report("bgbare")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.tcp_bad_csum == 0,
          "the console sent " .. gw.tcp_bad_csum ..
          " TCP segments with a bad checksum")
  r.check(text:find("[1] netecho started in the background", 1, true) ~= nil,
          "the bare `netecho 7777 &` did not report a background spawn")
  r.check(text:find("[1] netecho  running", 1, true) ~= nil,
          "jobs did not show the bare-launched process running")
  r.check(first_fwd_port == 7777,
          "netecho forwarded " .. tostring(first_fwd_port) ..
          ", not 7777 - the `&` reached it as an argument?")
  r.check(echo_seen, "the background netecho did not echo through the peer")
  r.check(text:find("arena conflict: [1] netecho holds that memory", 1, true)
          ~= nil,
          "the bare `httpd 8080 &` was not refused by name")
  r.check(text:find("httpd started", 1, true) == nil and gw.fwd_port == 7777,
          "httpd started despite the arena conflict")
  r.check(text:find("\nbuiltin &\n", 1, true) ~= nil,
          "echo did not print its `&` - a builtin took it as background")
  r.check(text:find("[1] killed", 1, true) ~= nil, "kill did not report")
  r.check(echo_sock ~= nil and echo_sock.reset,
          "the killed process's connection was never torn down - no RST " ..
          "reached the peer")
  r.check(text:find("no background jobs", 1, true) ~= nil,
          "jobs still shows something after the kill")

  r.finish()
end, emu.eventType.endFrame)
