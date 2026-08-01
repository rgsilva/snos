-- The network workhorse test, post-cutover: `get` is a real HTTP client on
-- the console's own TCP stack, and the gateway is a slirp box.
--
-- The hermetic peer (test/gwip.lua) speaks the real link protocol frame for
-- frame — same sequence numbers, same checksum, same stop-and-wait rules the
-- cable will use — and above it a real TCP/DNS/ICMP implementation whose
-- checksums are computed AND VERIFIED in Lua, so the console's stack and
-- this test cannot share a bug. On top of that this file runs a tiny HTTP
-- server, because that is what the far end of `get` is now: a website,
-- not a protocol brain.
--
-- It also does what no real cable can be asked to do on demand: drop a
-- frame and corrupt a checksum, deterministically, at a chosen moment. The
-- link's own recovery (retransmit, refuse, resync) sits UNDER the TCP now,
-- and a body that survives injected loss byte-perfect exercises both layers
-- at once.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local WRAM = emu.memType.snesWorkRam

local gw = G.new{
  dns = { ["snos.test"] = { 10, 0, 2, 80 } },
}

-- ------------------------------------------------------------ HTTP server
-- Bodies. The OK body's distinctive line is counted across fetches to prove
-- byte-perfect arrival; the big one exists to still be in flight when the
-- test aborts or kills mid-transfer (at TCP-over-link speed, ~75 B/frame,
-- 20 KB is over three hundred frames of transfer).
local BODY_OK = table.concat({
  "SNOS fetched this over its own TCP.",
  "Every header under this page was composed on a 65816:",
  "the DNS query, the SYN, the request, the acks.",
  "The daemon routed packets and understood none of them.",
}, "\n") .. "\n"
local BODY_404 = "not found\n"
local BODY_BIG = string.rep("the quick brown fox jumps over the lazy dog 0123\n", 400)

