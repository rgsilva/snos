-- `net speedtest` on the LINK_HW ROM, through a model of the Pico adapter on
-- controller port 2 (test/hwport.lua) instead of the WRAM mailbox: the real
-- kernel/linkxfer.s, NMI timing and foreground poll gate under test. The
-- peer is gwip.lua shaped like the Pico's NAT speedtest peer: 64-byte
-- frames, MSS 1460, one segment in flight, a fresh segment the moment the
-- last is acknowledged, upload counted over its own fixed window.
--
-- Build and run with `make LINK_HW=1 test-hwspeed`; HWSPEED="..." sets the
-- options below (see the Makefile), for instance
--   make LINK_HW=1 test-hwspeed HWSPEED="seconds=5, tune={[[net tune window 1]]}"
-- The verdict is that all three phases complete with intact byte counts, no
-- checksum failures and no panic; the rates are printed for the bench
-- (docs/link-throughput.md), not asserted.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")
local P = dofile("test/hwport.lua")

local CONFIG = {}
-- @CONFIG@ (the Makefile inserts `CONFIG = {...}` here)
local seconds = CONFIG.seconds or 10
local gw = G.new{addr = "10.0.2.99", frame = CONFIG.frame or 64, mss = 1460, rto_polls = 150,
                 window = CONFIG.window or 1}
local port = P.new(gw, {pico_us = CONFIG.pico_us or 300, frame = CONFIG.frame or 64,
                        staging = CONFIG.staging})
local r = L.new_report("hwspeed")

