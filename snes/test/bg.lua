-- M10 step 5: the milestone. A background process stays CONNECTED and
-- WORKING while a foreground application runs.
--
--   run netecho 7777 &     spawn a server as a background process
--   (the hermetic slirp box dials its forwarded port and streams bytes)
--   run hello motd.txt     a foreground app runs meanwhile
--   -> the echo comes back DURING the foreground run: two applications,
--      one stock SNES, no SA-1.
--
-- Also asserted, because the safety story is the point:
--   - `jobs` sees the process; the shell prompt stays alive throughout;
--   - a foreground app whose arena overlaps the background one (`get` shares
--     region C with netecho) is REFUSED by name at launch — deterministic
--     refusal, never runtime corruption;
--   - `kill` reaps the process AND its connection: since the uIP cutover the
--     daemon has no stream channel to be told about, so the evidence is the
--     RST the console's stack emits when ip_close aborts the orphaned
--     connection — a real TCP goodbye where C2S_CLOSE used to be;
--   - and `jobs` reports the corpse exactly once.
--
-- The peer is test/gwip.lua — the shared hermetic slirp box, speaking the
-- REAL link protocol frame for frame and real TCP above it, checksums
-- verified in Lua so the console's stack cannot grade its own homework.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local WRAM = emu.memType.snesWorkRam
local gw = G.new{}

-- The two connections this test opens into the console: netecho's echo
-- stream (kept open so `kill` has something to tear down) and httpd's
-- request at the end.
local echo_sock = nil
local httpd_sock = nil

