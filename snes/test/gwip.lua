-- gwip.lua — the hermetic slirp box: a v2 gateway plus a real IP/TCP/UDP/ICMP
-- peer, in Lua, for the Mesen tests.
--
-- One copy, shared by every network test since the uIP cutover (the
-- "consolidating the Lua gateway copies" note in ip.lua, executed). It speaks
-- the link protocol frame for frame — same sequence rules, same checksum,
-- same stop-and-wait the real cable will use — and above that it is a
-- genuine, independent TCP/DNS/ICMP implementation: checksums are computed
-- and VERIFIED here, in Lua, so the console's stack and this peer cannot
-- share a misunderstanding about the wire format. tools/slirp.py is
-- deliberately not reused for the same reason.
--
-- What a test gets from G.new{...}:
--
--   transport   gw:service() once per frame after claiming the mailbox;
--               fault injection via gw.drop_snes / gw.drop_out /
--               gw.corrupt_out (counts of frames to lose or damage)
--   control     HELLO answered with version 2 + gw.addr; IPFWD tracked in
--               gw.fwd_port / gw.fwd_ch
--   ICMP        echo requests answered (any destination, like a real slirp
--               box); gw.snes_pings counts them
--   DNS         gw.dns = { ["name"] = {a,b,c,d} }; others NXDOMAIN;
--               gw.dns_queries counts
--   TCP         gw.on_connect(sock, port) fires when the console dials OUT;
--               gw:dial(port) opens a connection INTO the console (the
--               daemon's half of a port forward, in miniature).
--               A sock: :send(s) queues bytes (segmented, windowed,
--               retransmitted), :close() FINs after the queue drains,
--               sock.rx is everything received, sock.fin_rcvd/.reset/.done.
--   observer    gw.observer(pkt) sees every datagram from the console before
--               default handling; return true to consume it.
--   counters    gw.tcp_syns, gw.first_syn_frame, gw.tcp_bad_csum,
--               gw.hello_seen
--
-- Offsets into Lua strings are 1-based; frame tables are 0-based like the
-- mailbox. That split is inherited from ip.lua and kept: the frame code was
-- debugged against the mailbox, the packet code against RFC diagrams.
local G = {}

local MB = 0xE000          -- link mailbox, WRAM $7EE000
local MAGIC = 0x4B4C       -- "LK"
local FRAME, HDR = 128, 4
local TXOFF, RXOFF = 0x10, 0x10 + FRAME
local SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
local F_DATA = 0x01
local CH_CTRL, CH_PAD, CH_IP = 0, 0xFF, 0x10
local WRAM = emu.memType.snesWorkRam

local C2S_HELLO = 0x04
local C2S_IPFWD, C2S_IPUNFWD = 0x0B, 0x0C
local S2C_ERR, S2C_HELLO = 0x83, 0x84

local SNES_IP = "10.0.2.15"
local MSS = 536

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

local function be32(v)
  return string.char((v >> 24) & 0xFF, (v >> 16) & 0xFF,
                     (v >> 8) & 0xFF, v & 0xFF)
end

local function quad(s)
  local a, b, c, d = s:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
  return string.char(tonumber(a), tonumber(b), tonumber(c), tonumber(d))
end

-- An IPv4 header around `payload`, proto given, src -> dst.
local function mk_ip(src, dst, proto, payload)
  local tot = 20 + #payload
  local ip = string.char(0x45, 0) .. be16(tot) .. be16(0x4242) ..
             be16(0x4000) .. string.char(64, proto) .. "\0\0" ..
             quad(src) .. quad(dst)
  local ips = 0xFFFF - csum(ip, 1, #ip)
  ip = ip:sub(1, 10) .. be16(ips) .. ip:sub(13)
  return ip .. payload
end

function G.new(opt)
  opt = opt or {}
  local gw = {
    -- transport
    tx_seq = 0, rx_next = 0, last_snes_seq = 0, out = {},
    cmsg = { state = 0, typ = 0, len = 0, buf = {} },
    window = {}, stall = 0,
    drop_snes = 0, drop_out = 0, corrupt_out = 0,
    frame = 0,
    -- control
    addr = opt.addr or "10.0.0.1",
    hello_seen = false,
    fwd_port = nil, fwd_ch = nil, unfwds = 0,
    -- IP layer
    ipbuf = "",
    dns = opt.dns or {},
    dns_queries = 0,
    snes_pings = 0,
    socks = {},                -- key "sport:dport" -> sock
    on_connect = opt.on_connect,
    observer = nil,
    tcp_syns = 0,
    first_syn_frame = nil,
    tcp_bad_csum = 0,
    dial_n = 0,
  }

  -- ------------------------------------------------------------ transport
  local function push(ch, s)
    gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 }
  end
  gw.push = push

  local function push_ctrl(typ, payload)
    push(CH_CTRL, string.char(typ, #payload) .. payload)
  end

  local function send_dgram(p)
    push(CH_IP, string.char(#p & 0xFF, (#p >> 8) & 0xFF) .. p)
  end
  gw.send_dgram = send_dgram

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
      if gw.on_hello then gw.on_hello() end
      -- Version 2: "I am a slirp box." This one byte arms the console's
      -- whole IP layer, and is the only kind of daemon it works with now.
      push_ctrl(S2C_HELLO, string.char(2) .. gw.addr)
    elseif c.typ == C2S_IPFWD and #c.buf >= 3 then
      -- The console asked us to hold a real socket for it. This script IS
      -- the outside world, so recording the port is holding the socket.
      gw.fwd_ch = c.buf[1]
      gw.fwd_port = c.buf[2] | (c.buf[3] << 8)
      if gw.fwd_refuse then
        push_ctrl(S2C_ERR, string.char(gw.fwd_ch, 1))
      end
    elseif c.typ == C2S_IPUNFWD and #c.buf >= 3 then
      gw.unfwds = gw.unfwds + 1
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
      elseif ch == CH_IP then
        local s = {}
        for i = 0, len - 1 do s[#s + 1] = string.char(f[o + 2 + i]) end
        gw.ipbuf = gw.ipbuf .. table.concat(s)
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
    -- Loss toward the SNES: the exchange still happened, the frame is gone.
    if gw.drop_out > 0 then
      gw.drop_out = gw.drop_out - 1
      wu16(0x04, ru16(0x04) + 1)
      return
    end
    local corrupt = gw.corrupt_out > 0
    if corrupt then gw.corrupt_out = gw.corrupt_out - 1 end
    for i = 0, FRAME - 1 do
      local v = f[i]
      if corrupt and i == SUM then v = (v + 1) & 0xFF end
      wb(RXOFF + i, v)
    end
    wu16(0x04, ru16(0x04) + 1)
  end

  -- ------------------------------------------------------------------ TCP
  -- A real peer for the console's own stack: handshake, sequence
  -- arithmetic, checksum VERIFICATION, windowed stop-and-wait sending with
  -- retransmission, FIN both ways. uIP handles one segment at a time, so
  -- one segment outstanding is the fast path, not a shortcut (the same
  -- reasoning as tools/slirp.py, arrived at independently).
  local function tcp_out(t, flags, payload)
    payload = payload or ""
    local hdr = be16(t.peer_port) .. be16(t.snes_port) .. be32(t.snd) ..
                be32(t.rcv) .. string.char(0x50, flags) .. be16(2048) ..
                "\0\0" .. "\0\0"
    local pseudo = quad(t.peer_ip) .. quad(SNES_IP) .. "\0\6" ..
                   be16(#hdr + #payload)
    local c = 0xFFFF - csum(pseudo .. hdr .. payload, 1,
                            #pseudo + #hdr + #payload)
    if c == 0 then c = 0xFFFF end
    local seg = hdr:sub(1, 16) .. be16(c) .. hdr:sub(19) .. payload
    send_dgram(mk_ip(t.peer_ip, SNES_IP, 6, seg))
  end

  local sockmt = {}
  sockmt.__index = sockmt

  function sockmt:send(s)
    self.txq = self.txq .. s
  end

  function sockmt:close()
    self.closing = true
  end

  -- Advance the send side: (re)transmit at most one segment. Called on every
  -- ack from the console and once per poll (the stall path).
  function sockmt:pump()
    if self.done or not self.established then return end
    if self.unacked ~= "" then
      self.stall = self.stall + 1
      if self.stall > 60 then           -- ~1 s of polls: retransmit
        self.stall = 0
        self.rexmits = (self.rexmits or 0) + 1
        -- Bounded patience, mimicking the real daemon: a console whose
        -- window stays shut for this long reads as dead, and giving up
        -- HERE is what lets a test catch an app that stops draining (the
        -- mc join burst did exactly that, and only the live NAT noticed).
        if self.rexmits > 100 then
          self.reset = true
          self.done = true
          return
        end
        tcp_out(self, 0x18, self.unacked)
      end
      return
    end
    if #self.txq > 0 then
      local wnd = self.wnd or 2048
      if wnd <= 0 then return end       -- closed window: wait for the update
      local n = #self.txq
      if n > MSS then n = MSS end
      if n > wnd then n = wnd end
      self.unacked = self.txq:sub(1, n)
      self.txq = self.txq:sub(n + 1)
      self.stall = 0
      tcp_out(self, 0x18, self.unacked)                -- PSH|ACK
    elseif self.closing and not self.fin_sent then
      self.fin_sent = true
      tcp_out(self, 0x11)                              -- FIN|ACK
      self.snd = (self.snd + 1) & 0xFFFFFFFF
    end
  end

  local function sock_new(sport, dport, peer_ip, iss)
    local t = setmetatable({
      snes_port = sport, peer_port = dport, peer_ip = peer_ip,
      snd = iss, rcv = 0, wnd = 2048,
      rx = "", txq = "", unacked = "", stall = 0,
      established = false, closing = false, fin_sent = false,
      fin_rcvd = false, reset = false, done = false,
    }, sockmt)
    gw.socks[sport .. ":" .. dport] = t
    return t
  end

  -- Dial the console: the daemon's half of a port forward, in miniature.
  -- The console is the SERVER, so its port is the well-known one and ours
  -- is ephemeral — the reverse of every other connection here.
  function gw:dial(port, peer_ip)
    gw.dial_n = gw.dial_n + 1
    local t = sock_new(port, 40000 + gw.dial_n, peer_ip or "10.0.2.2",
                       0x7000 + gw.dial_n * 0x1000)
    t.dialed = true
    tcp_out(t, 0x02)                                   -- SYN
    t.snd = (t.snd + 1) & 0xFFFFFFFF
    return t
  end

  local function handle_tcp(p, ihl, dst)
    local o = ihl + 1
    local sport = p:byte(o) * 256 + p:byte(o + 1)
    local dport = p:byte(o + 2) * 256 + p:byte(o + 3)
    local seq = 0
    for i = 0, 3 do seq = seq * 256 + p:byte(o + 4 + i) end
    local ack = 0
    for i = 0, 3 do ack = ack * 256 + p:byte(o + 8 + i) end
    local doff = (p:byte(o + 12) >> 4) * 4
    local flags = p:byte(o + 13)
    local wnd = p:byte(o + 14) * 256 + p:byte(o + 15)
    local data = p:sub(o + doff)
    -- Verify the console's checksum the way a real peer does: a stack that
    -- computes it wrongly must fail here, not be quietly accepted.
    local pseudo = quad(SNES_IP) .. quad(dst) .. "\0\6" .. be16(#p - ihl)
    if csum(pseudo .. p:sub(ihl + 1), 1, #pseudo + #p - ihl) ~= 0xFFFF then
      gw.tcp_bad_csum = gw.tcp_bad_csum + 1
      return
    end

    local t = gw.socks[sport .. ":" .. dport]

    if (flags & 0x02) ~= 0 and t == nil then           -- SYN: console dials out
      gw.tcp_syns = gw.tcp_syns + 1
      if gw.first_syn_frame == nil then gw.first_syn_frame = gw.frame end
      t = sock_new(sport, dport, dst, 0x5000 + gw.tcp_syns * 0x100)
      t.rcv = (seq + 1) & 0xFFFFFFFF
      t.established = true
      t.wnd = wnd
      tcp_out(t, 0x12)                                 -- SYN|ACK
      t.snd = (t.snd + 1) & 0xFFFFFFFF
      if gw.on_connect then gw.on_connect(t, dport) end
      return
    end
    if t == nil then
      -- No such connection. A RST here would be the polite thing; silence is
      -- indistinguishable for the tests, which never probe closed ports.
      return
    end
    t.wnd = wnd

    if t.reset then return end
    if (flags & 0x04) ~= 0 then                        -- RST from the console
      t.reset = true
      t.done = true
      return
    end

    -- Our dial answered: SYN|ACK from the console's listener.
    if t.dialed and not t.established then
      if (flags & 0x12) == 0x12 then
        t.rcv = (seq + 1) & 0xFFFFFFFF
        t.established = true
        tcp_out(t, 0x10)                               -- ACK completes it
        if t.on_established then t:on_established() end
      end
      return
    end

    -- Data, in order or a retransmission.
    if #data > 0 then
      if seq == t.rcv then
        t.rcv = (t.rcv + #data) & 0xFFFFFFFF
        t.rx = t.rx .. data
        tcp_out(t, 0x10)                               -- ACK
      else
        tcp_out(t, 0x10)                               -- re-ack a retransmit
      end
    end

    -- Their ack for our outstanding segment (or our FIN).
    if (flags & 0x10) ~= 0 then
      if t.unacked ~= "" then
        local edge = (t.snd + #t.unacked) & 0xFFFFFFFF
        if ack == edge then
          t.snd = edge
          t.unacked = ""
          t.stall = 0
          t.rexmits = 0
        end
      elseif t.fin_sent and not t.fin_acked and ack == t.snd then
        t.fin_acked = true
        if t.fin_rcvd then t.done = true end
      end
    end

    if (flags & 0x01) ~= 0 then                        -- FIN from the console
      local fs = (seq + #data) & 0xFFFFFFFF
      if fs == t.rcv then
        t.rcv = (t.rcv + 1) & 0xFFFFFFFF
        t.fin_rcvd = true
        tcp_out(t, 0x10)
        if t.fin_sent and t.fin_acked then t.done = true end
        if t.on_fin then t:on_fin() end
      end
    end

    -- An ack (or window update) may have made sending possible: try now
    -- rather than a poll later.
    t:pump()
  end

  -- ------------------------------------------------------------------ DNS
  -- Answers one table of names and NXDOMAINs everything else, which is what
  -- makes "cannot resolve" testable. The A record sits behind a compression
  -- pointer on purpose: the console must step over it without following it.
  local function dns_answer(q)
    if #q < 12 then return nil end
    local qd = q:byte(5) * 256 + q:byte(6)
    if qd < 1 then return nil end
    local o, labels = 13, {}
    while o <= #q do
      local n = q:byte(o)
      if n == 0 then o = o + 1; break end
      if n >= 0xC0 then return nil end   -- no compression in a question
      labels[#labels + 1] = q:sub(o + 1, o + n)
      o = o + 1 + n
    end
    local qname_end = o + 4              -- QTYPE + QCLASS
    if qname_end - 1 > #q then return nil end
    local name = table.concat(labels, ".")
    gw.dns_queries = gw.dns_queries + 1

    local hdr = q:sub(1, 2)              -- the id, echoed
    local question = q:sub(13, qname_end - 1)
    local addr = gw.dns[name]
    if not addr then
      -- NXDOMAIN: flags QR|RD|RA|rcode3, no answers.
      return hdr .. string.char(0x81, 0x83) .. be16(1) .. be16(0) ..
             be16(0) .. be16(0) .. question
    end
    local rr = string.char(0xC0, 0x0C) ..
               be16(1) .. be16(1) ..
               string.char(0, 0, 0, 60) ..
               be16(4) .. string.char(table.unpack(addr))
    return hdr .. string.char(0x81, 0x80) .. be16(1) .. be16(1) ..
           be16(0) .. be16(0) .. question .. rr
  end

  local function handle_udp(p, ihl, src, dst)
    local u = p:sub(ihl + 1)
    if #u < 8 then return end
    local sport = u:byte(1) * 256 + u:byte(2)
    local dport = u:byte(3) * 256 + u:byte(4)
    local ulen = u:byte(5) * 256 + u:byte(6)
    if dport ~= 53 or dst ~= "10.0.2.3" then return end
    local reply = dns_answer(u:sub(9, ulen))
    if reply == nil then return end
    local ln = 8 + #reply
    local uh = be16(53) .. be16(sport) .. be16(ln) .. "\0\0"
    -- The pseudo-header is part of the UDP checksum; getting it wrong here
    -- would look exactly like the console mis-parsing the answer.
    local pseudo = quad("10.0.2.3") .. quad(SNES_IP) ..
                   string.char(0, 17) .. be16(ln)
    local c = 0xFFFF - csum(pseudo .. uh .. reply, 1, #pseudo + #uh + #reply)
    if c == 0 then c = 0xFFFF end
    local udp = uh:sub(1, 6) .. be16(c) .. reply
    send_dgram(mk_ip("10.0.2.3", SNES_IP, 17, udp))
  end

  -- ----------------------------------------------------------------- ICMP
  local function handle_icmp(p, ihl)
    local t = p:byte(ihl + 1)
    if t ~= 8 then return end
    -- Answer echo requests to ANY address: a slirp box proxies a ping
    -- onward and the reply comes back wearing the address that was pinged.
    gw.snes_pings = gw.snes_pings + 1
    local icmp = p:sub(ihl + 1)
    local rep = string.char(0, 0) .. "\0\0" .. icmp:sub(5)
    local rs = 0xFFFF - csum(rep, 1, #rep)
    rep = rep:sub(1, 2) .. be16(rs) .. rep:sub(5)
    local dst = table.concat({ p:byte(17, 20) }, ".")
    send_dgram(mk_ip(dst, SNES_IP, 1, rep))
  end

  local function handle_dgram(p)
    if gw.observer and gw.observer(p) then return end
    if #p < 20 then return end
    local ihl = (p:byte(1) & 0x0F) * 4
    local proto = p:byte(10)
    local src = table.concat({ p:byte(13, 16) }, ".")
    local dst = table.concat({ p:byte(17, 20) }, ".")
    if proto == 17 then handle_udp(p, ihl, src, dst)
    elseif proto == 6 then handle_tcp(p, ihl, dst)
    elseif proto == 1 then handle_icmp(p, ihl) end
  end

  local function ip_pump()
    while #gw.ipbuf >= 2 do
      local len = gw.ipbuf:byte(1) + gw.ipbuf:byte(2) * 256
      if #gw.ipbuf < 2 + len then break end
      local pkt = gw.ipbuf:sub(3, 2 + len)
      gw.ipbuf = gw.ipbuf:sub(3 + len)
      handle_dgram(pkt)
    end
  end

  -- --------------------------------------------------------------- service
  function gw:claim()
    wu16(0x00, MAGIC)
  end

  function gw:service()
    gw.frame = gw.frame + 1
    local ss = ru16(0x02)
    local polled = (ss ~= gw.last_snes_seq)
    if polled then
      gw.last_snes_seq = ss
      if gw.drop_snes > 0 then
        gw.drop_snes = gw.drop_snes - 1  -- pretend the frame never arrived
      else
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
    end
    ip_pump()
    for _, t in pairs(gw.socks) do t:pump() end
    if not polled then return end

    -- A window rather than stop-and-wait, go-back-N on a gap: the SNES takes
    -- frames strictly in order and re-states a cumulative ack on every frame,
    -- so only this side had to change to pipeline. RESEND_AFTER must exceed
    -- window depth + round trip or normal pipelining looks like a stall.
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
      -- Window full and nothing due for resend: still answer the exchange so
      -- the SNES keeps receiving our ack.
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

  return gw
end

-- Also exported for tests that compose their own packets (ip.lua's verdicts).
G.csum, G.be16, G.be32, G.quad, G.mk_ip, G.MSS = csum, be16, be32, quad, mk_ip, MSS

return G
