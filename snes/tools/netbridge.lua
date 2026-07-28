-- Tier 2: carry link frames between the emulator and a real gateway.
--
-- UNVERIFIED. Everything in M6 is tested hermetically against a Lua gateway
-- that speaks the same protocol; this script and tools/gateway.py are the path
-- to the actual internet, and neither can be exercised headlessly. Treat the
-- first run as bring-up.
--
-- HOW TO RUN IT
--   1. Enable Debug -> Script Window -> "Allow access to I/O and OS functions".
--      This is OFF by default and off in Ricardo's settings; turning it on
--      grants file and process access to ANY Lua script you subsequently run,
--      so turn it back off when you are done.
--   2. python3 snes/tools/gateway.py        (leave it running)
--   3. Load snos.sfc in the Mesen GUI, then load and Run this script.
--   4. In SNOS:  net      -> should say the gateway is up
--                get example.com
--
-- WHY FILES RATHER THAN A SOCKET OR A PIPE
--   Mesen's Lua has no socket library — `require("socket")` fails even with
--   I/O access enabled — so the process that opens sockets has to be a separate
--   one, and something has to carry bytes to it. A FIFO would be the obvious
--   choice and is the wrong one: a blocking read inside an endFrame callback
--   freezes the emulator. Reading a regular file at a tracked offset returns
--   whatever is there and never blocks, which is the property that matters.
--
--   The two files grow for the length of a session. That is fine for a dev
--   bridge and is why they are truncated at startup.

local UP = "/tmp/snos-up.bin"     -- SNES -> gateway
local DOWN = "/tmp/snos-down.bin" -- gateway -> SNES

local MB = 0xE000                 -- link mailbox, WRAM $7EE000
local MAGIC = 0x4B4C              -- "LK"
local FRAME = 64
local WRAM = emu.memType.snesWorkRam

if type(io) ~= "table" then
  print("netbridge: I/O is disabled. Enable Script Window -> " ..
        "'Allow access to I/O and OS functions' and run this again.")
  return
end

-- Truncate both directions so a previous session's traffic is not replayed.
local up = io.open(UP, "wb")
if not up then print("netbridge: cannot write " .. UP); return end
up:close()
up = io.open(UP, "ab")
local down_off = 0
do
  local f = io.open(DOWN, "wb")
  if f then f:close() end
end

local function rb(o) return emu.read(MB + o, WRAM) end
local function wb(o, v) emu.write(MB + o, v & 0xFF, WRAM) end
local function ru16(o) return emu.read16(MB + o, WRAM) end
local function wu16(o, v) emu.write16(MB + o, v & 0xFFFF, WRAM) end

local last_snes_seq = ru16(0x02)
local claimed = false
local sent, received = 0, 0

print("netbridge: up=" .. UP .. " down=" .. DOWN)
print("netbridge: start tools/gateway.py, then 'net' in SNOS")

emu.addEventCallback(function()
  -- Claim the link only after the kernel has zeroed the mailbox, or net_init()
  -- wipes the magic straight back out.
  if not claimed then
    if ru16(0x00) == MAGIC then
      claimed = true
    elseif emu.read16(0xF000, WRAM) == 0xCAFE then
      wu16(0x00, MAGIC)
      claimed = true
      print("netbridge: link claimed")
    else
      return
    end
  end

  -- SNES -> gateway
  local ss = ru16(0x02)
  if ss ~= last_snes_seq then
    last_snes_seq = ss
    local bytes = {}
    for i = 0, FRAME - 1 do bytes[#bytes + 1] = string.char(rb(0x10 + i)) end
    up:write(table.concat(bytes))
    up:flush()
    sent = sent + 1
  end

  -- gateway -> SNES. One frame per poll, exactly like the cable: handing over
  -- several at once would let this backend outrun what the hardware can do and
  -- hide timing bugs that M7 would then inherit.
  local f = io.open(DOWN, "rb")
  if f then
    f:seek("set", down_off)
    local chunk = f:read(FRAME)
    f:close()
    if chunk and #chunk == FRAME then
      down_off = down_off + FRAME
      for i = 0, FRAME - 1 do wb(0x50 + i, chunk:byte(i + 1)) end
      wu16(0x04, ru16(0x04) + 1)
      received = received + 1
    end
  end
end, emu.eventType.endFrame)
