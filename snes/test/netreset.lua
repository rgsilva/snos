-- Reset an occupied network, replace its peer, and use the new session.
-- A second reset interrupts DNS with nobody answering; a third resets a
-- live peer and mc's two listeners, exercising handle reclamation again.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local WRAM = emu.memType.snesWorkRam
local gw = G.new{addr = "10.0.0.1"}
local online = false
local frame, step, at = 0, 0, 200
local old, fresh
local before_echo, after_echo = false, false
local first_report, absent_report, live_report
local first_jobs, absent_jobs, live_jobs
local dns_started = false
local recovered_body = false
local web = {}
local r = L.new_report("netreset")

local function send(cmd, delay)
  L.feed(cmd)
  at = frame + delay
end

local function detach()
  online = false
  emu.write16(0xE000, 0, WRAM) -- LINK_MB_MAGIC: the adapter is unplugged
end

local function replacement(addr)
  gw = G.new{addr = addr, dns = {["reset.test"] = {10, 0, 2, 99}}}
  gw.on_connect = function(sock) web[#web + 1] = sock end
  gw:claim()
  online = true
end

local function dial()
  local sock = gw:dial(7777)
  sock.on_established = function(t) t:send("reset echo\n") end
  return sock
end

emu.addEventCallback(function()
  frame = frame + 1
  if frame > 10000 or L.status() == L.STATUS_PANIC then
    print(L.ring_text())
    r.check(false, "timeout or panic at step " .. step)
    r.finish()
    return
  end
  if L.magic() ~= 0xCAFE then return end
  if online then gw:service() end
  for i = #web, 1, -1 do
    local sock = web[i]
    if sock.rx:find("\r\n\r\n", 1, true) then
      local body = "TCP after network reset\n"
      sock:send("HTTP/1.0 200 OK\r\nContent-Length: " .. #body ..
                "\r\n\r\n" .. body)
      sock:close()
      table.remove(web, i)
    end
  end
  if frame < at then return end

  if step == 0 then
    gw:claim(); online = true
    at = frame + 200; step = 1; return
  end
  if step == 1 then
    r.check(gw.hello_seen, "initial peer never received HELLO")
    send("run netecho 7777 &\n", 300); step = 2; return
  end
  if step == 2 then
    r.check(gw.fwd_port == 7777, "initial listener never bound")
    old = dial(); at = frame + 250; step = 3; return
  end
  if step == 3 then
    before_echo = old.rx:find("reset echo", 1, true) ~= nil
    detach()
    send("net reset\n", 45); step = 4; return
  end
  if step == 4 then
    replacement("10.0.0.2")
    at = frame + 300; step = 5; return
  end
  if step == 5 then
    first_report = L.ring_text()
    r.check(gw.hello_seen, "replacement peer never received HELLO")
    send("jobs\njobs\n", 100); step = 6; return
  end
  if step == 6 then
    first_jobs = L.ring_text()
    -- Hold a real lookup BUSY, then remove its peer before it can answer.
    gw.observer = function(p)
      if p:byte(10) == 17 then dns_started = true; return true end
      return false
    end
    send("run get http://pending.test/ - &\n", 30); step = 7; return
  end
  if step == 7 then
    r.check(dns_started, "background get never started its DNS lookup")
    detach()
    send("net reset\n", 260); step = 8; return
  end
  if step == 8 then
    absent_report = L.ring_text()
    send("jobs\njobs\n", 100); step = 9; return
  end
  if step == 9 then
    absent_jobs = L.ring_text()
    replacement("10.0.0.3")
    at = frame + 200; step = 10; return
  end
  if step == 10 then
    send("get http://reset.test/ -\n", 500); step = 11; return
  end
  if step == 11 then
    recovered_body = L.ring_text():find("TCP after network reset", 1, true) ~= nil
    r.check(gw.dns_queries > 0, "resolver was not usable after reset")
    r.check(gw.tcp_syns > 0 and gw.tcp_bad_csum == 0,
            "fresh TCP connection failed or sent a bad checksum")
    send("run mc &\n", 350); step = 12; return
  end
  if step == 12 then
    r.check(gw.fwd_port == 25565, "mc did not bind before the live reset")
    send("net reset\n", 350); step = 13; return
  end
  if step == 13 then
    live_report = L.ring_text()
    send("jobs\njobs\n", 100); step = 14; return
  end
  if step == 14 then
    live_jobs = L.ring_text()
    send("run netecho 7777 &\n", 300); step = 15; return
  end
  if step == 15 then
    r.check(gw.fwd_port == 7777, "new listener could not reclaim a channel")
    fresh = dial(); at = frame + 250; step = 16; return
  end
  if step == 16 then
    after_echo = fresh.rx:find("reset echo", 1, true) ~= nil
    send("kill 1\n", 100); step = 17; return
  end

  print("--- first reset ---\n" .. first_report)
  print("--- absent peer ---\n" .. absent_report)
  print("--- live peer ---\n" .. live_report)
  r.check(before_echo, "old session never carried application data")
  r.check(first_report:find("1 channel failed, IP cleared", 1, true) ~= nil,
          "reset did not fail the background channel")
  r.check(first_report:find("gateway alive @ 10.0.0.2", 1, true) ~= nil,
          "reset did not report the replacement's address")
  r.check(first_jobs:find("no background jobs", 1, true) ~= nil,
          "netecho remained wedged on the dead session")
  r.check(absent_report:find("1 channel failed, IP cleared\nno link, no answer in 3s", 1, true) ~= nil,
          "reset without a peer did not return with a bounded failure")
  r.check(absent_jobs:find("[1] get  exited", 1, true) ~= nil,
          "background get did not exit after its pending lookup was reset")
  r.check(recovered_body, "DNS and TCP did not recover after the absent-peer reset")
  r.check(live_report:find("2 channels failed, IP cleared", 1, true) ~= nil,
          "reset did not fail both mc listeners")
  r.check(live_report:find("gateway alive @ 10.0.0.3", 1, true) ~= nil,
          "a running peer could not resynchronise after reset")
  r.check(live_jobs:find("[1] mc  exited", 1, true) ~= nil,
          "mc remained wedged on its dead listeners")
  r.check(after_echo, "a fresh background listener did not echo after recovery")
  r.finish()
end, emu.eventType.endFrame)
