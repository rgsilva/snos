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
-- is garbage. That matters more than ever now, because the tilemap is no longer
-- the identity: see below.
--
-- ------------------------------------------------------------------ panning
--
-- THE SERVER HERE IS 512x384 AND THE CONSOLE SHOWS 256x192 OF IT. The desktop
-- is deliberately bigger than the machine, exactly as tools/vncbox/start.sh
-- makes it, and this server answers the REQUESTED RECTANGLE rather than always
-- sending the top-left corner — which is what a real one does and what the
-- whole feature rests on.
--
-- Four things about a pan are asserted, and each of them is a way it could be
-- silently wrong:
--
--   1. THE PICTURE, through the tilemap, against the pixel the SERVER has at
--      that position. p(x,y) = 5x + 11y over the whole 512x384 desktop, so a
--      window drawn at the wrong origin cannot accidentally look right.
--
--   2. THE TILEMAP ITSELF is rotated, not rewritten from new tile data. Cell
--      (0,0) must point at the tile the pan origin lands in.
--
--   3. ONLY THE EXPOSED STRIP IS REQUESTED. A pan that re-asks for the whole
--      48 KB window would still paint the right picture, in fourteen seconds.
--      The request log is checked rectangle by rectangle.
--
--   4. A CLICK AFTER PANNING lands at pan origin + cursor, in server
--      coordinates. That is the one that reads as a regression rather than a
--      missing feature, because it works perfectly until the first pan.
--
--   5. A COPYRECT SCROLL, sent while the window is at (32,32). The shadow is a
--      torus, so a copy's source and destination wrap at different points on
--      it, and a scroll's rows overlap — neither of which is visible while the
--      pan origin is (0,0), which is the only place the old code ever ran.
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
-- written to the wrong place can accidentally look right — and, now that the
-- desktop is four times the console, no WINDOW drawn at the wrong origin can
-- either.
local SRV_W, SRV_H = 512, 384
local VIEW_W, VIEW_H = 256, 192
local function raw_px(x, y) return (x * 5 + y * 11) % 256 end

-- `scroll` is how far the desktop has moved under itself, which is how the
-- CopyRect case below is expressed: after it, the pixel at (x,y) is whatever
-- used to be 32 rows lower. Both the server's bytes and the client's expected
-- picture come from this one function, so a copy that lands in the wrong place
-- cannot agree with it.
local scroll = 0
local function px(x, y) return raw_px(x, y + scroll) end

local fbrow = {}
local function rebuild_fb()
  for y = 0, SRV_H - 1 do
    local t = {}
    for x = 0, SRV_W - 1 do t[x + 1] = string.char(px(x, y)) end
    fbrow[y + 1] = table.concat(t)
  end
end
rebuild_fb()

