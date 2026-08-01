-- M11 -- the console composes and answers real IPv4 packets.
--
-- The transport and the generic peer live in test/gwip.lua now (this file is
-- where they were born; the "consolidating the Lua gateway copies" note is
-- executed). What stays HERE is everything with a verdict in it:
--
--   1. host -> SNES: an ICMP echo request (with an ODD-length payload, which
--      is the csum_odd path) comes back as a byte-correct reply -- addresses
--      swapped, type 0, payload echoed, BOTH checksums freshly valid. The
--      checksums are recomputed in Lua, so the console's asm and this test
--      cannot share a bug.
--   2. SNES -> host: `ping 10.0.2.2` typed at the shell produces echo
--      requests the peer answers, and the shell prints replies with an RTT.
--   3. DNS the console builds and parses itself (compression pointer and
--      all), NXDOMAIN included, and `ping NAME` chaining both.
--   4. TCP both directions: `debug send` by address and BY NAME, and httpd
--      serving two consecutive inbound connections through the forward.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local WRAM = emu.memType.snesWorkRam
local csum, be16 = G.csum, G.be16

local gw = G.new{
  addr = "10.0.0.1",
  dns = { ["snos.test"] = { 93, 184, 216, 34 } },
}

-- The kernel must HELLO all by itself, before anything is typed: IP mode is
-- autoconfiguration, and nobody types `net status` before using the network.
local typed_anything = false
local hello_before_typing = false
gw.on_hello = function()
  if not typed_anything then hello_before_typing = true end
end

-- ------------------------------------------------- our echo, our verdict
-- Composed HERE, not via gwip helpers, so a shared bug is impossible.
local ODD_PAYLOAD = "SNOSPING!"     -- 9 bytes: exercises the odd-byte path
local reply_ok = nil                -- verdict on the reply to OUR request
local reply_err = "no reply seen"

