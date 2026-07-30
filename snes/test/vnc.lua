-- Regression test for `vnc`: a Lua RFB SERVER behind the Lua gateway.
--
-- test/net.lua implements the gateway; this implements what is on the far side
-- of it, so the console runs the real RFB protocol — version handshake, security
-- type, SetPixelFormat, SetEncodings, FramebufferUpdate, PointerEvent — against
-- something that answers frame for frame. Nothing is stubbed below the app.
--
-- It exists because two bugs Ricardo reported were both invisible to every other
-- kind of check, and both are asserted here:
--
--   A. THE SCREEN ALTERNATED BETWEEN THE PICTURE AND BLACK. flush_dirty() held
--      forced blank across a whole rectangle's repaint, and it repaints once per
--      received rectangle — measured, six episodes of 207 frames each with the
--      WHOLE screen dark. So the test does not count blanks (the fix blanks
--      deliberately, several times a second); it checks WHERE each one falls.
--      Every forced blank must open and close between scanlines 192 and 224,
--      below the picture, inside one frame.
--
--   B. THE CURSOR DID NOT MOVE. OAM writes during active display do not go where
--      OAMADDR points, so place_cursor() did nothing at all. Both input paths
--      are driven — a key through the mailbox, exactly as tools/kbdbridge.lua
--      pushes it, and the controller d-pad through emu.setInput — and the sprite
--      is read back out of OAM.
--
-- Space sends a PointerEvent at the cursor, so the coordinate the SERVER
-- receives is the app's own idea of where the cursor is. That is what separates
-- "the keystroke was lost" from "the keystroke arrived and only the OAM write
-- went nowhere", and it is what identified bug B in a single run.
--
-- The picture itself is checked by FOLLOWING THE TILEMAP, exactly as the PPU
-- does. Decoding tile data in assumed order bypasses the tilemap, which is where
-- the previous display bug actually was, and reports "correct" for a screen that
-- is garbage.
--
-- NOT part of `make test`: it is a two-minute run on its own, against an app
-- with no other coverage. `make test-vnc`.
local L = dofile("test/lib.lua")

local MB = 0xE000          -- link mailbox, WRAM $7EE000
local MAGIC = 0x4B4C       -- "LK"
local FRAME, HDR = 128, 4
-- Derived, not literals -- see the LINK_MB_RX note in kernel/link.h.
local TXOFF, RXOFF = 0x10, 0x10 + FRAME
local SEQ, ACK, FLAGS, SUM = 0, 1, 2, 3
local F_DATA = 0x01
local CH_CTRL, CH_PAD = 0, 0xFF
local WRAM = emu.memType.snesWorkRam

local C2S_CONNECT, C2S_HELLO, C2S_CLOSE, C2S_HANGUP = 0x07, 0x04, 0x02, 0x09
local S2C_HELLO, S2C_OPEN, S2C_EOF = 0x84, 0x81, 0x82

local function rb(o) return emu.read(MB + o, WRAM) end
local function wb(o, v) emu.write(MB + o, v & 0xFF, WRAM) end
local function ru16(o) return emu.read16(MB + o, WRAM) end
local function wu16(o, v) emu.write16(MB + o, v & 0xFFFF, WRAM) end

local function sum_of(f)
  local s = 0
  for i = 0, FRAME - 1 do if i ~= SUM then s = s + f[i] end end
  return s & 0xFF
end

local gw = {
  tx_seq = 0, rx_next = 0, last_snes_seq = 0, out = {},
  cmsg = { state = 0, typ = 0, len = 0, buf = {} },
  window = {}, stall = 0, connects = {},
}

