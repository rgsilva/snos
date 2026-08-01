-- Regression test for `vnc`: a Lua RFB SERVER on the far side of the console's
-- OWN TCP (the uIP cutover: there is no stream gateway to ride any more).
--
-- test/gwip.lua is the hermetic slirp box; this implements what is beyond it,
-- so the console runs the real RFB protocol — version handshake, security
-- type, SetPixelFormat, SetEncodings, FramebufferUpdate, PointerEvent — against
-- something that answers frame for frame, over TCP segments the 65816 composed
-- and checksummed itself. Nothing is stubbed below the app.
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
--      48 KB window would still paint the right picture, in a minute or two
--      over TCP. The request log is checked rectangle by rectangle.
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
-- ------------------------------------------------------- scrolling and the map
--
-- Two more things the app grew, and the ways each of them can be silently
-- wrong:
--
--   6. J AND K ARE WHEEL CLICKS, not Page Up/Down keystrokes: PointerEvent with
--      button 5 (mask 16) for down and button 4 (mask 8) for up, each a press
--      followed by a release. The mask is asserted because a wheel sent as
--      button 1 would still make a browser do SOMETHING, and the COORDINATES
--      are asserted twice — at the origin and again after a pan, because pan
--      origin plus cursor is exactly the arithmetic a click got wrong once
--      before, and at (0,0) the wrong answer is the right one.
--
--   7. THE MINIMAP lives in the strip below the picture — the outer frame in
--      tile data the strip's tilemap rows now point at, the moving marker in
--      two sprites. Both halves are checked, and the frame is checked THROUGH
--      THE TILEMAP for the same reason the picture is: repointing those rows is
--      the sort of change that decodes correctly in assumed order and displays
--      as garbage. The whole strip is decoded, so a cell that should still be
--      black and is not fails here too.
--
--      The marker is checked against (px, py) after every pan. It is the only
--      thing on screen that says where in the desktop the window is, so a
--      marker that does not move is a feature that is present and useless.
--
-- TCP makes the bulk transfer slower than the old raw stream — a 48 KB screen
-- is ~92 segments of 536, each a link round trip — so the waits here are gated
-- on observable progress (requests answered, DMA quiet, the server's queues
-- empty) rather than fixed frame counts, and the global guard is sized for the
-- slower wire.
--
-- NOT part of `make test`: it is a minutes-long run on its own, against an app
-- with no other coverage. `make test-vnc`.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}

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

local be16, be32 = G.be16, G.be32

local rfb = { sock = nil, inbuf = "", requests = 0, nonincr = 0, pointers = {},
              reqlog = {}, lastnoninc = nil, co = nil, done_first = false }
local connects = {}    -- { ip=, port= } per SYN the console sent us

local function rfb_send(s) rfb.sock:send(s) end

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

