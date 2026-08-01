-- mc: a Minecraft server on the console, driven by a scripted 1.16.5 client.
--
-- The bytes themselves were proven by a REAL client against tools/mcprobe.py
-- (M13 step 1), so this test asserts the console's TRANSPORT of them: the
-- status round trip, the login splice, the whole 146 KB join burst arriving
-- as well-formed framed packets over the console's own TCP, keepalives
-- between blobs, and the two live behaviours — chat echo and the dig-revert
-- Block Change. The client here speaks real protocol-754 frames through
-- gwip's TCP peer, dialled in through the port forward like any outside
-- machine.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}

-- ------------------------------------------------- tiny protocol helpers
local function varint(v)
  local out = ""
  repeat
    local b = v & 0x7F
    v = v >> 7
    if v ~= 0 then b = b | 0x80 end
    out = out .. string.char(b)
  until v == 0
  return out
end

local function mcstr(s) return varint(#s) .. s end
local function pkt(id, body) local b = varint(id) .. body; return varint(#b) .. b end

local function read_varint(s, off)
  local v, sh = 0, 0
  for i = 0, 4 do
    local b = s:byte(off + i)
    if b == nil then return nil end
    v = v | ((b & 0x7F) << sh)
    sh = sh + 7
    if (b & 0x80) == 0 then return v, off + i + 1 end
  end
end

-- Walk sock.rx as framed packets; returns list of {id=..,body=..} and the
-- offset of the first incomplete frame (rx is never cleared: offsets hold).
local function walk(rx, from)
  local out, off = {}, from
  while true do
    local len, o2 = read_varint(rx, off)
    if len == nil then return out, off end
    if #rx < o2 + len - 1 then return out, off end
    local id, o3 = read_varint(rx, o2)
    out[#out + 1] = { id = id, body = rx:sub(o3, o2 + len - 1) }
    off = o2 + len
  end
end

-- ------------------------------------------------------------------ test
local frame, step, at = 0, 1, nil
local last_step = nil
local claimed = false
local status_sock, play_sock
local burst_done_at = nil
local counts = {}
local kept = { chat = nil, blockchange = nil }

local DIG_X, DIG_Y, DIG_Z = 2, 3, 18   -- grass, south of the letters
local function pos_pack(x, y, z)
  local v = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
  local out = ""
  for i = 7, 0, -1 do out = out .. string.char((v >> (8 * i)) & 0xFF) end
  return out
end

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL mc - no mailbox magic"); emu.stop(1) end
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
  if step ~= last_step then print("step " .. tostring(step) ..
                                  " @ frame " .. frame); last_step = step end

  -- The burst is ~146 KB over uIP's stop-and-wait: minutes of emulated
  -- frames. Guard generously, like vnc does, and gate on progress instead
  -- of the clock wherever possible.
  if frame > 90000 then
    print("RESULT FAIL mc - wedged at step " .. tostring(step) ..
          " rx=" .. tostring(play_sock and #play_sock.rx))
    emu.stop(1)
    return
  end

  if step == 1 then L.feed("mc\n"); at = frame + 300; step = 2; return end

  if step == 2 then
    if not gw.fwd_port then at = frame + 30; return end
    -- Connection one: a server-list ping.
    status_sock = gw:dial(gw.fwd_port)
    status_sock.on_established = function(t)
      t:send(pkt(0x00, varint(754) .. mcstr("snes") ..
                 string.char(0x63, 0xDD) .. varint(1)))
      t:send(pkt(0x00, ""))
    end
    at = frame + 240
    step = 3
    return
  end

  if step == 3 then
    if status_sock.rx:find("Super Nintendo", 1, true) == nil then
      at = frame + 30; return
    end
    -- Deliberately NOT closed yet: the real client's login SYN arrives
    -- while the ping connection is still up, and with a single listener
    -- that SYN was refused with a RST - Ricardo's first live failure.
    at = frame + 30
    step = 4
    return
  end

  if step == 4 then
    -- Connection two: the join, OVERLAPPING the still-open ping connection.
    play_sock = gw:dial(gw.fwd_port)
    play_sock.on_established = function(t)
      t:send(pkt(0x00, varint(754) .. mcstr("snes") ..
                 string.char(0x63, 0xDD) .. varint(2)))
      t:send(pkt(0x00, mcstr("snesbot")))
    end
    at = frame + 120
    step = 5
    return
  end

  if step == 5 then
    -- The ping connection goes away mid-join, as the real client's does.
    if not status_sock.closing then status_sock:close() end
    -- ...and the client CHATTERS during the burst, as the real one does
    -- (settings, brand, keepalive answers). This is what filled the ring
    -- when the app did not drain between blobs: uIP stopped the window,
    -- the segments went unacked, and the live NAT reset a healthy join at
    -- chunk 13. The drain keeps the ring shallow; this traffic proves it.
    play_sock:send(pkt(0x05, mcstr("en_us") ..
                       string.char(2, 0, 0x7F, 1, 1, 1)))
    -- Progress-gated: done when Position And Look (0x34) has arrived and
    -- every frame before it walks cleanly.
    local pkts = walk(play_sock.rx, 1)
    counts = {}
    for _, p in ipairs(pkts) do counts[p.id] = (counts[p.id] or 0) + 1 end
    if (counts[0x34] or 0) < 1 then at = frame + 60; return end
    burst_done_at = frame
    -- Now poke the live paths: a dig on known grass, and a chat line.
    play_sock:send(pkt(0x1B, varint(0) .. pos_pack(DIG_X, DIG_Y, DIG_Z) ..
                       string.char(1)))
    play_sock:send(pkt(0x03, mcstr("hello from the outside")))
    at = frame + 600
    step = 6
    return
  end

  if step == 6 then
    local pkts = walk(play_sock.rx, 1)
    for _, p in ipairs(pkts) do
      if p.id == 0x0B then kept.blockchange = p.body end
      if p.id == 0x0E and p.body:find("outside", 1, true) then
        kept.chat = p.body
      end
    end
    if kept.blockchange == nil or kept.chat == nil then
      at = frame + 60; return
    end
    -- Wait past one keepalive interval to see it fire post-burst.
    at = frame + 700
    step = 7
    return
  end

  if step == 7 then L.feed(" "); at = frame + 300; step = 8; return end

  local r = L.new_report("mc")
  local text = L.ring_text()
  print("--- console ---"); print(text); print("---")
  local ids = {}
  for id, n in pairs(counts) do ids[#ids + 1] = string.format("%02x:%d", id, n) end
  table.sort(ids)
  print("clientbound: " .. table.concat(ids, " "))

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(gw.tcp_bad_csum == 0,
          "console sent " .. gw.tcp_bad_csum .. " bad TCP checksums")
  r.check(gw.fwd_port == 25565, "mc did not forward port 25565")

  -- Status connection: the baked JSON, served whole.
  r.check(status_sock.rx:find('"protocol":754', 1, true) ~= nil,
          "status JSON did not carry the protocol version")

  -- Login: success packet first, name echoed, then the baked burst intact.
  local pkts = walk(play_sock.rx, 1)
  r.check(#pkts >= 55, "only " .. #pkts .. " packets arrived")
  r.check(pkts[1].id == 0x02 and pkts[1].body:find("snesbot", 1, true),
          "Login Success with the spliced name was not first")
  r.check(pkts[2].id == 0x24, "Join Game did not follow Login Success")
  r.check((counts[0x20] or 0) == 25,
          "expected 25 chunk packets, got " .. tostring(counts[0x20]))
  r.check((counts[0x23] or 0) == 25,
          "expected 25 light packets, got " .. tostring(counts[0x23]))
  r.check((counts[0x1F] or 0) >= 1,
          "no keepalive between the burst's blobs")
  r.check((counts[0x42] or 0) == 1 and (counts[0x34] or 0) == 1,
          "spawn position / position-and-look missing")

  -- The world is read-only: the dig came back as the original grass.
  r.check(kept.blockchange ~= nil and
          kept.blockchange:byte(#kept.blockchange) == 9,
          "dig was not reverted to grass (state 9)")
  r.check(kept.chat ~= nil, "chat was not echoed")
  r.check(text:find("<snesbot> hello from the outside", 1, true) ~= nil,
          "chat did not reach the console screen")
  r.check(text:find("dig - put it back", 1, true) ~= nil,
          "the dig was not reported on the console")
  r.check(text:find("snesbot joined the world", 1, true) ~= nil,
          "the join was not reported on the console")
  print(string.format("burst complete at frame %d (%d bytes)",
        burst_done_at or -1, #play_sock.rx))

  r.finish()
end, emu.eventType.endFrame)
