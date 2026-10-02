-- hwport.lua -- the Pico W adapter on controller port 2, for the LINK_HW ROM.
--
-- The emulator build talks to its peer through a WRAM mailbox and never runs
-- kernel/linkxfer.s. This puts a model of the adapter on the port itself, so
-- the hardware ROM's real exchange code, its NMI timing and the foreground
-- poll gate run under Mesen, against a gwip.lua peer driven one exchange at a
-- time (gw:exchange) exactly as pico/main.c drives pico/link.c.
--
-- The wire (docs/pico-adapter.md section 4): each data bit is a $4016 write
-- (LATCH = the console's bit) followed by a $4017 read (one clock; bit 0
-- returns the adapter's bit, inverted as the real CPU reads the line). A
-- frame is LINK_FRAME*8 such pairs, and its first bit always has TWO writes
-- before it: link_serial_xfer drops LATCH before its quiet spin. Between
-- frames, a read with fewer is a kick (kernel/linkxfer.s link_serial_kick):
-- a short burst the adapter's gap detector turns into a restart.
--
-- pico/port.c's TX staging, which is the part that decides throughput: the
-- frame shifted out in an exchange is the one latched at the adapter's most
-- recent restart before the exchange's first clock. Restarts happen at the
-- end of every exchange (DMA complete), ~64 us after the vblank auto-joypad
-- read (its 16 clocks are a short burst; Mesen does not show them to Lua, so
-- the NMI event stands in), and ~64 us after a kick. A reply only exists
-- `pico_us` after the frame it answers ended -- the Pico's main loop.
-- opt.staging picks port.c's policy: "fifo" (the firmware now) stages
-- replies in order and replaces a staged frame only once it has gone out
-- whole; "latest" (the firmware before link windows) stages the newest
-- reply at every restart, so a staged frame can be replaced unsent.
--
-- Time comes from emu.getState().masterClock, which costs ~0.3 ms of real
-- time per call, so it is read at most a few times per exchange.
local P = {}

local LINE = 1364
local GAP = 1376          -- the PIO's 64.08 us countdown, master cycles
local IRQ = 300           -- gap IRQ to restart() done
local AUTOREAD_END = 200 + 4224

local function clock()
  return emu.getState()["masterClock"]
end

function P.new(gw, opt)
  opt = opt or {}
  local FR = opt.frame or 64
  local pico = {
    gw = gw,
    pico_mc = math.floor((opt.pico_us or 300) * 21.47727),
    staging = opt.staging or "fifo",
    latched_sent = true,
    exchanges = 0, kicks = 0, short = 0,
    latched = nil,          -- frame the next exchange shifts out
    pending = {},           -- { ready = clock, frame = table }, oldest first
    last_end = 0, last_kick = -1, last_nmi = -1,
    t_first = 0,
    rx = {}, nbits = 0, latch = 0, writes = 0, kicking = 0,
    -- observers
    on_exchange = nil,      -- function(console_frame, reply, t_first, t_end)
  }

  local function idle_frame()
    local f = {}
    for i = 0, FR - 1 do f[i] = 0xFF end
    return f
  end
  pico.latched = idle_frame()

  local function restage(t, whole)
    if whole then pico.latched_sent = true end
    if pico.staging == "fifo" then
      if pico.latched_sent and #pico.pending > 0 and pico.pending[1].ready <= t then
        pico.latched = table.remove(pico.pending, 1).frame
        pico.latched_sent = false
      end
      return
    end
    while #pico.pending > 0 and pico.pending[1].ready <= t do
      pico.latched = table.remove(pico.pending, 1).frame
    end
  end

  local txbits = {}
  local function load_tx()
    local f = pico.latched
    for i = 0, FR - 1 do
      local b = f[i]
      for k = 0, 7 do txbits[i * 8 + k] = (b >> (7 - k)) & 1 end
    end
  end

  emu.addEventCallback(function()
    pico.last_nmi = clock()
    -- A $01 store that lands as vblank latches its NMI (cop_ret, at line
    -- 225) is followed by the whole NMI handler, its exchange included, with
    -- bit 7 still clear: that mask is not a foreground poll's.
    if pico.mask then pico.mask.nmi = true end
  end, emu.eventType.nmi)
  -- Vblank begins (endFrame is line 225) whether or not NMI is enabled, and
  -- the auto-joypad read with it: an exchange still clocking then has the
  -- auto-read's clocks in it, and an NMI held off past here is the hazard
  -- input.c's fg_poll_fits exists to prevent.
  pico.collisions = 0
  emu.addEventCallback(function()
    if pico.nbits ~= 0 then
      pico.collisions = pico.collisions + 1
      if pico.collisions <= 8 then
        local st = emu.getState()
        print(string.format("hwport: vblank at bit %d of an exchange that began %.1f lines ago,"
          .. " on line %d; last NMI %.1f lines ago", pico.nbits,
          (st.masterClock - pico.t_first) / LINE, pico.first_line or -1,
          (st.masterClock - pico.last_nmi) / LINE))
      end
    end
  end, emu.eventType.endFrame)

  -- The clocks stopping before vblank is not the whole of it: a foreground
  -- exchange runs with NMI masked ($4200 bit 7 clear, net_service_fg), and
  -- after its last clock come the receive check and net_service's return
  -- before the unmask. NMI held off at the top of vblank is the hazard
  -- itself, clocks or none. So every runtime mask ($01: NMI off, auto-joypad
  -- on; boot's $00 is not one) is followed: one that held an exchange is timed
  -- from its unmask to the next vblank, and a vblank that begins while one is
  -- open with an exchange in it -- or an exchange not yet begun but more than
  -- a line of C already run, which is a poll, not a lifecycle store -- counts.
  pico.mask = nil           -- { t, line, exchanges } of the runtime mask now open
  pico.fg_masks = 0         -- runtime masks that held an exchange
  pico.masked_vblanks = 0
  pico.unmask_t = nil       -- the last such mask's unmask, until the next vblank
  pico.margin_min = nil     -- lines from that unmask to vblank: the least seen
  pico.tail_max = 0         -- lines from an exchange's last clock to its unmask
  -- The worst case, not just the worst seen: link_serial.c's xfer() latches
  -- the beam ($2137) for its last check, so from that read to the unmask is
  -- what the check must leave room for: with the beam anywhere on its last
  -- allowed line, the unmask lands by that line + 1 + this many. HDMA ($420C,
  -- the keyboard's split) takes cycles from every line and has a gate of its
  -- own, so each state is kept apart. The gates come from link_serial.c,
  -- spliced in by the Makefile; without them, the earliest refused line
  -- stands in.
  local GATE = { [false] = tonumber("@XFER_LAST@"), [true] = tonumber("@XFER_LAST_HDMA@") }
  local function gate_stats()
    return { exchanges = 0, check_max = -1, after_max = 0, refused_min = nil }
  end
  pico.gates = { [false] = gate_stats(), [true] = gate_stats() }
  pico.hdma = 0
  emu.addMemoryCallback(function(addr, value)
    pico.hdma = value
  end, emu.callbackType.write, 0x420C)
  -- The check's line is the one the gate compared: the V counter $2137
  -- latched, low eight bits, as the $213D read after it returns them
  -- (input.c, beam_line). ppu.scanline is a different clock, the debugger's,
  -- sampled a few cycles after the read: one in the last cycles of line 131
  -- latches 131, which the gate allows, while ppu.scanline already says 132,
  -- and an exchange the ROM was right to start failed check_max <= gate. It
  -- stays for one thing the eight bits cannot say, whether the check came
  -- before vblank (check_scan < 225): lines 256-261 latch as 0-5.
  emu.addMemoryCallback(function()
    if pico.mask then
      local st = emu.getState()
      pico.mask.check_t, pico.mask.check_scan = st["masterClock"], st["ppu.scanline"]
      pico.mask.check_line = nil
      pico.mask.hdma = pico.hdma ~= 0
    end
  end, emu.callbackType.read, 0x2137)
  emu.addMemoryCallback(function(addr, value)
    local m = pico.mask
    if m and m.check_t and not m.check_line then m.check_line = value end
  end, emu.callbackType.read, 0x213D)
  emu.addMemoryCallback(function(addr, value)
    if (value & 0x80) == 0 then
      if not pico.mask and (value & 1) == 1 then
        local st = emu.getState()
        pico.mask = { t = st["masterClock"], line = st["ppu.scanline"],
                      exchanges = pico.exchanges }
      end
      return
    end
    local m = pico.mask
    pico.mask = nil
    if m and not m.nmi and m.check_line and pico.exchanges == m.exchanges and
       m.check_scan < 225 then
      local g = pico.gates[m.hdma] -- checked, and no exchange: refused
      if not g.refused_min or m.check_line < g.refused_min then g.refused_min = m.check_line end
    end
    if m and not m.nmi and pico.exchanges > m.exchanges then
      local t = clock()
      pico.fg_masks = pico.fg_masks + 1
      pico.unmask_t = t
      local tail = (t - pico.last_end) / LINE
      if tail > pico.tail_max then pico.tail_max = tail end
      if m.check_line and m.check_scan < 225 then
        local g = pico.gates[m.hdma]
        local after = (t - m.check_t) / LINE
        g.exchanges = g.exchanges + 1
        if after > g.after_max then g.after_max = after end
        if m.check_line > g.check_max then g.check_max = m.check_line end
      end
    end
  end, emu.callbackType.write, 0x4200)
  emu.addEventCallback(function()
    local m = pico.mask
    if m then
      local t = clock()
      if pico.exchanges > m.exchanges or pico.nbits ~= 0 or t - m.t > LINE then
        pico.masked_vblanks = pico.masked_vblanks + 1
        if pico.masked_vblanks <= 8 then
          print(string.format("hwport: vblank with NMI masked since line %d (%.1f lines),"
            .. " %d exchanges in the mask, at bit %d", m.line, (t - m.t) / LINE,
            pico.exchanges - m.exchanges, pico.nbits))
        end
      end
    end
    if pico.unmask_t then
      local margin = (clock() - pico.unmask_t) / LINE
      if not pico.margin_min or margin < pico.margin_min then pico.margin_min = margin end
      pico.unmask_t = nil
    end
  end, emu.eventType.endFrame)
  -- `need` says the run must have had foreground exchanges at all, so that
  -- a pass is not vacuous.
  function pico.mask_report(r, label, need)
    print(string.format("%s: %d foreground masks held an exchange; NMI unmasked at least"
      .. " %.1f lines before vblank, at most %.1f lines after the last clock", label,
      pico.fg_masks, pico.margin_min or -1, pico.tail_max))
    for _, hdma in ipairs({ false, true }) do
      local g, gate = pico.gates[hdma], GATE[hdma]
      if g.exchanges > 0 then
        -- A check passes on its gate's line at the latest, so its unmask
        -- comes before gate + 1 + after_max.
        local last = gate and gate + 1 or g.refused_min or (g.check_max + 1)
        local worst = last + g.after_max
        print(string.format("%s: %s: %d checked exchanges, the latest on line %d%s; check to"
          .. " unmask at most %.1f lines, so one checked at the gate's last moment unmasks"
          .. " by line %.1f, %.1f lines before vblank", label,
          hdma and "HDMA on" or "HDMA off", g.exchanges, g.check_max,
          gate and string.format(", gate %d", gate) or "", g.after_max, worst, 225 - worst))
        if need then
          r.check(225 - worst >= 2, string.format("%s: the last-moment check leaves %.1f lines"
            .. " between its worst-case unmask and vblank, under 2", hdma and "HDMA on" or
            "HDMA off", 225 - worst))
          if gate then
            r.check(g.check_max <= gate, string.format("an exchange was checked on line %d,"
              .. " past the gate of %d", g.check_max, gate))
          end
        end
      end
    end
    r.check(pico.masked_vblanks == 0, pico.masked_vblanks ..
      " vblanks began with NMI masked by a foreground poll")
    if need then r.check(pico.fg_masks > 0, "no foreground exchange to check") end
  end

  emu.addMemoryCallback(function(addr, value)
    pico.latch = value & 1
    pico.writes = pico.writes + 1
  end, emu.callbackType.write, 0x4016)

  emu.addMemoryCallback(function(addr, value)
    local writes = pico.writes
    pico.writes = 0
    if (pico.nbits == 0 and writes < 2) or writes == 0 then
      -- Not a frame's clock: the kick's short burst.
      if pico.kicking == 0 then pico.last_kick = clock() end
      pico.kicking = pico.kicking + 1
      return (value & 0xFE) | 1
    end
    if pico.kicking > 0 then
      pico.kicks = pico.kicks + 1
      pico.kicking = 0
    end
    local n = pico.nbits
    if n == 0 then
      -- First clock: what is in the shifter now is what goes out.
      local st0 = emu.getState()
      local t = st0["masterClock"]
      pico.t_first = t
      pico.first_line = st0["ppu.scanline"]
      restage(pico.last_end + IRQ, true)
      local ar = pico.last_nmi + AUTOREAD_END + GAP + IRQ
      local kick = pico.last_kick + GAP + IRQ
      local kicked = pico.last_kick > pico.last_end and kick < t
      local autoread = pico.last_nmi >= 0 and ar > pico.last_end and ar < t
      if autoread and (not kicked or ar < kick) then restage(ar, false) end
      if kicked then restage(kick, false) end
      if autoread and kicked and ar >= kick then restage(ar, false) end
      load_tx()
      pico.sent = pico.latched
      for i = 0, FR - 1 do pico.rx[i] = 0 end
    end
    local byte = n >> 3
    pico.rx[byte] = (pico.rx[byte] << 1 | pico.latch) & 0xFF
    local bit = txbits[n]
    pico.nbits = n + 1
    if pico.nbits == FR * 8 then
      pico.nbits = 0
      local t_end = clock()
      pico.last_end = t_end
      pico.exchanges = pico.exchanges + 1
      pico.span_sum = (pico.span_sum or 0) + (t_end - pico.t_first)
      local f = {}
      for i = 0, FR - 1 do f[i] = pico.rx[i] end
      local reply = gw:exchange(f)
      pico.pending[#pico.pending + 1] = { ready = t_end + pico.pico_mc, frame = reply }
      if pico.on_exchange then pico.on_exchange(f, pico.sent, pico.t_first, t_end) end
    end
    -- The CPU reads the line inverted: line high (a 1) is a 0 in bit 0.
    return (value & 0xFE) | (1 - bit)
  end, emu.callbackType.read, 0x4017)

  return pico
end

return P