-- The console dials out (SYS_NET_CONNECT -> a real SYN): the RFB server
-- answers on whatever connection arrives. Client-to-server bytes accumulate
-- in sock.rx; the endFrame loop below drains them into the coroutine.
gw.on_connect = function(sock, port)
  connects[#connects + 1] = { ip = sock.peer_ip, port = port }
  rfb.sock = sock
  rfb_start()
end

-- Is the server still holding bytes the console has not taken? Three queues
-- now, not one: link records (gw.out), the segment in flight (sock.unacked)
-- and the TCP send queue behind it (sock.txq).
local function srv_busy()
  if #gw.out > 0 then return true end
  local s = rfb.sock
  return s ~= nil and (#s.txq > 0 or s.unacked ~= "")
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
-- data has not reached yet. So the server's queues have to be empty too — all
-- three of them, now that TCP holds bytes the link layer never sees.
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

-- ---------------------------------------------------------------- minimap
-- Mirrors apps/vnc/vnc.c's geometry. The numbers are small and fixed, but they
-- are derived here the same way the app derives them so that a change to MM_W
-- or MM_H is a change in one place on each side rather than a table of magic
-- constants to hunt through.
local MM_W, MM_H = 32, 10
local MM_Y0 = 224 - MM_H              -- 214: the first line never blanked
local MM_R0 = MM_Y0 // 8              -- 26
local MM_TR = 28 - MM_R0              -- 2 tilemap rows
local MM_TW = MM_W // 8               -- 4 tile columns
local MM_C0 = (32 - MM_TW) // 2       -- centred
local MM_X0 = MM_C0 * 8
local MM_INK = (5 << 5) | (5 << 2) | 2
local TILE_BLANK = 0xF000 // 64
local TILE_MM = 0xF040 // 64
local MM_MARK_W, MM_MARK_H = 16, 8

-- n * num / den, rounded — the app's scale(), which it spells out by repeated
-- addition because a 65816 has no divide. Mirrored rather than simplified to
-- `math.floor(n * num / den + 0.5)`: the point is to assert the number the app
-- actually produces, and the two agree only if the rounding does.
local function mmscale(n, num, den)
  if den == 0 or num == 0 then return 0 end
  local acc, out = den // 2, 0
  for _ = 1, n do
    acc = acc + num
    while acc >= den do acc = acc - den; out = out + 1 end
  end
  return out
end

local MAXPX = (SRV_W - VIEW_W) & 0xFFF8
local MAXPY = (SRV_H - VIEW_H) & 0xFFF8
local mm_mw = mmscale(VIEW_W >> 3, MM_W, SRV_W >> 3)
local mm_mh = mmscale(VIEW_H >> 3, MM_H, SRV_H >> 3)
local mm_travx = (mm_mw < MM_W) and (MM_W - mm_mw) or 0
local mm_travy = (mm_mh < MM_H) and (MM_H - mm_mh) or 0
if mm_mw > MM_MARK_W then mm_mw = MM_MARK_W end
if mm_mh > MM_MARK_H then mm_mh = MM_MARK_H end

-- Where the marker's top-left pixel belongs for a given pan origin.
local function marker_at(panx, pany)
  return MM_X0 + mmscale(panx >> 3, mm_travx, MAXPX >> 3),
         MM_Y0 + mmscale(pany >> 3, mm_travy, MAXPY >> 3)
end

-- The marker is two 8x8 sprites side by side, OAM entries 1 and 2. Sprite Y is
-- one less than the scanline the sprite appears on, which is why the app writes
-- y - 1 and why this expects it back.
local function check_marker(r, panx, pany, what)
  local wx, wy = marker_at(panx, pany)
  local gx, gy, gt = oam(4), oam(5), oam(6)
  local hx, hy, ht = oam(8), oam(9), oam(10)
  print(string.format("minimap marker at pan (%d,%d): OAM (%d,%d) tile %d + (%d,%d) tile %d",
                      panx, pany, gx, gy, gt, hx, hy, ht))
  r.check(gx == wx and gy == wy - 1, string.format(
    "%s: the minimap marker is at (%d,%d), expected (%d,%d) for pan (%d,%d)",
    what, gx, gy, wx, wy - 1, panx, pany))
  r.check(hx == wx + 8 and hy == wy - 1 and gt == 1 and ht == 2, string.format(
    "%s: the marker's second half is at (%d,%d) tiles %d/%d, expected (%d,%d) tiles 1/2",
    what, hx, hy, gt, ht, wx + 8, wy - 1))
end

-- Every displayed pixel BELOW the picture, decoded through the tilemap exactly
-- as check_picture does it. Two things have to hold at once:
--
--   the cells the minimap claims point at its own tiles, everything else still
--   points at the all-zero tile — repointing a row is how the strip stops being
--   black, and a black strip is what makes the forced-blank window invisible;
--
--   and what those tiles actually contain is the box: a one-pixel rectangle in
--   MM_INK with nothing inside it.
--
-- Decoding the minimap's tile data in assumed order would confirm neither.
--
-- THE WHOLE 16-BIT ENTRY IS COMPARED, not `entry & 0x3FF`, and that is the
-- point of this check rather than a detail of it. The strip's entries were
-- 0xFFC0 — right tile, and every one of the palette, priority and FLIP bits
-- set, because Calypsi folds `0xF000 / 64` to -64 (see the note on VRAM_MM in
-- vnc.c). An all-zero tile is its own mirror image, so nothing showed it; the
-- minimap came out rotated 180 degrees the moment the strip had a shape in it.
-- Masking with 0x3FF is masking off exactly the bits that were wrong.
local function check_strip()
  local bad, first = 0, nil
  local function note(s) bad = bad + 1; first = first or s end
  for ty = VIEW_H // 8, 27 do
    for tx = 0, 31 do
      local entry = map_at(tx, ty)
      local inbox = ty >= MM_R0 and ty < MM_R0 + MM_TR and
                    tx >= MM_C0 and tx < MM_C0 + MM_TW
      local want_entry = inbox
        and (TILE_MM + (ty - MM_R0) * MM_TW + (tx - MM_C0)) or TILE_BLANK
      if entry ~= want_entry then
        note(string.format(
          "strip cell (%d,%d) entry is %04X, expected %04X (flip/priority bits "
          .. "in the top six are as wrong as a wrong tile)",
          tx, ty, entry, want_entry))
      end
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
          local sx, sy = tx * 8 + col, ty * 8 + row
          local want = 0
          if sx >= MM_X0 and sx < MM_X0 + MM_W and
             sy >= MM_Y0 and sy < MM_Y0 + MM_H then
            if sx == MM_X0 or sx == MM_X0 + MM_W - 1 or
               sy == MM_Y0 or sy == MM_Y0 + MM_H - 1 then want = MM_INK end
          end
          if v ~= want then
            note(string.format("strip pixel (%d,%d) is %d, want %d (entry %04X)",
                               sx, sy, v, want, entry))
          end
        end
      end
    end
  end
  return bad, first
end

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
-- of these ever reads 256x192 the pan still LOOKS right and takes a minute
-- over TCP, which is the failure this line exists to catch.
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

-- RFB button 4 is wheel-up and button 5 is wheel-down, so bit 3 and bit 4 of
-- the PointerEvent mask. J scrolls down and K scrolls up, which is Ricardo's
-- choice of keys and the one thing here that is a preference rather than a
-- protocol.
local WHEEL_DOWN, WHEEL_UP = 16, 8
local wheel_base, click_base = 0, 0

-- A wheel click is a PRESS AND A RELEASE at the same point, so two events. Both
-- are checked: a client that sent only the press would leave a button held down
-- for the rest of the session, which a server is entitled to treat as a drag.
local function check_wheel(r, base, mask, wantx, wanty, what)
  local a, b = rfb.pointers[base + 1], rfb.pointers[base + 2]
  r.check(a ~= nil and b ~= nil, what .. ": expected a press and a release, got " ..
          tostring(#rfb.pointers - base))
  if not (a and b) then return end
  print(string.format("%s: mask %d then %d at (%d,%d)", what, a.mask, b.mask, a.x, a.y))
  r.check(a.mask == mask and b.mask == 0, string.format(
    "%s: masks were %d/%d, expected %d/0", what, a.mask, b.mask, mask))
  r.check(a.x == wantx and a.y == wanty and b.x == wantx and b.y == wanty,
          string.format(
    "%s: wheel landed at (%d,%d)/(%d,%d) in server coordinates, expected (%d,%d)",
    what, a.x, a.y, b.x, b.y, wantx, wanty))
end

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
    gw:claim()
    claimed = true
    at = frame + 60
    return
  end

  gw:service()
  -- Drain the connection into the RFB parser. sock.rx is "everything
  -- received"; taking it and clearing is this test's consumption.
  if rfb.sock and #rfb.sock.rx > 0 then
    local s = rfb.sock.rx
    rfb.sock.rx = ""
    rfb_feed(s)
  end

  -- 45000 was the raw-stream budget; TCP spends a link round trip per 536-byte
  -- segment, so the initial 48 KB screen alone is ~700-1200 frames and each
  -- pan strip ~100-200 more. Triple the guard rather than trim the margin.
  if frame > 135000 then
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
    r.check(#connects == 1 and connects[1].ip == "10.0.0.2" and
            connects[1].port == 5900,
            "the app did not connect to host:5900 (" ..
            (connects[1] and (connects[1].ip .. ":" .. connects[1].port)
                          or "no connection") .. ")")
    r.check(gw.tcp_bad_csum == 0,
            "the console sent " .. gw.tcp_bad_csum ..
            " TCP segments with a bad checksum")
    cursor0 = oam(0)
    print(string.format("cursor at rest: OAM x=%d y=%d high=%02X",
                        cursor0, oam(1), oam(0x200)))
    r.check(cursor0 == VIEW_W / 2, "the cursor did not start in the middle")
    do
      local sbad, sfirst = check_strip()
      print(string.format("strip: %d wrong pixels of %d",
                          sbad, 32 * 8 * (224 - VIEW_H)))
      r.check(sbad == 0, string.format(
        "the strip below the picture is wrong in %d places; first: %s",
        sbad, tostring(sfirst)))
      check_marker(r, 0, 0, "at the origin")
    end
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
    step = 20
    at = frame + 1
    return
  end

  -- ------------------------------------------------------- the wheel, J and K
  -- At the origin first, where a client that forgot the pan origin still looks
  -- perfect. The pair after the pan below is the one that catches it.
  if step == 20 then
    wheel_base = #rfb.pointers
    L.feed("j")
    step = 21
    at = frame + 120
    return
  end

  if step == 21 then
    check_wheel(r, wheel_base, WHEEL_DOWN, cursor0, VIEW_H / 2, "J at the origin")
    wheel_base = #rfb.pointers
    L.feed("k")
    step = 22
    at = frame + 120
    return
  end

  if step == 22 then
    check_wheel(r, wheel_base, WHEEL_UP, cursor0, VIEW_H / 2, "K at the origin")
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
    if not settled(120, frame, srv_busy()) then return end
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
      -- The marker is the only thing on screen that says where in the desktop
      -- this window is, so it is checked on every pan rather than once.
      check_marker(r, p.at[1], p.at[2], "pan '" .. p.key .. "'")
      if p.click then
        click_base = #rfb.pointers
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
    if not settled(120, frame, srv_busy()) then return end
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
    local p = rfb.pointers[click_base + 1]
    r.check(p ~= nil, "space after a pan produced no pointer event")
    if p then
      print(string.format("pointer after panning to (%d,%d): x=%d y=%d",
                          PANS[pan_i].at[1], PANS[pan_i].at[2], p.x, p.y))
      r.check(p.x == want.x and p.y == want.y, string.format(
        "a click after panning landed at (%d,%d) in server coordinates, expected (%d,%d)",
        p.x, p.y, want.x, want.y))
    end
    -- And the wheel from the same place. A pan origin dropped from a wheel
    -- event scrolls whatever happens to be under (cx, cy) on the SERVER's
    -- top-left corner instead of under the cursor, which on a page with one
    -- scrollable region looks exactly like it worked.
    wheel_base = #rfb.pointers
    L.feed("j")
    step = 23
    at = frame + 120
    return
  end

  if step == 23 then
    check_wheel(r, wheel_base, WHEEL_DOWN, PANS[pan_i].at[1] + cursor0,
                PANS[pan_i].at[2] + VIEW_H / 2, "J after panning")
    step = 13                                -- the CopyRect case, still panned
    at = frame + 1
    return
  end

  if step == 12 then
    local lo, hi, out = dma_span()
    -- The strip again, at the end: four pans and a CopyRect later, the picture
    -- rows have been rewritten three times over and the minimap must be exactly
    -- as it was. write_tilemap() touches rows 0-23 only, and this is what says
    -- so.
    do
      local sbad, sfirst = check_strip()
      print(string.format("strip after four pans and a scroll: %d wrong pixels", sbad))
      r.check(sbad == 0, string.format(
        "the strip is wrong in %d places after panning; first: %s",
        sbad, tostring(sfirst)))
    end
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
