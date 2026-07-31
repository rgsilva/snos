-- How long does the NMI handler actually take?
--
-- Append this to a test or to the bridge and load the result, because Mesen runs
-- one script at a time:
--
--   cat build/test/vnc.lua tools/nmiprof.lua > build/test/vncprof.lua
--   sh tools/runtest.sh "$MESEN" build/snos.sfc build/test/vncprof.lua
--
-- Use the VNC test, not the net test. net_service() is called from exactly one
-- place — traps.c:81, inside NMI — and it is gated on !con_dirty_pending(), so
-- while the shell is drawing it is mostly SKIPPED. The expensive case is an
-- application owning the PPU, where con_dirty_pending() returns 0 by design and
-- the link runs on every single frame. Profiling the net test measures the cheap
-- path and reports that there is nothing to fix.
--
-- WHY THIS EXISTS. The vnc work found the handler overrunning into the next
-- frame — which is why an application on this machine has no usable vblank (see
-- apps/vnc/vnc.c). Before optimising it, measure it: the frame copy and the two
-- LINK_FRAME-sized checksums are equal in byte count, so "MVN the copy" can only
-- address about half, and guessing which half is how the display bugs got fixed
-- twice.
--
-- ZERO KERNEL CHANGES, deliberately. Both addresses come from build/snos.map
-- plus a byte count verified against the ROM image, so nothing here perturbs
-- what it is measuring:
--
--   $00:8124  snos_nmi   -- map: "snos_nmi = 008124"
--   $00:8158  the rti    -- verified by decoding the shim from the ROM image:
--                           all three `beq nmi_ret` displacements ($8140+2+0x11,
--                           $8146+2+0x0B, $814D+2+0x04) land on $8153, the five
--                           pulls run $8153-$8157, and $8158 is $40. Re-derive
--                           BOTH whenever calypsi/header.s changes.
--
-- If calypsi/header.s changes, these go stale. The pairing counters below are the
-- guard: entries and exits must track 1:1, and a wrong address shows up as zero
-- samples rather than as plausible nonsense.
--
-- MEASURE WITH masterClock, NOT WITH SCANLINE ARITHMETIC. This cost two runs.
-- Mesen advances frameCount at the START of vblank, which is exactly when NMI
-- fires, so a handler that runs into the next frame reports the SAME frame number
-- with a LOWER scanline. Spans then come out negative and a "sanity" guard
-- discards them — silently throwing away precisely the long spans this exists to
-- find. The first version reported worst=10 scanlines with n frozen at 175 of
-- 12000 NMIs, which looks like a working instrument. masterClock is monotonic.

local NMI_ENTRY = 0x8124
local NMI_RTI = 0x8158
local PER_LINE = 1364 -- master cycles per NTSC scanline
local PER_CPU = 6     -- master cycles per 3.58 MHz CPU cycle

local open_clk = nil
local n, sum, worst, best = 0, 0, -1, 1 / 0
local entries, exits, nested, maxdepth, depth = 0, 0, 0, 0, 0

-- A bimodal distribution is the expected shape, so buckets rather than a mean:
-- cheap NMIs (link idle, or net_service skipped) interleave with expensive ones,
-- and an average of the two describes neither. Bucket edges are in SCANLINES.
local EDGES = { 8, 16, 32, 64, 128, 262 }
local hist = {}
for i = 1, #EDGES + 1 do hist[i] = 0 end

local function clk()
  local ok, st = pcall(emu.getState)
  if not ok or not st then return nil end
  return st["masterClock"]
end

local function report(tag)
  if n == 0 then
    print(string.format("nmiprof: %s NO SAMPLES (entries=%d exits=%d) -- check the addresses",
      tag, entries, exits))
    return
  end
  local b = {}
  for i = 1, #EDGES do b[#b + 1] = string.format("<=%d:%d", EDGES[i], hist[i]) end
  b[#b + 1] = string.format(">%d:%d", EDGES[#EDGES], hist[#hist])
  print(string.format(
    "nmiprof: %s n=%d mean=%.1f best=%.1f worst=%.1f lines (worst %.0f cpu cycles) | %s | pairs %d/%d re-entry %d depth %d",
    tag, n, sum / n / PER_LINE, best / PER_LINE, worst / PER_LINE, worst / PER_CPU,
    table.concat(b, " "), entries, exits, nested, maxdepth))
end

emu.addMemoryCallback(function()
  entries = entries + 1
  depth = depth + 1
  if depth > maxdepth then maxdepth = depth end
  if depth > 1 then
    -- Re-entry is itself a finding: NMI is edge-triggered and the I flag does not
    -- mask it, so a handler that overruns its frame can be re-entered while still
    -- running. That is the suspected mechanism behind the LINK_FRAME 256 panic
    -- (docs/performance.md). The outer call keeps the measurement.
    nested = nested + 1
    return
  end
  open_clk = clk()
end, emu.callbackType.exec, NMI_ENTRY, NMI_ENTRY, emu.cpuType.snes)

emu.addMemoryCallback(function()
  exits = exits + 1
  local was = depth
  if depth > 0 then depth = depth - 1 end
  if was ~= 1 then return end
  local now = clk()
  if not (now and open_clk) then return end
  local span = now - open_clk
  open_clk = nil
  if span < 0 then return end
  n = n + 1
  sum = sum + span
  if span > worst then worst = span end
  if span < best then best = span end
  local lines = span / PER_LINE
  local placed = false
  for i = 1, #EDGES do
    if not placed and lines <= EDGES[i] then hist[i] = hist[i] + 1; placed = true end
  end
  if not placed then hist[#hist] = hist[#hist] + 1 end
  if n % 2000 == 0 then report("@" .. n) end
end, emu.callbackType.exec, NMI_RTI, NMI_RTI, emu.cpuType.snes)

print(string.format("nmiprof: watching $%04X -> $%04X", NMI_ENTRY, NMI_RTI))
