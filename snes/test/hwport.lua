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

  local function restage(t)
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
  end, emu.eventType.nmi)

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
      local t = clock()
      pico.t_first = t
      restage(pico.last_end + IRQ)
      local ar = pico.last_nmi + AUTOREAD_END + GAP + IRQ
      if pico.last_nmi >= 0 and ar > pico.last_end and ar < t then restage(ar) end
      local kick = pico.last_kick + GAP + IRQ
      if pico.last_kick > pico.last_end and kick < t then restage(kick) end
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
