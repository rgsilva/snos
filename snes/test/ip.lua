-- M11 step 1 -- the console composes and answers real IPv4 packets.
--
-- This gateway claims HELLO version 2, which is the whole capability
-- negotiation: the kernel must ask (auto-hello, unprompted by any typing),
-- see "2", and only then put LINK_CH_IP records on the wire. Then both ping
-- directions are proven:
--   1. host -> SNES: an ICMP echo request (with an ODD-length payload, which
--      is the csum_odd path) comes back as a byte-correct reply -- addresses
--      swapped, type 0, payload echoed, BOTH checksums freshly valid. The
--      checksums are recomputed HERE, in Lua, so the console's asm and this
--      test cannot share a bug.
--   2. SNES -> host: `ping 10.0.2.2` typed at the shell produces echo
--      requests this script answers, and the shell prints the replies with
--      an RTT.
--
-- Transport core copied from bg.lua/telnetd.lua; consolidating the Lua
-- gateway copies is queued for M7.
local L = dofile("test/lib.lua")

local MB = 0xE000
local MAGIC = 0x4B4C
local FRAME, HDR = 128, 4
local TXOFF, RXOFF = 0x10, 0x10 + FRAME
local SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
local F_DATA = 0x01
local CH_CTRL, CH_PAD, CH_IP = 0, 0xFF, 0x10
local WRAM = emu.memType.snesWorkRam

local C2S_HELLO = 0x04
local S2C_HELLO = 0x84

local function rb(o) return emu.read(MB + o, WRAM) end
local function wb(o, v) emu.write(MB + o, v & 0xFF, WRAM) end
local function ru16(o) return emu.read16(MB + o, WRAM) end
local function wu16(o, v) emu.write16(MB + o, v & 0xFFFF, WRAM) end

local function sum_of(f)
  local s = 0
  for i = 0, FRAME - 1 do
    if i ~= SUM then s = s + f[i] end
  end
  return s & 0xFF
end

local gw = {
  tx_seq = 0, rx_next = 0, last_snes_seq = 0, out = {},
  cmsg = { state = 0, typ = 0, len = 0, buf = {} },
  window = {}, stall = 0,
  upstream = {}, hello_seen = false, hello_before_typing = false,
}

local function push(ch, s)
  gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 }
end