-- The profiler (HWPROF=1): exec callbacks at each listed kernel function's
-- entry and `rtl`s (tools/hwprof.py), timing it in master cycles. Time the
-- NMI spent inside a foreground function is subtracted from it, so each line
-- is what that code itself costs.
local prof = {}
if CONFIG.prof then
  local stack, nmi_total = {}, 0
  local function now() return emu.getState()["masterClock"] end
  local function rec(name, dt)
    local p = prof[name]
    if not p then p = { n = 0, sum = 0, min = 1 / 0, max = 0 }; prof[name] = p end
    p.n, p.sum = p.n + 1, p.sum + dt
    if dt < p.min then p.min = dt end
    if dt > p.max then p.max = dt end
  end
  local uip_len = CONFIG.prof.uip_len and CONFIG.prof.uip_len[1]
  for name, addrs in pairs(CONFIG.prof) do
    if #addrs == 1 then goto continue end
    emu.addMemoryCallback(function()
      local st = emu.getState()
      local e = { name = name, t = st["masterClock"], nmi = nmi_total }
      if name == "uip_process" and uip_len then
        -- uip_process(flag): 1 is a datagram in, of uip_len bytes; the rest
        -- (timer, poll) build whatever the connection sends.
        local flag, len = st["cpu.a"] & 0xFF, emu.read16(uip_len, emu.memType.snesMemory)
        e.sig = flag == 1 and string.format("uip_in_%s", len <= 64 and "small" or
                                            len <= 600 and "mid" or "big")
                or string.format("uip_flag%d", flag)
      end
      if name == "snos_cop_c" then
        -- The COP signature byte names the syscall: PB:PC-1 from the frame.
        local sp = st["cpu.a"] & 0xFFFF
        local pc = emu.read16(sp + 11, emu.memType.snesMemory)
        local pb = emu.read(sp + 13, emu.memType.snesMemory)
        e.sig = emu.read((pb << 16) | ((pc - 1) & 0xFFFF), emu.memType.snesMemory)
      end
      stack[#stack + 1] = e
    end, emu.callbackType.exec, addrs[1])
    for i = 2, #addrs do
      emu.addMemoryCallback(function()
        local top = stack[#stack]
        if not top or top.name ~= name then return end
        stack[#stack] = nil
        local dt = now() - top.t
        if name == "snos_nmi_c" then
          nmi_total = nmi_total + dt
          rec(name, dt)
        else
          local label = name
        if type(top.sig) == "number" then label = string.format("cop_%02x", top.sig)
        elseif top.sig then label = top.sig end
        rec(label, dt - (nmi_total - top.nmi))
        if CONFIG.timeline and port.exchanges >= CONFIG.timeline[1] and
           port.exchanges < CONFIG.timeline[2] and label ~= "cop_63" and
           label ~= "cop_50" and label ~= "cop_21" and label ~= "cop_69" then
          print(string.format("tl %6d x%d %-14s start %9.0f lines %6.1f", port.exchanges,
            port.exchanges, label, top.t / 1364 % 262, (dt - (nmi_total - top.nmi)) / 1364))
        end
        end
      end, emu.callbackType.exec, addrs[i])
    end
    ::continue::
  end
end

-- CONFIG.linewatch: print where net_service_fg starts and ends whenever it
-- starts outside the zones link_serial_fg_fits allows.
if CONFIG.prof and CONFIG.linewatch and CONFIG.prof.net_service_fg then
  local a = CONFIG.prof.net_service_fg
  local started, shown = nil, 0
  emu.addMemoryCallback(function()
    local st = emu.getState()
    started = { line = st["ppu.scanline"], t = st["masterClock"] }
  end, emu.callbackType.exec, a[1])
  for i = 2, #a do
    emu.addMemoryCallback(function()
      local st = emu.getState()
      if started and (started.line > 110 and started.line < 229) and shown < 10 then
        shown = shown + 1
        print(string.format("linewatch: fg poll from line %d to line %d (%.1f lines)",
          started.line, st["ppu.scanline"], (st["masterClock"] - started.t) / 1364))
      end
    end, emu.callbackType.exec, a[i])
  end
end

-- With net_service profiled: how long its C runs before the exchange's first
-- clock, the part of a poll that varies.
if CONFIG.prof and CONFIG.prof.net_service then
  local entered
  emu.addMemoryCallback(function()
    entered = emu.getState()["masterClock"]
  end, emu.callbackType.exec, CONFIG.prof.net_service[1])
  local base = port.on_exchange
  port.on_exchange = function(f, sent, t_first, t_end)
    if entered and entered < t_first then
      local d = t_first - entered
      local p = prof["svc_to_first_clock"] or { n = 0, sum = 0, min = 1 / 0, max = 0 }
      prof["svc_to_first_clock"] = p
      p.n, p.sum = p.n + 1, p.sum + d
      if d < p.min then p.min = d end
      if d > p.max then p.max = d end
    end
    entered = nil
    if base then base(f, sent, t_first, t_end) end
  end
end

-- CONFIG.stackwatch: the stack pointer at kbd_idle's entry -- input.c's
-- fg_poll_room wants 640 bytes above the slot's floor for a foreground poll.
local sp_seen = {}
if CONFIG.prof and CONFIG.stackwatch and CONFIG.prof.kbd_idle then
  emu.addMemoryCallback(function()
    local sp = emu.getState()["cpu.sp"]
    sp_seen[sp] = (sp_seen[sp] or 0) + 1
  end, emu.callbackType.exec, CONFIG.prof.kbd_idle[1])
end

local function prof_report()
  for sp, n in pairs(sp_seen) do
    print(string.format("stackwatch: kbd_idle entered with S=%04x %d times", sp, n))
  end
  local names = {}
  for name in pairs(prof) do names[#names + 1] = name end
  table.sort(names)
  for _, name in ipairs(names) do
    local p = prof[name]
    print(string.format("hwprof: %-18s n=%6d mean=%8.0f min=%8.0f max=%8.0f mc (mean %.1f lines)",
      name, p.n, p.sum / p.n, p.min, p.max, p.sum / p.n / 1364))
  end
end

-- CONFIG.trace: one line per datagram crossing the link, numbered by
-- exchange, to line up against the host bench's BENCH_TRACE.
if CONFIG.trace then
  gw.observer = function(pkt)
    print(string.format("trace %6d up   %d flags %02x", port.exchanges, #pkt,
                        #pkt >= 40 and pkt:byte(34) or 0))
    return false
  end
  local send = gw.send_dgram
  gw.send_dgram = function(p)
    print(string.format("trace %6d down %d flags %02x", port.exchanges, #p,
                        #p >= 40 and p:byte(34) or 0))
    send(p)
  end
end

local sockets = {}
local frame, launched, completed, output_start = 0, false, nil, 1
local tunes = CONFIG.tune or {}
if type(tunes) == "string" then tunes = { tunes } end
local tune_i = 1

gw.on_connect = function(t, dest_port)
  r.check(dest_port == 7778 and t.peer_ip == "10.0.2.99", "wrong speedtest target")
  sockets[#sockets + 1] = t
end

local function serve()
  for _, t in ipairs(sockets) do
    if not t.reset and not t.done then
      if not t.kind then
        local k, n, rest = t.rx:match("^([DU]) (%d+)\n(.*)$")
        if k then
          t.kind, t.frames, t.rx = k, tonumber(n), rest
          t:send("OK\n")
        end
      elseif not t.start then
        if t.rx:sub(1, 3) == "GO\n" then
          t.rx = t.rx:sub(4)
          t.start, t.count = frame, 0
        end
      elseif t.kind == "D" then
        if not t.closing and #t.txq < 4096 then t:send(string.rep("x", 4096)) end
      elseif not t.reported then
        if frame - t.start >= t.frames then
          t.reported = true
          t:send(tostring(t.count) .. "\n")
        else
          r.check(t.rx:match("^x*$") ~= nil, "upload payload corrupt")
          t.count = t.count + #t.rx
          t.rx = ""
        end
      end
    end
  end
end

local last_report = { frame = 0, exchanges = 0, kicks = 0, resends = 0 }
local function rate_line()
  local f = frame - last_report.frame
  print(string.format("hwspeed: frame %d: %.2f exchanges/frame, %d kicks, %d peer resends",
    frame, (port.exchanges - last_report.exchanges) / f, port.kicks - last_report.kicks,
    gw.hw_resends - last_report.resends))
  last_report = { frame = frame, exchanges = port.exchanges, kicks = port.kicks,
                  resends = gw.hw_resends }
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then r.check(false, "no boot"); r.finish() end
    return
  end
  serve()
  if frame % 600 == 0 then rate_line() end
  if not launched then
    -- HELLO is asked for every two seconds until answered; give it time.
    if not gw.hello_seen or frame < 300 then return end
    if tune_i <= #tunes then
      if frame % 30 == 0 then
        L.feed(tunes[tune_i] .. "\n")
        tune_i = tune_i + 1
      end
      return
    end
    output_start = #L.ring_text() + 1
    L.feed("net speedtest - " .. seconds .. "\n")
    launched = frame
    return
  end
  local text = L.ring_text():sub(output_start)
  if text:find("refused ", 1, true) and not completed then completed = frame end
  if frame - launched > 60 * (3 * seconds + 90) then
    print(L.ring_text()); r.check(false, "test deadline"); r.finish(); return
  end
  if not completed or frame - completed < 30 then return end
  print("--- console ---"); print(L.ring_text())
  rate_line()
  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  for _, label in ipairs({"download", "upload", "both download", "both upload"}) do
    local rate = text:match(label .. ": (%d+) B/s")
    r.check(rate ~= nil, "no " .. label .. " result")
  end
  local sums = text:match("bad sums (%d+)")
  r.check(sums == "0", "checksum failures on the modelled wire")
  r.check(gw.tcp_bad_csum == 0, "bad TCP checksum")
  print(string.format("hwspeed: %d exchanges in %d frames, %d kicks, first-to-last clock %.0f mc",
    port.exchanges, frame, port.kicks, (port.span_sum or 0) / math.max(port.exchanges, 1)))
  r.check(port.collisions == 0, port.collisions .. " exchanges were still running at vblank")
  port.mask_report(r, "hwspeed", true)
  prof_report()
  r.finish()
end, emu.eventType.endFrame)