local function be16(v) return string.char((v >> 8) & 0xFF, v & 0xFF) end
local function be32(v)
  return string.char((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
end

local rfb = { ch = nil, inbuf = "", requests = 0, nonincr = 0, pointers = {},
              reqlog = {}, lastnoninc = nil, co = nil, done_first = false }

local function rfb_send(s) push(rfb.ch, s) end

local function rect(x, y, w, h, data)
  return be16(x) .. be16(y) .. be16(w) .. be16(h) .. be32(0) .. data
end

-- Raw pixels for an arbitrary rectangle of the desktop, which is the whole
-- point: the client asks for the part it is looking at, not for the corner.
local function rectdata(x, y, w, h)
  local t = {}
  for r = 0, h - 1 do t[r + 1] = fbrow[y + r + 1]:sub(x + 1, x + w) end
  return table.concat(t)
end

-- One update message carrying the requested rectangle, split into bands of at
-- most 32 rows. Several rectangles in one message on purpose: a real server
-- does split a screen, and the app repaints after every rectangle — which is
-- what made the old forced blank flash six times rather than once.
local function send_rect_update(x, y, w, h)
  local bands, yy = {}, y
  while yy < y + h do
    local bh = math.min(32, y + h - yy)
    bands[#bands + 1] = rect(x, yy, w, bh, rectdata(x, yy, w, bh))
    yy = yy + bh
  end
  rfb_send(string.char(0, 0) .. be16(#bands) .. table.concat(bands))
end

local function copyrect(x, y, w, h, sx, sy)
  return be16(x) .. be16(y) .. be16(w) .. be16(h) .. be32(1) .. be16(sx) .. be16(sy)
end

-- Scroll the desktop up by 32 and tell the client the way a real server would:
-- "this rectangle is already on your screen, 32 rows lower", plus raw pixels for
-- the band that has newly come up from below.
--
-- CopyRect is the one path panning rewrote that nothing else reaches. The shadow
-- is a torus now, so source and destination wrap at DIFFERENT points on it, and
-- a scroll's source and destination rows overlap — which is two ways to be
-- wrong that only appear once the pan origin is not (0,0). So this runs while
-- the window is at (32,32), where both wrap.
--
-- It also does not need a new request: the client always leaves one incremental
-- request outstanding, so a server with something to say can simply say it.
local function send_scroll(winx, winy)
  scroll = 32
  rebuild_fb()
  rfb_send(string.char(0, 0) .. be16(2) ..
           copyrect(winx, winy, VIEW_W, VIEW_H - 32, winx, winy + 32) ..
           rect(winx, winy + VIEW_H - 32, VIEW_W, 32,
                rectdata(winx, winy + VIEW_H - 32, VIEW_W, 32)))
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
    rfb_send(be16(SRV_W) .. be16(SRV_H) ..
             string.char(8, 8, 0, 1) .. be16(255) .. be16(255) .. be16(255) ..
             string.char(0, 0, 0, 0, 0, 0) ..
             be32(#name) .. name)

    rfb.pf = rd(20)                          -- SetPixelFormat
    rfb.se = rd(12)                          -- SetEncodings

    while true do
      local t = rd(1):byte(1)
      if t == 3 then                         -- FramebufferUpdateRequest
        local q = rd(9)
        local r = { inc = q:byte(1),
                    x = (q:byte(2) << 8) | q:byte(3),
                    y = (q:byte(4) << 8) | q:byte(5),
                    w = (q:byte(6) << 8) | q:byte(7),
                    h = (q:byte(8) << 8) | q:byte(9) }
        rfb.requests = rfb.requests + 1
        rfb.reqlog[#rfb.reqlog + 1] = r
        if r.x + r.w > SRV_W or r.y + r.h > SRV_H then
          error(string.format("client asked for %dx%d at (%d,%d), off a %dx%d desktop",
                              r.w, r.h, r.x, r.y, SRV_W, SRV_H))
        end
        if r.inc == 0 then
          rfb.nonincr = rfb.nonincr + 1
          rfb.lastnoninc = r
          send_rect_update(r.x, r.y, r.w, r.h)
        else
          -- An incremental request over a still desktop is answered with
          -- NOTHING AT ALL, which is what RFB does and what leaves the app
          -- waiting in rd() with the whole picture painted. It is also the
          -- state a pan has to be able to start from, since the app is then
          -- blocked on a message that may never come.
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
local STRIP_TOP, STRIP_END = VIEW_H, 225
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

-- "Has the app stopped painting?" A pan produces no further request once its
-- strip has arrived — it goes back to waiting on the incremental one that is
-- still outstanding — so there is no protocol event to wait for.
--
-- `busy` is the half that is not obvious. A rectangle is painted only once all
-- of it has been read, and a full-width strip is a single 8 KB rectangle — so
-- for hundreds of frames after the request there is no DMA at all, and a
-- quiet-counter on its own calls that "finished" and then checks a screen the
-- data has not reached yet. So the server's queue has to be empty too.
local dma_mark, dma_since = -1, 0
local function settled(n, frame, busy)
  if busy or dma.total ~= dma_mark then
    dma_mark, dma_since = dma.total, frame
    return false
  end
  return frame - dma_since >= n
end

-- ------------------------------------------------------------- decoding
local VRAM = emu.memType.snesVideoRam
local OAM = emu.memType.snesSpriteRam
local MAPBASE = 0xE000        -- BG1 tilemap, BYTE address

-- Follow the tilemap the way the PPU does: 32x32 entries of two bytes, tile
-- index = entry & 0x3FF, tile data 64 bytes per tile from VRAM 0, 8bpp as four
-- pairs of bitplanes.
--
-- (panx, pany) is where the window is supposed to be over the desktop, so this
-- checks the ORIGIN as well as the pixels: a client that panned its tilemap but
-- fetched the wrong strip, or fetched the right strip and forgot to rotate,
-- fails here rather than looking plausible.
local function map_at(tx, ty) return emu.read16(MAPBASE + (ty * 32 + tx) * 2, VRAM) end

local function check_picture(panx, pany)
  local bad, first = 0, nil
  for ty = 0, (VIEW_H / 8) - 1 do
    for tx = 0, (VIEW_W / 8) - 1 do
      local entry = map_at(tx, ty)
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
          local want = px(panx + tx * 8 + col, pany + ty * 8 + row)
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

-- The tile a map cell must point at for a given pan origin. The shadow is a
-- torus — server pixel (sx,sy) always lives at (sx mod 256, sy mod 192) — so
-- panning is a ROTATION of tile indices and nothing else.
local function want_entry(panx, pany, tx, ty)
  return (((pany // 8) + ty) % (VIEW_H // 8)) * 32 + (((panx // 8) + tx) % (VIEW_W // 8))
end

local function oam(i) return emu.read(i, OAM) end

-- ------------------------------------------------------------------ test
local r = L.new_report("vnc")
local step, at, claimed = 1, nil, false
local painted_at, cursor0, after_key, after_pad = nil, nil, nil, nil
local pad = nil

-- One press each way, and the rectangle each one is allowed to ask for.
--
-- 32 pixels is four tiles, so the 224 columns (or 160 rows) that stay on screen
-- move by rewriting the tilemap and nothing else. What is left to fetch is the
-- strip that has never been seen: 32x192 sideways, 256x32 up and down. If any
-- of these ever reads 256x192 the pan still LOOKS right and takes fourteen
-- seconds, which is the failure this line exists to catch.
--
-- The click goes in after the second pan, at origin (32,32), because a pointer
-- event that forgets the origin is exactly right at (0,0).
local PANS = {
  { key = "d", req = { 256, 0, 32, 192 },  at = { 32, 0 } },
  { key = "s", req = { 32, 192, 256, 32 }, at = { 32, 32 }, click = true },
  { key = "a", req = { 0, 32, 32, 192 },   at = { 0, 32 } },
  { key = "w", req = { 0, 0, 256, 32 },    at = { 0, 0 } },
}
local pan_i, pan_t0, pan_want, pan_cost = 0, 0, 0, {}

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

  if frame > 45000 then
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
    local bad, first = check_picture(0, 0)
    print(string.format("picture: %d wrong pixels of %d", bad, VIEW_W * VIEW_H))
    r.check(bad == 0, string.format(
      "%d pixels wrong through the tilemap; first: %s", bad, tostring(first)))
    r.check(rfb.reqlog[1] ~= nil and rfb.reqlog[1].inc == 0 and
            rfb.reqlog[1].w == VIEW_W and rfb.reqlog[1].h == VIEW_H and
            rfb.reqlog[1].x == 0 and rfb.reqlog[1].y == 0,
            "the first request was not the whole window at the origin")
    r.check(#gw.connects == 1 and gw.connects[1] == "10.0.0.2:5900",
            "the app did not connect to host:5900 (" ..
            tostring(gw.connects[1]) .. ")")
    cursor0 = oam(0)
    print(string.format("cursor at rest: OAM x=%d y=%d high=%02X",
                        cursor0, oam(1), oam(0x200)))
    r.check(cursor0 == VIEW_W / 2, "the cursor did not start in the middle")
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
      r.check(rfb.pointers[1].x == cursor0 + 8 and
              rfb.pointers[1].y == VIEW_H / 2, string.format(
        "the app thinks the cursor is at (%d,%d), expected (%d,%d)",
        rfb.pointers[1].x, rfb.pointers[1].y, cursor0 + 8, VIEW_H / 2))
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

  -- ------------------------------------------------------------- panning
  if step == 8 then
    pan_i = pan_i + 1
    if pan_i > #PANS then step = 12; at = frame + 1; return end
    pan_want = rfb.nonincr + 1
    pan_t0 = frame
    L.feed(PANS[pan_i].key)
    step = 9
    at = nil
    return
  end

  if step == 9 then
    -- The strip has to have been asked for AND the app has to have stopped
    -- painting it. There is no third request to wait for: once a pan's strip
    -- has landed the app goes back to waiting on the incremental request that
    -- was already outstanding, which is the whole point of the design.
    if rfb.nonincr < pan_want then return end
    if not settled(120, frame, #gw.out > 0) then return end
    do
      local p = PANS[pan_i]
      local q = rfb.lastnoninc
      local we = want_entry(p.at[1], p.at[2], 0, 0)
      local e = map_at(0, 0) & 0x3FF
      local bad, first
      pan_cost[pan_i] = frame - 120 - pan_t0
      print(string.format("pan '%s': asked %dx%d at (%d,%d), settled in %d frames",
                          p.key, q.w, q.h, q.x, q.y, pan_cost[pan_i]))
      r.check(q.x == p.req[1] and q.y == p.req[2] and
              q.w == p.req[3] and q.h == p.req[4], string.format(
        "pan '%s' fetched %dx%d at (%d,%d); only the exposed %dx%d at (%d,%d) is new",
        p.key, q.w, q.h, q.x, q.y, p.req[3], p.req[4], p.req[1], p.req[2]))
      r.check(e == we, string.format(
        "pan '%s': tilemap cell (0,0) points at tile %d, expected %d - the map did not rotate",
        p.key, e, we))
      bad, first = check_picture(p.at[1], p.at[2])
      print(string.format("pan '%s' -> window at (%d,%d): %d wrong pixels",
                          p.key, p.at[1], p.at[2], bad))
      r.check(bad == 0, string.format(
        "pan '%s': %d pixels wrong through the tilemap; first: %s",
        p.key, bad, tostring(first)))
      if p.click then
        L.feed(" ")
        step = 11
        at = frame + 120
        return
      end
    end
    step = 8
    at = frame + 1
    return
  end

  -- ------------------------------------------------------------- CopyRect
  if step == 13 then
    send_scroll(PANS[pan_i].at[1], PANS[pan_i].at[2])
    print("sent a CopyRect scroll with the window at (" ..
          PANS[pan_i].at[1] .. "," .. PANS[pan_i].at[2] .. ")")
    step = 14
    at = nil
    return
  end

  if step == 14 then
    if not settled(120, frame, #gw.out > 0) then return end
    do
      local p = PANS[pan_i]
      local bad, first = check_picture(p.at[1], p.at[2])
      print(string.format("after CopyRect at (%d,%d): %d wrong pixels",
                          p.at[1], p.at[2], bad))
      r.check(bad == 0, string.format(
        "CopyRect across the torus: %d pixels wrong; first: %s",
        bad, tostring(first)))
    end
    step = 8
    at = frame + 1
    return
  end

  -- A click AFTER panning. The cursor has not moved since step 7, so the only
  -- thing that can have changed is whether the app adds the pan origin.
  if step == 11 then
    local want = { x = PANS[pan_i].at[1] + cursor0, y = PANS[pan_i].at[2] + VIEW_H / 2 }
    local p = rfb.pointers[3]
    r.check(p ~= nil, "space after a pan produced no pointer event")
    if p then
      print(string.format("pointer after panning to (%d,%d): x=%d y=%d",
                          PANS[pan_i].at[1], PANS[pan_i].at[2], p.x, p.y))
      r.check(p.x == want.x and p.y == want.y, string.format(
        "a click after panning landed at (%d,%d) in server coordinates, expected (%d,%d)",
        p.x, p.y, want.x, want.y))
    end
    step = 13                                -- the CopyRect case, still panned
    at = frame + 1
    return
  end

  if step == 12 then
    local lo, hi, out = dma_span()
    print(string.format("first screen painted by frame %d", painted_at))
    print("pan cost, frames: " .. table.concat(pan_cost, " "))
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