local function push(ch, s) gw.out[#gw.out + 1] = { ch = ch, data = s, pos = 1 } end
local function push_ctrl(typ, payload)
  push(CH_CTRL, string.char(typ, #payload) .. payload)
end

-- ------------------------------------------------------------------- RFB
-- The framebuffer the server pretends to own. p(x,y) = 5x + 11y, so no tile
-- written to the wrong place can accidentally look right.
local FB_W, FB_H = 256, 192
local function px(x, y) return (x * 5 + y * 11) % 256 end
local fbrow = {}
for y = 0, FB_H - 1 do
  local t = {}
  for x = 0, FB_W - 1 do t[x + 1] = string.char(px(x, y)) end
  fbrow[y + 1] = table.concat(t)
end
local function band(y0, h) return table.concat(fbrow, "", y0 + 1, y0 + h) end

local function be16(v) return string.char((v >> 8) & 0xFF, v & 0xFF) end
local function be32(v)
  return string.char((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
end

local rfb = { ch = nil, inbuf = "", requests = 0, pointers = {}, co = nil,
              done_first = false }

local function rfb_send(s) push(rfb.ch, s) end

local function rect(x, y, w, h, data)
  return be16(x) .. be16(y) .. be16(w) .. be16(h) .. be32(0) .. data
end

-- A coroutine, because RFB is a framed byte stream and "wait for n more bytes"
-- is the whole of the parsing problem. rd() yields until they have arrived.
local function rfb_start()
  rfb.co = coroutine.create(function()
    local function rd(n)
      while #rfb.inbuf < n do coroutine.yield() end
      local s = rfb.inbuf:sub(1, n)
      rfb.inbuf = rfb.inbuf:sub(n + 1)
      return s
    end

    rfb_send("RFB 003.008\n")
    local ver = rd(12)
    if ver:sub(1, 3) ~= "RFB" then error("client version was " .. ver) end
    rfb_send(string.char(1, 1))              -- one security type: None
    rd(1)                                    -- the client's choice
    rfb_send(string.char(0, 0, 0, 0))        -- SecurityResult: OK
    rd(1)                                    -- ClientInit
    local name = "snoslua"
    rfb_send(be16(FB_W) .. be16(FB_H) ..
             string.char(8, 8, 0, 1) .. be16(255) .. be16(255) .. be16(255) ..
             string.char(0, 0, 0, 0, 0, 0) ..
             be32(#name) .. name)

    rfb.pf = rd(20)                          -- SetPixelFormat
    rfb.se = rd(12)                          -- SetEncodings

    while true do
      local t = rd(1):byte(1)
      if t == 3 then                         -- FramebufferUpdateRequest
        rd(9)
        rfb.requests = rfb.requests + 1
        if rfb.requests == 1 then
          -- Six 256x32 bands in ONE update message. A real server does split a
          -- screen, and the app repaints after every rectangle — which is what
          -- made the old forced blank flash six times rather than once.
          local parts = { string.char(0, 0) .. be16(6) }
          for i = 0, 5 do
            parts[#parts + 1] = rect(0, i * 32, 256, 32, band(i * 32, 32))
          end
          rfb_send(table.concat(parts))
        elseif rfb.requests == 2 then
          -- A still screen sends nothing at all, which is what RFB does and what
          -- leaves the app waiting in rd() with the whole picture painted.
          rfb.done_first = true
        end
      elseif t == 5 then                     -- PointerEvent
        local p = rd(5)
        rfb.pointers[#rfb.pointers + 1] =
          { mask = p:byte(1), x = (p:byte(2) << 8) | p:byte(3),
            y = (p:byte(4) << 8) | p:byte(5) }
      elseif t == 4 then rd(7)               -- KeyEvent
      elseif t == 2 then                     -- SetEncodings
        local h = rd(3)
        rd(4 * ((h:byte(2) << 8) | h:byte(3)))
      elseif t == 0 then rd(19)              -- SetPixelFormat
      else error(string.format("unknown client message %d", t))
      end
    end
  end)
  local ok, err = coroutine.resume(rfb.co)
  if not ok then print("rfb error: " .. tostring(err)) end
end

local function rfb_feed(s)
  rfb.inbuf = rfb.inbuf .. s
  if rfb.co and coroutine.status(rfb.co) == "suspended" then
    local ok, err = coroutine.resume(rfb.co)
    if not ok then print("rfb error: " .. tostring(err)) end
  end
end

-- ---------------------------------------------------------------- gateway
-- The link half, from test/net.lua, with the fault injection removed and
-- C2S_CONNECT — a plain TCP stream — added, since that is the one `vnc` uses.
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
  if c.dispatch then
    c.dispatch = false
    if c.typ == C2S_HELLO then
      push_ctrl(S2C_HELLO, string.char(1) .. "10.0.0.1")
    elseif c.typ == C2S_CONNECT and #c.buf >= 1 then
      local ch = c.buf[1]
      local host = ""
      for i = 2, #c.buf do host = host .. string.char(c.buf[i]) end
      gw.connects[#gw.connects + 1] = host
      rfb.ch = ch
      push_ctrl(S2C_OPEN, string.char(ch, 0, 0, 0))
      rfb_start()
    elseif (c.typ == C2S_CLOSE or c.typ == C2S_HANGUP) and #c.buf >= 1 then
      local ch = c.buf[1]
      local i = 1
      while i <= #gw.out do
        if gw.out[i].ch == ch then table.remove(gw.out, i) else i = i + 1 end
      end
    end
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
    elseif ch == rfb.ch then
      local t = {}
      for i = 0, len - 1 do t[#t + 1] = string.char(f[o + 2 + i]) end
      rfb_feed(table.concat(t))
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
  local o, any = HDR, false
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
        table.remove(gw.window, 1); gw.stall = 0
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

-- ------------------------------------------------------- instrumentation
local frame = 0

-- Where does each forced blank fall? See bug A at the top. The two blanks before
-- the app's display comes on — boot's, and the one vnc holds over its own PPU
-- setup — are legitimately full-screen, so checking starts after those.
local STRIP_TOP, STRIP_END = FB_H, 225
local ini = { last = nil, episodes = 0, releases = 0, armed = false,
              checked = 0, bad = 0, worst = -1, open_sl = nil, open_f = nil,
              first_bad = nil, log = {} }
emu.addMemoryCallback(function(_, value)
  local ok, st = pcall(emu.getState)
  local sl = (ok and st and st["ppu.scanline"]) or -1
  if ini.last == value then return end
  if value >= 0x80 then
    ini.episodes = ini.episodes + 1
    ini.open_sl, ini.open_f = sl, frame
  else
    ini.releases = ini.releases + 1
    if ini.armed and ini.open_sl then
      ini.checked = ini.checked + 1
      if sl > ini.worst then ini.worst = sl end
      if not (ini.open_sl >= STRIP_TOP and ini.open_sl < STRIP_END and
              sl >= STRIP_TOP and sl < STRIP_END and frame == ini.open_f) then
        ini.bad = ini.bad + 1
        ini.first_bad = ini.first_bad or string.format(
          "frame %d..%d, scanline %d..%d", ini.open_f, frame, ini.open_sl, sl)
      end
    end
    if ini.releases >= 2 then ini.armed = true end
    ini.open_sl = nil
  end
  if #ini.log < 16 then
    ini.log[#ini.log + 1] = string.format("f%d/L%d<-%02X", frame, sl, value)
  end
  ini.last = value
end, emu.callbackType.write, 0x2100, 0x2100, emu.cpuType.snes)

-- Every DMA the app starts, by scanline. This is the measurement that killed the
-- first attempt at fixing bug A: it waited for vblank, and the histogram showed
-- every transfer landing at scanline 104-122 instead, because NMI runs
-- net_service from the top of vblank until about scanline 100 of the next frame
-- whenever an application owns the PPU.
local dma = { total = 0, sl = {} }
emu.addMemoryCallback(function()
  dma.total = dma.total + 1
  local ok, st = pcall(emu.getState)
  if ok and st then
    local sl = st["ppu.scanline"] or -1
    dma.sl[sl] = (dma.sl[sl] or 0) + 1
  end
end, emu.callbackType.write, 0x420B, 0x420B, emu.cpuType.snes)

local function dma_span()
  local lo, hi, out = 999, -1, 0
  for k, n in pairs(dma.sl) do
    if k < lo then lo = k end
    if k > hi then hi = k end
    if k < STRIP_TOP then out = out + n end
  end
  return lo, hi, out
end

-- ------------------------------------------------------------- decoding
local VRAM = emu.memType.snesVideoRam
local OAM = emu.memType.snesSpriteRam
local MAPBASE = 0xE000        -- BG1 tilemap, BYTE address

-- Follow the tilemap the way the PPU does: 32x32 entries of two bytes, tile
-- index = entry & 0x3FF, tile data 64 bytes per tile from VRAM 0, 8bpp as four
-- pairs of bitplanes.
local function check_picture()
  local bad, first = 0, nil
  for ty = 0, (FB_H / 8) - 1 do
    for tx = 0, 31 do
      local entry = emu.read16(MAPBASE + (ty * 32 + tx) * 2, VRAM)
      local base = (entry & 0x3FF) * 64
      for row = 0, 7 do
        local p = {}
        for pl = 0, 7 do
          p[pl] = emu.read(base + (pl >> 1) * 16 + row * 2 + (pl & 1), VRAM)
        end
        for col = 0, 7 do
          local bit, v = 0x80 >> col, 0
          for pl = 0, 7 do
            if (p[pl] & bit) ~= 0 then v = v | (1 << pl) end
          end
          local want = px(tx * 8 + col, ty * 8 + row)
          if v ~= want then
            bad = bad + 1
            first = first or string.format(
              "tile(%d,%d) entry=%04X pixel(%d,%d) is %d, want %d",
              tx, ty, entry, tx * 8 + col, ty * 8 + row, v, want)
          end
        end
      end
    end
  end
  return bad, first
end

local function oam(i) return emu.read(i, OAM) end

-- ------------------------------------------------------------------ test
local r = L.new_report("vnc")
local step, at, claimed = 1, nil, false
local painted_at, cursor0, after_key, after_pad = nil, nil, nil, nil
local pad = nil

emu.addEventCallback(function()
  if pad then emu.setInput(pad, 0) end
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL vnc - no mailbox magic"); emu.stop(1) end
    return
  end
  -- Claim the link only once the kernel has zeroed the mailbox, or net_init()
  -- wipes the magic out from under us.
  if not claimed then
    if frame < 200 then return end
    wu16(0x00, MAGIC)
    claimed = true
    at = frame + 60
    return
  end

  service()

  if frame > 30000 then
    print("ring: " .. L.ring_text())
    print("RESULT FAIL vnc - timed out at step " .. tostring(step))
    emu.stop(1)
    return
  end

  if at and frame < at then return end

  if step == 1 then L.feed("vnc 10.0.0.2\n"); step = 2; at = nil; return end

  -- The app asks a SECOND time only once it has painted everything the first
  -- answer contained, so this is "the first screen is on screen".
  if step == 2 then
    if not rfb.done_first then return end
    painted_at = frame
    step = 3
    at = frame + 60
    return
  end

  if step == 3 then
    local bad, first = check_picture()
    print(string.format("picture: %d wrong pixels of %d", bad, FB_W * FB_H))
    r.check(bad == 0, string.format(
      "%d pixels wrong through the tilemap; first: %s", bad, tostring(first)))
    r.check(#gw.connects == 1 and gw.connects[1] == "10.0.0.2:5900",
            "the app did not connect to host:5900 (" ..
            tostring(gw.connects[1]) .. ")")
    cursor0 = oam(0)
    print(string.format("cursor at rest: OAM x=%d y=%d high=%02X",
                        cursor0, oam(1), oam(0x200)))
    r.check(cursor0 == FB_W / 2, "the cursor did not start in the middle")
    L.feed("\31")                            -- SNOS_KEY_RIGHT, as kbdbridge sends it
    step = 4
    at = frame + 120
    return
  end

  if step == 4 then
    after_key = oam(0)
    print(string.format("after RIGHT through the mailbox: OAM x=%d", after_key))
    r.check(after_key == cursor0 + 8, string.format(
      "the cursor sprite did not move right: x was %d, now %d", cursor0, after_key))
    -- Space reports the app's own cursor position back to the server, which is
    -- what tells a lost keystroke apart from a lost OAM write.
    L.feed(" ")
    step = 5
    at = frame + 120
    return
  end

  if step == 5 then
    r.check(#rfb.pointers >= 1, "space produced no pointer event at all")
    if #rfb.pointers >= 1 then
      print(string.format("pointer event: x=%d y=%d", rfb.pointers[1].x,
                          rfb.pointers[1].y))
      r.check(rfb.pointers[1].x == cursor0 + 8, string.format(
        "the app thinks the cursor is at x=%d, expected %d",
        rfb.pointers[1].x, cursor0 + 8))
    end
    pad = { left = true }                    -- and now the d-pad
    step = 6
    at = frame + 8
    return
  end

  if step == 6 then pad = nil; step = 7; at = frame + 120; return end

  if step == 7 then
    after_pad = oam(0)
    print(string.format("after LEFT on the d-pad: OAM x=%d", after_pad))
    r.check(after_pad == cursor0, string.format(
      "the d-pad did not move the cursor: x was %d, now %d", after_key, after_pad))
    step = 8
    at = frame + 1
    return
  end

  if step == 8 then
    local lo, hi, out = dma_span()
    print(string.format("first screen painted by frame %d", painted_at))
    print(string.format("DMA: %d transfers, scanlines %d..%d, %d above the strip",
                        dma.total, lo, hi, out))
    print(string.format(
      "forced blanks: %d episodes, %d checked, %d outside scanlines %d..%d, latest close L%d",
      ini.episodes, ini.checked, ini.bad, STRIP_TOP, STRIP_END - 1, ini.worst))
    print("$2100: " .. table.concat(ini.log, " "))
    r.check(ini.checked > 10, "hardly any forced blank was checked")
    r.check(ini.bad == 0, string.format(
      "%d of %d forced blanks fell where the picture is (first: %s)",
      ini.bad, ini.checked, tostring(ini.first_bad)))
    r.finish()
    return
  end
end, emu.eventType.endFrame)