-- -------------------------------------------------------------------- test
local frame, step, at = 0, 1, nil
local claimed = false
local echo_seen_during_fg = false
local notify_during_fg = false
local first_fwd_port = nil

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL bg - no mailbox magic"); emu.stop(1) end
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

  -- The moment worth the whole milestone: the echo arriving WHILE the
  -- foreground app runs (steps 4..5 are hello's window). The bytes ride the
  -- console's own TCP now, but the claim is unchanged — a background process
  -- served a connection while another app owned the screen.
  if step >= 4 and step < 6 then
    if not echo_seen_during_fg and echo_sock and
       echo_sock.rx:find("hello%-bg") then
      echo_seen_during_fg = true
    end
    -- The attention cell: netecho (backgrounded) got a connection and asked
    -- for eyes; the kernel's overlay must be visible in the top-right cell
    -- WHILE the foreground app owns the rest of the screen.
    if not notify_during_fg and L.display_text(0, 0):sub(64, 64) == "!" then
      notify_during_fg = true
    end
  end

  if at and frame < at then return end

  if step == 1 then send("run netecho 7777 &\n", 400); step = 2; return end
  if step == 2 then send("jobs\n", 200); step = 3; return end
  if step == 3 then
    -- The server holds a port forward. Dial it and stream at it, then
    -- IMMEDIATELY start a foreground app: the handshake and the echo have to
    -- complete while hello owns the console.
    if not gw.fwd_port then
      print("--- mailbox at failure ---")
      print(L.ring_text())
      print("RESULT FAIL bg - netecho never asked for a port forward")
      emu.stop(1)
      return
    end
    first_fwd_port = gw.fwd_port
    echo_sock = gw:dial(gw.fwd_port)
    echo_sock.on_established = function(t)
      t:send("hello-bg\n")
      -- No close: the connection stays open on purpose, so that `kill`
      -- later has a live connection to reap.
    end
    send("run hello motd.txt\n", 300)
    step = 4
    return
  end
  if step == 4 then at = frame + 200; step = 5; return end -- let the echo land
  if step == 5 then send("run get nothing -\n", 250); step = 6; return end
  if step == 6 then send("kill 1\n", 200); step = 7; return end
  if step == 7 then send("jobs\n", 200); step = 7.2; return end
  -- fg: respawn, bring it to the console, then the keyboard break kills the
  -- FOREGROUNDED job and hands the shell back -- with the shell asleep in
  -- cmd_fg's wait the whole time.
  if step == 7.2 then send("run netecho 7777 &\n", 300); step = 7.4; return end
  if step == 7.4 then send("fg 1\n", 150); step = 7.6; return end
  if step == 7.6 then
    emu.write16(L.MB + 0x00E, 1, WRAM) -- what F12 writes
    at = frame + 250
    step = 8
    return
  end
  -- httpd in the BACKGROUND serving a file while the foreground shell reads
  -- the SAME file: two concurrent read handles, the per-process fs proof.
  if step == 8 then send("run httpd 8080 &\n", 300); step = 8.2; return end
  if step == 8.2 then
    if gw.fwd_port ~= 8080 then
      print("RESULT FAIL bg - httpd never asked for its port forward (got " ..
            tostring(gw.fwd_port) .. ")")
      emu.stop(1)
      return
    end
    httpd_sock = gw:dial(gw.fwd_port)
    httpd_sock.on_established = function(t)
      t:send("GET /MOTD.TXT HTTP/1.1\r\n\r\n")
    end
    httpd_sock.on_fin = function(t)
      t:close()   -- the server hung up; finish our side politely
    end
    send("cat motd.txt\n", 500)
    step = 8.4
    return
  end
  if step == 8.4 then send("kill 1\n", 200); step = 8.6; return end
  if step == 8.6 then send("debug ver\n", 200); step = 9; return end

  local r = L.new_report("bg")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.tcp_bad_csum == 0,
          "the console sent " .. gw.tcp_bad_csum ..
          " TCP segments with a bad checksum")
  r.check(text:find("] netecho started in the background", 1, true) ~= nil,
          "the background spawn did not report")
  r.check(text:find("netecho  running", 1, true) ~= nil,
          "jobs did not show the process running")
  r.check(first_fwd_port == 7777,
          "netecho forwarded " .. tostring(first_fwd_port) .. ", not 7777")
  r.check(text:find("[hello exited 42]", 1, true) ~= nil,
          "the foreground app did not run to completion")
  r.check(echo_seen_during_fg,
          "the echo did not arrive while the foreground app was running - " ..
          "background execution is not real")
  r.check(notify_during_fg,
          "the attention cell never showed while the foreground app ran")
  r.check(L.display_text(0, 0):sub(64, 64) ~= "!",
          "the attention cell survived its raiser being killed")
  r.check(text:find("arena conflict", 1, true) ~= nil and
          text:find("netecho holds that memory", 1, true) ~= nil,
          "the overlapping foreground launch was not refused by name")
  r.check(text:find("[1] killed", 1, true) ~= nil, "kill did not report")
  -- The killed process's connection: the reap runs ip_close, the orphaned
  -- uIP connection is aborted, and the abort reaches this peer as a RST.
  -- That RST is the cutover's equivalent of "the gateway saw the close".
  r.check(echo_sock ~= nil and echo_sock.reset,
          "the killed process's connection was never torn down - no RST " ..
          "reached the peer")
  r.check(text:find("no background jobs", 1, true) ~= nil,
          "jobs still shows something after the kill")
  r.check(text:find("] netecho killed", 1, true) ~= nil,
          "the keyboard break did not kill the foregrounded job")
  r.check(text:find("kernel ABI ", 1, true) ~= nil,
          "the shell did not answer at the end")

  -- The background httpd's response, received at this peer: status line and
  -- the file's actual bytes -- while the foreground cat of the SAME file
  -- also succeeded (its content is in the console transcript).
  do
    local resp = httpd_sock and httpd_sock.rx or ""
    r.check(resp:find("HTTP/1.1 200 OK", 1, true) ~= nil,
            "background httpd did not serve a 200")
    r.check(resp:find("Performance does not matter", 1, true) ~= nil,
            "background httpd's body is not the file's bytes")
  end
  do
    -- Exactly one console copy expected: the foreground cat's. httpd's copy
    -- goes to the socket, never to the screen.
    local _, cats = text:gsub("Performance does not matter", "")
    r.check(cats == 1,
            "expected exactly the foreground cat copy on console (saw " ..
            tostring(cats) .. " copies)")
  end

  r.finish()
end, emu.eventType.endFrame)