local function mk_echo_req(id, seq, payload)
  local icmp = string.char(8, 0) .. "\0\0" .. be16(id) .. be16(seq) .. payload
  local ics = 0xFFFF - csum(icmp, 1, #icmp)
  icmp = icmp:sub(1, 2) .. be16(ics) .. icmp:sub(5)
  local tot = 20 + #icmp
  local ip = string.char(0x45, 0) .. be16(tot) .. be16(0x1234) ..
             be16(0x4000) .. string.char(64, 1) .. "\0\0" ..
             string.char(10, 0, 2, 2) .. string.char(10, 0, 2, 15)
  local ips = 0xFFFF - csum(ip, 1, #ip)
  ip = ip:sub(1, 10) .. be16(ips) .. ip:sub(13)
  return ip .. icmp
end

-- Sees every datagram from the console before gwip's default handling; the
-- echo REPLY to our request is ours to judge (and consume).
gw.observer = function(p)
  if #p < 20 then return false end
  local ihl = (p:byte(1) & 0x0F) * 4
  if p:byte(10) ~= 1 then return false end
  if p:byte(ihl + 1) ~= 0 then return false end     -- not an echo reply
  if reply_ok ~= nil then return true end           -- already judged
  local src = table.concat({ p:byte(13, 16) }, ".")
  local dst = table.concat({ p:byte(17, 20) }, ".")
  if csum(p, 1, ihl) ~= 0xFFFF then
    reply_err = "bad IP checksum"; reply_ok = false; return true
  end
  if src ~= "10.0.2.15" or dst ~= "10.0.2.2" then
    reply_err = "addresses not swapped (" .. src .. "->" .. dst .. ")"
    reply_ok = false; return true
  end
  if csum(p, ihl + 1, #p) ~= 0xFFFF then
    reply_err = "bad ICMP checksum"; reply_ok = false; return true
  end
  if p:byte(ihl + 5) ~= 0x77 or p:byte(ihl + 6) ~= 0x11 then
    reply_err = "id not echoed"; reply_ok = false; return true
  end
  if p:sub(ihl + 9) ~= ODD_PAYLOAD then
    reply_err = "payload not echoed byte-for-byte"; reply_ok = false; return true
  end
  reply_ok = true
  return true
end

-- --------------------------------------------------------- the TCP server
-- An HTTP-shaped answer for `debug send`, served to any outbound connection.
local BODY = "HELLO FROM REAL TCP\n"
local RESPONSE = "HTTP/1.0 200 OK\r\nContent-Length: " .. #BODY ..
                 "\r\nConnection: close\r\n\r\n" .. BODY
local tcp_requests = 0
local webconns = {}

gw.on_connect = function(sock, port)
  webconns[#webconns + 1] = sock
end

local function web_service()
  for i = #webconns, 1, -1 do
    local sock = webconns[i]
    if sock.reset or sock.done then
      table.remove(webconns, i)
    elseif sock.rx:find("\r\n\r\n", 1, true) then
      tcp_requests = tcp_requests + 1
      sock:send(RESPONSE)
      sock:close()
      table.remove(webconns, i)
    end
  end
end

-- Inbound: dial httpd through the forward, twice, and keep each body.
local inbound = nil
local inbound_bodies = {}

local function inbound_open(port)
  inbound = gw:dial(port)
  inbound.on_established = function(t)
    t:send("GET / HTTP/1.0\r\nHost: snes\r\n\r\n")
  end
  inbound.on_fin = function(t)
    inbound_bodies[#inbound_bodies + 1] = t.rx
    t:close()
  end
end

-- -------------------------------------------------------------------- test
local frame = 0
local step, at = 1, nil
local claimed = false
local connect_typed = nil    -- gw.frame when `debug send` was typed

local function send(cmd, settle)
  typed_anything = true
  L.feed(cmd)
  at = frame + settle
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL ip - no mailbox magic"); emu.stop(1) end
    return
  end
  if not claimed then
    if frame < 200 then return end
    gw:claim()
    claimed = true
    at = frame + 30
    return
  end
  gw:service()
  web_service()

  if at and frame < at then return end

  if step == 1 then
    -- Say nothing. The kernel must ask who we are all by itself.
    if not gw.hello_seen then
      if frame > 2000 then
        print("RESULT FAIL ip - kernel never auto-helloed"); emu.stop(1)
      end
      return
    end
    at = frame + 120 -- let the v2 reply land and arm the stack
    step = 2
    return
  end
  if step == 2 then
    -- Host -> SNES, odd-length payload, fixed id 0x7711.
    gw.send_dgram(mk_echo_req(0x7711, 1, ODD_PAYLOAD))
    at = frame + 300
    step = 3
    return
  end
  if step == 3 then send("ping 10.0.2.2\n", 700); step = 4; return end
  -- The console's OWN resolver: a real query it composed, an answer it
  -- parsed -- including stepping over a compression pointer without
  -- following it, which the answer deliberately contains.
  if step == 4 then send("resolve snos.test\n", 400); step = 4.5; return end
  if step == 4.5 then send("resolve nope.test\n", 400); step = 4.7; return end
  -- ...and a NAME as a ping target, which is both features at once.
  if step == 4.7 then send("ping snos.test\n", 700); step = 5; return end
  -- The console's OWN TCP: `debug send` connects, sends a request and
  -- prints the answer. Not one line of that command changed for M11 - the
  -- socket underneath it did.
  if step == 5 then
    connect_typed = gw.frame
    send("debug send 10.0.2.99:80\n", 900); step = 5.5; return
  end
  -- ...and again BY NAME, which is a different path entirely: the connect
  -- waits on the resolver before uIP ever sees it. Testing only the dotted
  -- quad left that half uncovered, and it was broken.
  if step == 5.5 then send("debug send snos.test:80\n", 900); step = 5.55; return end
  if step == 5.55 then send("mem\n", 300); step = 5.6; return end
  -- INBOUND: httpd serves a page over the console's own TCP, to a
  -- connection opened from outside the NAT.
  if step == 5.6 then send("run httpd 8080 &\n", 400); step = 5.62; return end
  if step == 5.62 then
    if not gw.fwd_port then
      print("--- console ---"); print(L.ring_text())
      print("RESULT FAIL ip - httpd never asked for a port forward")
      emu.stop(1); return
    end
    inbound_open(gw.fwd_port)
    at = frame + 900
    step = 5.7
    return
  end
  -- A SECOND connection on the same port. `httpd` hangs up and returns to
  -- LISTEN between requests -- that loop is what ip_hangup exists for, and
  -- one connection would never exercise it.
  if step == 5.7 then
    inbound_open(gw.fwd_port)
    at = frame + 900
    step = 5.75
    return
  end
  if step == 5.75 then send("kill 1\n", 300); step = 5.8; return end
  if step == 5.8 then send("net status\n", 400); step = 6; return end

  local r = L.new_report("ip")
  local text = L.ring_text()
  print("--- console ---"); print(text); print("---")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(hello_before_typing,
          "the kernel did not HELLO on its own - IP mode needs autoconfig, " ..
          "nobody types `net status` before using the network")
  r.check(reply_ok ~= nil, "no echo reply from the console: " .. reply_err)
  r.check(reply_ok == true, "echo reply wrong: " .. reply_err)
  r.check(gw.snes_pings >= 8,
          "`ping` sent " .. gw.snes_pings .. " echo requests, expected 8 (two runs of four)")
  r.check(text:find("reply from 10.0.2.2: seq=", 1, true) ~= nil,
          "the shell never printed a ping reply")
  r.check(text:find("timeout", 1, true) == nil,
          "some pings timed out on a perfect link")
  r.check(text:find("ip 10.0.2.15 via 10.0.2.2", 1, true) ~= nil,
          "`net status` does not report IP mode against a v2 gateway")
  -- DNS, composed and parsed on the console
  r.check(gw.dns_queries >= 2,
          "the console sent " .. gw.dns_queries .. " DNS queries, expected 2+")
  r.check(text:find("snos.test is 93.184.216.34", 1, true) ~= nil,
          "the console did not resolve snos.test - a real query it built " ..
          "and an answer it parsed, compression pointer and all")
  r.check(text:find("cannot resolve nope.test", 1, true) ~= nil,
          "NXDOMAIN was not reported as a failure")
  r.check(text:find("reply from 93.184.216.34", 1, true) ~= nil,
          "`ping NAME` did not resolve-then-ping")
  -- TCP, entirely on the console: handshake, request, response, close
  r.check(gw.tcp_syns >= 1, "the console never sent a SYN - SYS_NET_CONNECT is " ..
          "not reaching the stack")
  -- uip_connect only CREATES a connection; the SYN goes out of the periodic
  -- timer's SYN_SENT arm, so without an immediate poke every connection on
  -- this machine starts with up to half a second of silence. 40 frames is
  -- two-thirds of a second: comfortably past the tick, so this fails if the
  -- poke is ever removed.
  local lag = (gw.first_syn_frame or 99999) - (connect_typed or 0)
  r.check(lag < 40,
          "the SYN took " .. lag .. " frames to leave after the command - " ..
          "uip_connect is waiting for the 30-frame periodic instead of " ..
          "sending immediately")
  r.check(gw.tcp_bad_csum == 0,
          "the console sent " .. tostring(gw.tcp_bad_csum) ..
          " TCP segments with a bad checksum")
  r.check(tcp_requests >= 1,
          "the request never arrived complete - the handshake finished but " ..
          "data did not cross")
  local _, hellos = text:gsub("HELLO FROM REAL TCP", "")
  r.check(hellos >= 2,
          "only " .. hellos .. " of the two `debug send` runs got a response " ..
          "- connecting BY NAME goes through the resolver first and is a " ..
          "different path from connecting to an address")
  r.check(text:find("uip tcp rx 0 ", 1, true) == nil,
          "uIP says it received no TCP at all")
  -- ...and INBOUND: the console as a server on its own stack
  r.check(gw.fwd_port == 8080,
          "httpd asked to forward port " .. tostring(gw.fwd_port) .. ", not 8080")
  local last = inbound_bodies[#inbound_bodies] or ""
  r.check(last:find("HTTP/1.0 200", 1, true) ~= nil or
          last:find("HTTP/1.1 200", 1, true) ~= nil,
          "httpd served nothing over the stack (got " .. #last ..
          " bytes: " .. last:sub(1, 60):gsub("[^%\32-%\126]", ".") .. ")")
  r.check(last:find("Hello from a SNES", 1, true) ~= nil,
          "the served page is not the console's INDEX.HTM")
  r.check(#inbound_bodies >= 2 and
          inbound_bodies[1]:find("Hello from a SNES", 1, true) ~= nil,
          "the FIRST connection got no page - httpd only serves once, so " ..
          "the listen loop does not survive a hangup")
  r.check(#last > 400,
          "only " .. #last .. " bytes of the page arrived - the " ..
          "response is being truncated, not just delayed")
  -- uIP is the thing answering, not leftover hand-written code: the stack's
  -- OWN counters have to show the echo request arriving and a reply leaving.
  -- A correct reply on the wire proves only that something replied.
  local irecv = text:match("uip ip (%d+)/(%d+)")
  local isent = select(2, text:match("uip ip (%d+)/(%d+)"))
  r.check(irecv ~= nil, "`net status` does not report uIP's counters")
  r.check(tonumber(irecv or "0") >= 1,
          "uIP processed no datagrams - the stack is linked but not running")
  r.check(tonumber(isent or "0") >= 1,
          "uIP sent nothing - the echo reply came from somewhere else")
  local icrecv, icsent = text:match("icmp (%d+)/(%d+)")
  r.check(tonumber(icsent or "0") >= 1,
          "uIP's ICMP counter says it never replied (" ..
          tostring(icrecv) .. "/" .. tostring(icsent) .. ")")
  -- uIP's tcp.sent sits on the shared IP output path and counts UDP as TCP;
  -- net status must not report it (three DNS queries once read "tcp 0/3").
  r.check(text:find("uip tcp rx ", 1, true) ~= nil,
          "the uIP TCP line is missing")
  r.check(text:match("uip tcp rx %d+/") == nil,
          "net status is reporting uIP's tcp.sent, which counts UDP output " ..
          "as TCP - the number is a library quirk, not a measurement")

  r.finish()
end, emu.eventType.endFrame)