-- A canned .IMG: two tiles by two, with a palette and tile bytes chosen so a
-- wrong offset or a transposed copy cannot accidentally look right.
--   palette entry i = i          -> CGRAM byte 2i is i, 2i+1 is 0
--   tile byte j     = (j*7) & FF -> VRAM byte j is predictable
-- No conversion happens anywhere any more: the server serves these bytes and
-- the console stores them verbatim, which is the M12 shape (a render service
-- as an ordinary website) in miniature.
local IMG_TW, IMG_TH = 2, 2
local BODY_IMG
do
  local t = { "SIMG",
              string.char(1, 0),          -- version
              string.char(3),             -- BG mode
              string.char(0),             -- flags
              string.char(IMG_TW, 0),
              string.char(IMG_TH, 0),
              string.char(0, 0, 0, 0) }   -- reserved
  for i = 0, 255 do t[#t + 1] = string.char(i, 0) end
  for j = 0, IMG_TW * IMG_TH * 64 - 1 do t[#t + 1] = string.char((j * 7) & 0xFF) end
  BODY_IMG = table.concat(t)
end

local requests = {}          -- every path served, in order
local webconns = {}          -- sockets still waiting for a full request

local ROUTES = {
  ["/ok"] = { 200, "text/plain", BODY_OK },
  ["/missing"] = { 404, "text/html", BODY_404 },
  ["/big"] = { 200, "text/plain", BODY_BIG },
  ["/pic.img"] = { 200, "application/octet-stream", BODY_IMG },
}

gw.on_connect = function(sock, port)
  if port == 80 then webconns[#webconns + 1] = sock end
end

-- Serve any connection whose request has fully arrived. Response then FIN:
-- HTTP/1.0 close semantics, which is exactly what `get` expects — the body
-- ends when the server closes.
local function web_service()
  for i = #webconns, 1, -1 do
    local sock = webconns[i]
    if sock.reset or sock.done then
      table.remove(webconns, i)
    elseif sock.rx:find("\r\n\r\n", 1, true) then
      local path = sock.rx:match("^GET (%S+) HTTP") or "?"
      requests[#requests + 1] = path
      local r = ROUTES[path] or { 404, "text/html", BODY_404 }
      local reason = (r[1] == 200) and "OK" or "Not Found"
      sock:send("HTTP/1.0 " .. r[1] .. " " .. reason ..
                "\r\nContent-Type: " .. r[2] .. "\r\n\r\n" .. r[3])
      sock:close()
      table.remove(webconns, i)
    end
  end
end

-- The echo scenario's connection, driven by the step machine below.
local echo_sock = nil
-- The /big connection that the kill switch orphans: the console must RST it.
-- At the moment of the kill it is the only port-80 connection still LIVE —
-- every earlier fetch's socket is done (served and closed) or reset (the
-- keypress abort) — which is what makes this findable in an unordered table.
local function live_port80_sock()
  for _, t in pairs(gw.socks) do
    if t.peer_port == 80 and not t.done and not t.reset then return t end
  end
  return nil
end

-- ------------------------------------------------------------------ test
local frame, step, at = 0, 1, nil
local claimed = false
local img_cg, img_vr
local killed_sock = nil

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL net - no mailbox magic"); emu.stop(1) end
    return
  end

  -- Claim the link only once the kernel has zeroed it, otherwise the magic is
  -- wiped out from under us by net_init().
  if not claimed then
    if frame < 200 then return end
    gw:claim()
    claimed = true
    at = frame + 60
    return
  end

  gw:service()
  web_service()

  if frame < at then return end

  if step == 1 then L.feed("net status\n"); at = frame + 300; step = 2; return end
  if step == 2 then L.feed("get http://snos.test/ok -\n"); at = frame + 900; step = 3; return end
  if step == 3 then L.feed("get http://snos.test/missing -\n"); at = frame + 600; step = 4; return end
  if step == 4 then
    -- The paths a real cable cannot be asked to exercise: lose two frames
    -- from the SNES, and hand it two frames with broken checksums. The link
    -- recovers under TCP's feet and the body must arrive byte-perfect.
    gw.drop_snes = 2
    gw.corrupt_out = 2
    L.feed("get http://snos.test/ok -\n")
    at = frame + 1200
    step = 5
    return
  end

  -- The same stream, written to the SRAM volume instead of the screen.
  if step == 5 then
    L.feed("get http://snos.test/ok DLTEST.TXT\n")
    at = frame + 1200
    step = 6
    return
  end
  -- Read it back off the volume. This is the assertion that matters: it proves
  -- the bytes reached storage, not merely that they crossed the link.
  if step == 6 then L.feed("cat DLTEST.TXT\n"); at = frame + 600; step = 7; return end

  -- A download long enough to interrupt. The kill switch only reaches
  -- applications, and a shell command is not one, so without a working abort a
  -- wrong URL means power-cycling the machine.
  if step == 7 then
    L.feed("get http://snos.test/big BIG.TXT\n")
    -- Interrupt while it is genuinely still running: the 20 KB body takes
    -- ~300 frames of TCP, and the abort lands ~100 in.
    at = frame + 160
    step = 8
    return
  end
  if step == 8 then L.feed(" "); at = frame + 400; step = 9; return end
  -- A gap INSIDE the link's send window mid-transfer: only a pipelined sender
  -- can produce this, and go-back-N must repair it byte-perfectly.
  if step == 9 then
    gw.drop_out = 2
    L.feed("get http://snos.test/ok WINLOSS.TXT\n")
    at = frame + 1500
    step = 10
    return
  end
  if step == 10 then L.feed("cat WINLOSS.TXT\n"); at = frame + 600; step = 11; return end
  -- "partial file kept" is a claim; check the volume actually has it.
  if step == 11 then L.feed("ls\n"); at = frame + 300; step = 11.5; return end
  -- TLS is refused with its reason, not attempted and failed obscurely.
  if step == 11.5 then
    L.feed("get https://snos.test/ok -\n")
    at = frame + 300
    step = 12
    return
  end

  -- The image path, post-cutover: no conversion anywhere. The server serves
  -- ready-made Mode 3 tiles (the M12 render-service shape) and the console
  -- stores them verbatim; the assertions read CGRAM and VRAM directly,
  -- because whether a picture actually reached the PPU is not something the
  -- console can be asked.
  if step == 12 then
    L.feed("get http://snos.test/pic.img TINY.IMG\n")
    at = frame + 1200
    step = 13
    return
  end
  if step == 13 then L.feed("run view TINY.IMG\n"); at = frame + 400; step = 14; return end
  if step == 14 then
    -- Sample while the picture is still on screen; the viewer holds until a key.
    img_cg = {}
    img_vr = {}
    for i = 0, 15 do img_cg[i] = emu.read(i, emu.memType.snesCgRam) end
    for i = 0, 15 do img_vr[i] = emu.read(i, emu.memType.snesVideoRam) end
    L.feed(" ")                       -- any key leaves the viewer
    at = frame + 400
    step = 15
    return
  end

  -- Leaked-channel reclamation. A keypress abort is NOT the case that matters:
  -- `get` still returns through its own cleanup and closes politely. The kill
  -- switch is the one that leaks, because it unwinds the application without
  -- running another instruction of it -- which is exactly what it is for.
  if step == 15 then
    L.feed("get http://snos.test/big LEAK.TXT\n")
    at = frame + 160                 -- mid-transfer, while a channel is open
    step = 16
    return
  end
  if step == 16 then
    killed_sock = live_port80_sock()
    emu.write(0xF00E, 1, WRAM)       -- MB_BREAK: the kill switch, as F12 does
    at = frame + 400
    step = 17
    return
  end
  if step == 17 then L.feed("net status\n"); at = frame + 300; step = 18; return end
  -- The server path: bind a port (a NAT forward now), take a connection,
  -- echo, hang up. netecho serves one client per accept, over the console's
  -- own TCP.
  if step == 18 then L.feed("netecho 7777\n"); at = frame + 300; step = 18.5; return end
  if step == 18.5 then
    if not gw.fwd_port then
      print("RESULT FAIL net - netecho never asked for a port forward")
      emu.stop(1)
      return
    end
    echo_sock = gw:dial(gw.fwd_port)
    echo_sock.on_established = function(t)
      t:send("ping over tcp\n")
      -- NOT closed here. uIP 1.0 has no CLOSE_WAIT: a FIN kills both
      -- directions at once, so a client that half-closes immediately races
      -- the echo — the recorded uIP limit (PLAN.md M11). This client does
      -- what a default netcat does: wait for the reply, then close, which
      -- the step below performs. The immediate-half-close case is absorbed
      -- by the DAEMON (slirp.py's FIN grace on forwards), not by uIP.
    end
    at = frame + 900
    step = 18.7
    return
  end
  if step == 18.7 then
    if echo_sock.rx:find("ping over tcp", 1, true) == nil then
      print("RESULT FAIL net - no echo within the wait")
      emu.stop(1)
      return
    end
    echo_sock:close()
    at = frame + 400
    step = 19
    return
  end
  if step == 19 then L.feed(" "); at = frame + 400; step = 20; return end

  local r = L.new_report("net")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")
  print(string.format("server saw %d requests, faults left: drop=%d corrupt=%d out=%d",
        #requests, gw.drop_snes, gw.corrupt_out, gw.drop_out))

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  -- `get` is an application, reached without a `run` prefix.
  r.check(text:find("[get exited", 1, true) ~= nil,
          "get did not run as an application via the name fallback")
  -- "alive" rather than "up": the status line is a HELLO round trip, so this
  -- asserts the peer *answered*, not merely that the mailbox had a magic
  -- word in it.
  r.check(text:find("gateway alive @ 10.0.0.1", 1, true) ~= nil,
          "net status did not get a HELLO reply (with address) from the daemon")
  r.check(text:find("ip 10.0.2.15 via 10.0.2.2", 1, true) ~= nil,
          "net status does not report IP mode against a v2 daemon")

  -- The console's own stack did the talking, correctly.
  r.check(gw.tcp_bad_csum == 0,
          "the console sent " .. gw.tcp_bad_csum ..
          " TCP segments with a bad checksum")
  r.check(gw.dns_queries >= 1,
          "no DNS query arrived - the URL's name was never resolved on-console")

  -- A URL that crossed the link intact proves framing, TCP and the client.
  r.check(#requests >= 3, "server received " .. #requests .. " requests, wanted 3+")
  r.check(requests[1] == "/ok",
          "first path arrived as " .. tostring(requests[1]))

  -- A successful fetch: status parsed from the status line, body printed.
  r.check(text:find("HTTP 200", 1, true) ~= nil, "no HTTP 200 status reported")
  r.check(text:find("over its own TCP", 1, true) ~= nil,
          "body did not arrive intact")

  -- An error status must reach the user, not be swallowed.
  r.check(text:find("HTTP 404", 1, true) ~= nil, "404 status not reported")
  r.check(text:find("not found", 1, true) ~= nil, "404 body missing")

  -- Fault injection must have actually happened, and been survived.
  r.check(gw.drop_snes == 0 and gw.corrupt_out == 0,
          "fault injection never fired")
  local _, bodies = text:gsub("over its own TCP", "")
  r.check(bodies >= 2,
          "body survived " .. bodies .. " times; the fault-injected fetch lost data")

  -- download: the body has to reach the volume, not just the wire.
  r.check(text:find("-> DLTEST.TXT", 1, true) ~= nil, "download did not name a file")
  r.check(text:find("saved DLTEST.TXT", 1, true) ~= nil, "download did not report success")
  local _, all = text:gsub("over its own TCP", "")
  r.check(all >= 3,
          "cat of the downloaded file did not return the body (" .. all .. " copies seen)")

  -- abort: a long transfer must stop on a keypress and keep what it had.
  r.check(text:find("[aborted", 1, true) ~= nil,
          "a keypress did not abort the download - a stuck transfer would need a power cycle")
  r.check(text:find("BIG.TXT", 1, true) ~= nil,
          "the aborted download left nothing on the volume, despite saying it kept a partial file")

  -- Loss inside the link's send window must be repaired, not merely survived.
  r.check(gw.drop_out == 0, "window-loss injection never fired")
  r.check(text:find("saved WINLOSS.TXT", 1, true) ~= nil,
          "the download that lost frames mid-window never completed")
  local _, wl = text:gsub("over its own TCP", "")
  r.check(wl >= 4,
          "the file downloaded across a window gap did not read back intact (" ..
          wl .. " copies of the body seen)")

  -- TLS: refused with the reason, not attempted.
  r.check(text:find("https needs TLS", 1, true) ~= nil,
          "https was not refused with its reason")

  -- The kill switch leaves a connection open by construction: no amount of
  -- tidy code in the app reaches its own close. The kernel reclaims the
  -- channel (and says so), and the reclaim must reach the WIRE as a RST —
  -- the daemon holds real state for this connection and has to be told.
  r.check(text:find("[get killed]", 1, true) ~= nil,
          "the kill switch did not terminate the download")
  r.check(text:find("left 1 connection open - closed", 1, true) ~= nil,
          "a channel leaked by a killed app was not reclaimed")
  r.check(killed_sock ~= nil and killed_sock.reset == true,
          "the reclaimed connection never RST the peer - the daemon's NAT " ..
          "entry would live until timeout")

  -- The image file: stored verbatim, viewed from the volume.
  r.check(text:find("saved TINY.IMG", 1, true) ~= nil,
          "the .IMG download did not save")
  if img_cg then
    local pal_ok = true
    for i = 0, 7 do
      if img_cg[i * 2] ~= i or img_cg[i * 2 + 1] ~= 0 then pal_ok = false end
    end
    r.check(pal_ok, "palette did not reach CGRAM intact")
  else
    r.check(false, "never sampled CGRAM")
  end
  if img_vr then
    local vr_ok = true
    for i = 0, 15 do
      if img_vr[i] ~= ((i * 7) & 0xFF) then vr_ok = false end
    end
    r.check(vr_ok, "tile data did not reach VRAM in order")
  else
    r.check(false, "never sampled VRAM")
  end
  r.check(text:find("viewed TINY.IMG", 1, true) ~= nil,
          "the viewer never returned to the shell")

  -- Servers: netecho bound a port through the NAT, took a connection from
  -- outside, and sent bytes it composed itself.
  r.check(gw.fwd_port == 7777,
          "netecho did not forward the port it was given (got " ..
          tostring(gw.fwd_port) .. ")")
  r.check(echo_sock ~= nil and
          echo_sock.rx:find("ping over tcp", 1, true) ~= nil,
          "the echo server did not send the bytes back: got '" ..
          tostring(echo_sock and echo_sock.rx) .. "'")
  -- The client half-closed. An app that treats EOF as "give up" never
  -- replies, and one that hangs up early truncates; the FIN after the echo
  -- asserts it did neither.
  r.check(echo_sock ~= nil and echo_sock.fin_rcvd,
          "the echo server never hung up, so the connection would hang until timeout")

  r.finish()
end, emu.eventType.endFrame)