local function push_ctrl(typ, payload)
  push(CH_CTRL, string.char(typ, #payload) .. payload)
end

local typed_anything = false

local function ctrl_byte(b)
  local c = gw.cmsg
  if c.state == 0 then
    c.typ = b; c.state = 1
  elseif c.state == 1 then
    c.len = b; c.buf = {}; c.state = (b > 0) and 2 or 0
    if c.state == 0 then c.dispatch = true end
  else
    c.buf[#c.buf + 1] = b
    if #c.buf >= c.len then c.state = 0; c.dispatch = true end
  end
  if not c.dispatch then return end
  c.dispatch = false

  if c.typ == C2S_HELLO then
    gw.hello_seen = true
    if not typed_anything then gw.hello_before_typing = true end
    -- Version 2: "I am a slirp box." This one byte is what arms the
    -- console's whole IP layer.
    push_ctrl(S2C_HELLO, string.char(2) .. "10.0.0.1")
  end
end

local function deliver(f)
  local o = HDR
  while o + 2 <= FRAME do
    local ch, len = f[o], f[o + 1]
    if ch == CH_PAD then break end
    if o + 2 + len > FRAME then break end
    if ch == CH_CTRL then
      for i = 0, len - 1 do ctrl_byte(f[o + 2 + i]) end
    else
      gw.upstream[ch] = (gw.upstream[ch] or "")
      for i = 0, len - 1 do
        gw.upstream[ch] = gw.upstream[ch] .. string.char(f[o + 2 + i])
      end
    end
    o = o + 2 + len
  end
end

local function build()
  local f = {}
  for i = 0, FRAME - 1 do f[i] = 0 end
  f[SEQ] = gw.tx_seq
  f[ACK] = (gw.rx_next - 1) & 0xFF
  f[FLAGS] = 0
  local o = HDR
  local any = false
  while #gw.out > 0 and o + 2 < FRAME do
    local item = gw.out[1]
    local room = FRAME - o - 2
    local left = #item.data - item.pos + 1
    local n = (left < room) and left or room
    if n <= 0 then break end
    f[o] = item.ch; f[o + 1] = n
    for i = 0, n - 1 do f[o + 2 + i] = item.data:byte(item.pos + i) end
    o = o + 2 + n
    item.pos = item.pos + n
    if item.pos > #item.data then table.remove(gw.out, 1) end
    any = true
  end
  while o < FRAME do f[o] = CH_PAD; o = o + 1 end
  if any then f[FLAGS] = f[FLAGS] | F_DATA end
  f[SUM] = 0
  f[SUM] = sum_of(f)
  return f, any
end

local function seq_le(a, b) return ((b - a) & 0xFF) < 128 end

local function write_frame(f)
  for i = 0, FRAME - 1 do wb(RXOFF + i, f[i]) end
  wu16(0x04, ru16(0x04) + 1)
end

local function service()
  local ss = ru16(0x02)
  local polled = (ss ~= gw.last_snes_seq)
  if polled then
    gw.last_snes_seq = ss
    local f = {}
    for i = 0, FRAME - 1 do f[i] = rb(TXOFF + i) end
    if f[SUM] == sum_of(f) then
      while #gw.window > 0 and seq_le(gw.window[1].seq, f[ACK]) do
        table.remove(gw.window, 1)
        gw.stall = 0
      end
      if (f[FLAGS] & F_DATA) ~= 0 and f[SEQ] == gw.rx_next then
        deliver(f)
        gw.rx_next = (gw.rx_next + 1) & 0xFF
      end
    end
  end
  if not polled then return end

  local WIN, RESEND_AFTER = 4, 12
  gw.stall = gw.stall + 1
  if #gw.window > 0 and gw.stall > RESEND_AFTER then
    gw.stall = 0
    local head = gw.window[1]
    head.frame[ACK] = (gw.rx_next - 1) & 0xFF
    head.frame[SUM] = 0
    head.frame[SUM] = sum_of(head.frame)
    write_frame(head.frame)
  elseif #gw.window < WIN then
    local f, any = build()
    if any then
      gw.window[#gw.window + 1] = { seq = gw.tx_seq, frame = f }
      gw.tx_seq = (gw.tx_seq + 1) & 0xFF
    end
    write_frame(f)
  else
    local f = {}
    for i = 0, FRAME - 1 do f[i] = 0 end
    f[SEQ] = gw.tx_seq
    f[ACK] = (gw.rx_next - 1) & 0xFF
    f[FLAGS] = 0
    local o = HDR
    while o < FRAME do f[o] = CH_PAD; o = o + 1 end
    f[SUM] = 0
    f[SUM] = sum_of(f)
    write_frame(f)
  end
end

-- ------------------------------------------------------------- IP helpers
-- Packets are Lua strings of raw bytes; offsets below are 1-based.

-- RFC 1071 ones-complement sum over s[first..last] (inclusive, 1-based).
local function csum(s, first, last)
  local sum = 0
  local i = first
  while i + 1 <= last do
    sum = sum + s:byte(i) * 256 + s:byte(i + 1)
    i = i + 2
  end
  if i == last then sum = sum + s:byte(i) * 256 end
  while sum > 0xFFFF do sum = (sum & 0xFFFF) + (sum >> 16) end
  return sum
end

local function be16(v) return string.char((v >> 8) & 0xFF, v & 0xFF) end

-- Compose an ICMP echo request FROM 10.0.2.2 TO 10.0.2.15.
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

local function send_dgram(p)
  push(CH_IP, string.char(#p & 0xFF, (#p >> 8) & 0xFF) .. p)
end

-- Reassembled datagrams FROM the console.
local ipbuf = ""
local reply_ok = nil          -- verdict on the reply to OUR echo request
local reply_err = "no reply seen"
local snes_pings = 0          -- echo requests the console sent US
local ODD_PAYLOAD = "SNOSPING!"     -- 9 bytes: exercises the odd-byte path

local function handle_dgram(p)
  if #p < 20 then return end
  local ihl = (p:byte(1) & 0x0F) * 4
  local proto = p:byte(10)
  local src = table.concat({ p:byte(13, 16) }, ".")
  local dst = table.concat({ p:byte(17, 20) }, ".")
  if proto ~= 1 then return end
  local t = p:byte(ihl + 1)
  if t == 8 and dst == "10.0.2.2" then
    -- The console is pinging us: answer like slirp would.
    snes_pings = snes_pings + 1
    local icmp = p:sub(ihl + 1)
    local rep = string.char(0, 0) .. "\0\0" .. icmp:sub(5)
    local rs = 0xFFFF - csum(rep, 1, #rep)
    rep = rep:sub(1, 2) .. be16(rs) .. rep:sub(5)
    local tot = 20 + #rep
    local ip = string.char(0x45, 0) .. be16(tot) .. be16(0x4321) ..
               be16(0x4000) .. string.char(64, 1) .. "\0\0" ..
               string.char(10, 0, 2, 2) .. string.char(10, 0, 2, 15)
    local ips = 0xFFFF - csum(ip, 1, #ip)
    ip = ip:sub(1, 10) .. be16(ips) .. ip:sub(13)
    send_dgram(ip .. rep)
  elseif t == 0 and reply_ok == nil then
    -- The reply to OUR request: judge every byte of it.
    if csum(p, 1, ihl) ~= 0xFFFF then reply_err = "bad IP checksum"; reply_ok = false; return end
    if src ~= "10.0.2.15" or dst ~= "10.0.2.2" then
      reply_err = "addresses not swapped (" .. src .. "->" .. dst .. ")"; reply_ok = false; return
    end
    if csum(p, ihl + 1, #p) ~= 0xFFFF then reply_err = "bad ICMP checksum"; reply_ok = false; return end
    if p:byte(ihl + 5) ~= 0x77 or p:byte(ihl + 6) ~= 0x11 then
      reply_err = "id not echoed"; reply_ok = false; return
    end
    if p:sub(ihl + 9) ~= ODD_PAYLOAD then
      reply_err = "payload not echoed byte-for-byte"; reply_ok = false; return
    end
    reply_ok = true
  end
end

local function ip_pump()
  ipbuf = ipbuf .. (gw.upstream[CH_IP] or "")
  gw.upstream[CH_IP] = ""
  while #ipbuf >= 2 do
    local len = ipbuf:byte(1) + ipbuf:byte(2) * 256
    if #ipbuf < 2 + len then break end
    handle_dgram(ipbuf:sub(3, 2 + len))
    ipbuf = ipbuf:sub(3 + len)
  end
end

-- -------------------------------------------------------------------- test
local frame, step, at = 0, 1, nil
local claimed = false

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
    wu16(0x00, MAGIC)
    claimed = true
    at = frame + 30
    return
  end
  service()
  ip_pump()

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
    send_dgram(mk_echo_req(0x7711, 1, ODD_PAYLOAD))
    at = frame + 300
    step = 3
    return
  end
  if step == 3 then send("ping 10.0.2.2\n", 700); step = 4; return end
  if step == 4 then send("net status\n", 400); step = 5; return end

  local r = L.new_report("ip")
  local text = L.ring_text()
  print("--- console ---"); print(text); print("---")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.hello_before_typing,
          "the kernel did not HELLO on its own - IP mode needs autoconfig, " ..
          "nobody types `net status` before using the network")
  r.check(reply_ok ~= nil, "no echo reply from the console: " .. reply_err)
  r.check(reply_ok == true, "echo reply wrong: " .. reply_err)
  r.check(snes_pings >= 4,
          "`ping` sent " .. snes_pings .. " echo requests, expected 4")
  r.check(text:find("reply from 10.0.2.2: seq=", 1, true) ~= nil,
          "the shell never printed a ping reply")
  r.check(text:find("timeout", 1, true) == nil,
          "some pings timed out on a perfect link")
  r.check(text:find("ip 10.0.2.15 via 10.0.2.2", 1, true) ~= nil,
          "`net status` does not report IP mode against a v2 gateway")
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

  r.finish()
end, emu.eventType.endFrame)
