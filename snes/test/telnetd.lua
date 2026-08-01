-- telnetd v2 -- remoting into a Super Nintendo.
--
-- Drives a REAL telnet-shaped TCP connection into a background telnetd --
-- through the console's own stack since the uIP cutover: the hermetic slirp
-- box (test/gwip.lua) records the port forward telnetd asks for, dials it,
-- and the bytes both ways ride TCP segments the console composes and parses
-- itself. Asserts the three things that make it a mirror rather than a log:
--   1. on connect it clears the far terminal and paints the CURRENT screen
--      (the boot banner is already scrolled in -- a stream tee could never
--      produce it, only a state diff can);
--   2. keystrokes sent down the socket reach the shell, and the result
--      appears BOTH in the mirror and on the real console;
--   3. all of it while a foreground application runs.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}
local sock = nil

-- -------------------------------------------------------------------- test
local frame, step, at = 0, 1, nil
local claimed = false
local mirror_during_fg = false
local idle_mark, idle_grew = nil, nil

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

local function mirrored() return sock and sock.rx or "" end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL telnetd - no mailbox magic"); emu.stop(1) end
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

  -- The mirror must keep working while a foreground app owns the console.
  if step >= 6 and step < 8 and not mirror_during_fg then
    if mirrored():find("hello from a SNOS app", 1, true) then mirror_during_fg = true end
  end

  if at and frame < at then return end

  if step == 1 then send("run telnetd 2323 &\n", 400); step = 2; return end
  if step == 2 then
    if not gw.fwd_port then
      print("--- mailbox ---"); print(L.ring_text())
      print("RESULT FAIL telnetd - never asked for its port forward"); emu.stop(1); return
    end
    -- The telnet client arrives: a SYN into the forwarded port, answered by
    -- the console's own listener. The connection stays open for the whole
    -- test -- a mirror is continuous output, not request/response.
    sock = gw:dial(gw.fwd_port)
    at = frame + 500          -- let the first full paint cross the link
    step = 2.4
    return
  end
  -- An IDLE screen must be SILENT. SYS_CONDIRTY is read-and-clear; if the
  -- clear half breaks, telnetd re-sends unchanged rows forever and the only
  -- symptom is wasted link -- invisible to every content assertion below.
  -- Nothing types and nothing prints during this window, so the byte count
  -- must not move at all.
  if step == 2.4 then
    idle_mark = #mirrored()
    at = frame + 240
    step = 2.6
    return
  end
  if step == 2.6 then
    idle_grew = #mirrored() - idle_mark
    at = frame + 1
    step = 3
    return
  end
  if step == 3 then
    -- Type into the SOCKET, as a telnet client would (CR LF for Enter).
    sock:send("debug ver\r\n")
    at = frame + 500
    step = 4
    return
  end
  if step == 4 then send("run hello motd.txt\n", 500); step = 6; return end
  if step == 6 then at = frame + 300; step = 8; return end
  if step == 8 then send("kill 1\n", 200); step = 9; return end
  if step == 9 then send("debug ver\n", 200); step = 10; return end

  local r = L.new_report("telnetd")
  local text = L.ring_text()
  local up = mirrored()
  print("--- console ---"); print(text)
  print("--- mirrored (first 400) ---"); print(up:sub(1, 400)); print("---")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.fwd_port == 2323,
          "telnetd forwarded " .. tostring(gw.fwd_port) .. ", not 2323")
  r.check(gw.tcp_bad_csum == 0,
          "the console sent " .. gw.tcp_bad_csum ..
          " TCP segments with a bad checksum")
  -- 1. the far terminal was cleared and given the screen as it STANDS
  r.check(up:find("\27[2J", 1, true) ~= nil, "no clear-screen on connect")
  r.check(up:find("\27[", 1, true) ~= nil, "no ANSI positioning at all")
  r.check(up:find("type 'help' for commands", 1, true) ~= nil,
          "the mirror did not carry the CURRENT screen (banner missing) - " ..
          "a state diff must reproduce text printed before the connection")
  -- 2. keystrokes from the socket reached the shell, and the answer came back
  r.check(text:find("kernel ABI ", 1, true) ~= nil,
          "the socket's keystrokes never reached the shell")
  r.check(up:find("kernel ABI ", 1, true) ~= nil,
          "the command's output never reached the remote terminal")
  -- 3. and it all kept working under a foreground application
  r.check(mirror_during_fg,
          "the mirror stopped while a foreground app ran")
  -- 4. a static screen costs nothing on the wire
  r.check(idle_grew == 0,
          "an idle screen sent " .. tostring(idle_grew) ..
          " bytes - the change mask is not clearing, so unchanged rows " ..
          "are being re-diffed and re-sent")
  -- 5. the kill released the forward: an app killed outright never gets to
  -- say IPUNFWD itself, so the kernel's reap must -- otherwise the daemon's
  -- socket outlives the app and the next run finds its port taken.
  r.check(gw.unfwds >= 1,
          "killing telnetd never withdrew the port forward")

  r.finish()
end, emu.eventType.endFrame)
