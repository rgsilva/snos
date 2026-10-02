-- Shared Mesen-testrunner helpers for SNOS.
-- Loaded with dofile("test/lib.lua"); Mesen runs testrunner scripts with the
-- working directory set to wherever it was invoked from, which the Makefile
-- guarantees is snos/snes/.

local M = {}

-- ---------------------------------------------------------------- mailbox
-- kernel/mailbox.h: base $7EF000 => offset 0xF000 in snesWorkRam.
M.MB = 0xF000
M.RING = M.MB + 0x010
M.RING_SIZE = 3552
M.STATUS_RUNNING, M.STATUS_PASS, M.STATUS_PANIC = 0, 1, 0xDEAD

function M.u16(off) return emu.read16(off, emu.memType.snesWorkRam) end
function M.magic() return M.u16(M.MB + 0x000) end
function M.status() return M.u16(M.MB + 0x002) end

function M.ring_text()
  -- The write index is monotonic and the writer stores at (index % size), so
  -- once total output passes RING_SIZE the transcript has to be unrotated:
  -- the OLDEST surviving byte sits at (windex % size), not at 0. The old
  -- clamp-and-read-from-zero returned a transcript with the wrap point
  -- spliced into the middle, which made long-output tests fail (or pass!)
  -- on assertions unrelated to the code under test. Caveat: the index also
  -- wraps at 65536, which 3552 does not divide, so a single test emitting
  -- >64 KB would scramble again -- no current test comes near that.
  local w = M.u16(M.MB + 0x004)
  local out = {}
  if w <= M.RING_SIZE then
    for i = 0, w - 1 do
      out[#out + 1] = string.char(emu.read(M.RING + i, emu.memType.snesWorkRam))
    end
  else
    for i = 0, M.RING_SIZE - 1 do
      out[#out + 1] = string.char(
        emu.read(M.RING + ((w + i) % M.RING_SIZE), emu.memType.snesWorkRam))
    end
  end
  return table.concat(out)
end

-- Feed bytes to the kernel's stdin ring (mailbox in_ring at +0xE00). This is
-- the third kbd_getchar() backend from PLAN.md §1 — how tests drive the shell
-- with no human holding a controller.
M.IN_RING = M.MB + 0xE00
M.IN_RING_SIZE = 512

function M.feed(s)
  local wi = M.u16(M.MB + 0x006)
  for i = 1, #s do
    emu.write(M.IN_RING + (wi % M.IN_RING_SIZE), s:byte(i), emu.memType.snesWorkRam)
    wi = (wi + 1) % 65536
  end
  emu.write16(M.MB + 0x006, wi, emu.memType.snesWorkRam)
end

-- ----------------------------------------------------------------- screen
-- Reconstruct the console text straight out of VRAM. This asserts far more
-- than the mailbox does: that the font uploaded, the tilemaps were written,
-- and the vblank DMA actually landed.
--
-- console.c layout at 64 columns: Mode 5, BG2 tilemap at word $0000 (EVEN
-- text columns), BG1 tilemap at word $0400 (ODD columns). At 32 columns:
-- Mode 0, BG2 alone, one text column per cell; BG1's map is not on screen and
-- is not read. Cell entry = (char << 1) | $2000, so char = (entry & 0x1FF) >> 1.
-- 32 cells x 32 rows, used as a scroll ring. M.COLS is the widest mode.
M.BG2_MAP, M.BG1_MAP = 0x0000, 0x0400
M.COLS, M.ROWS, M.RING_ROWS = 64, 28, 32

-- The console's mode, published by con_publish_mode() in the mailbox's spare
-- bytes at $7EFDF0: low byte the live width, high byte the colour index, and
-- the colour itself as BGR555 at $7EFDF2. Anything but 32 decodes as 64,
-- which is also what a kernel that predates the mode word looks like.
function M.con_cols()
  return emu.read(M.MB + 0xDF0, emu.memType.snesWorkRam) == 32 and 32 or 64
end
function M.con_colour() return M.u16(M.MB + 0xDF2) end

local function map_char(mapword, ring_row, cell)
  local w = mapword + ring_row * 32 + cell
  return (emu.read16(w * 2, emu.memType.snesVideoRam) & 0x1FF) >> 1
end

-- One row of a pair of tilemaps as text, in whichever layout the live mode
-- puts on screen.
local function maps_row(bg2, bg1, row)
  local out = {}
  local wide = M.con_cols() == 64
  for cell = 0, 31 do
    out[#out + 1] = string.char(map_char(bg2, row, cell))
    if wide then out[#out + 1] = string.char(map_char(bg1, row, cell)) end
  end
  return (table.concat(out):gsub("%s+$", ""))
end

-- One screen row as text. ring_row is the tilemap row, which for an unscrolled
-- console is the same as the screen row.
function M.screen_row(ring_row) return maps_row(M.BG2_MAP, M.BG1_MAP, ring_row) end

-- Console scroll state, published by con_publish(): low byte = ring offset of
-- screen row 0, high byte = how many rows the text area currently occupies.
function M.con_state()
  local v = M.u16(M.MB + 0x00C)
  return v & 0xFF, (v >> 8) & 0xFF
end

-- A row as it actually APPEARS on screen, resolving the scroll ring. The plain
-- screen_row() above indexes the tilemap directly, which says what exists but
-- not where it is.
function M.display_row(r)
  local top = M.con_state()
  return M.screen_row((top + r) % M.RING_ROWS)
end

function M.display_text(from_row, to_row)
  local out = {}
  for r = from_row, to_row do out[#out + 1] = M.display_row(r) end
  return (table.concat(out, "\n"):gsub("[\128-\255]", function(c)
    return string.char(c:byte() & 0x7F)
  end))
end

function M.screen_text(nrows)
  local out = {}
  for r = 0, (nrows or M.RING_ROWS) - 1 do out[#out + 1] = M.screen_row(r) end
  return table.concat(out, "\n")
end

-- Same, with the inverse-video bit stripped. Content assertions should use
-- this; whether a glyph is highlighted is a separate question from what the
-- glyph says, and mixing the two makes tests fail for the wrong reason.
function M.screen_text_plain(nrows)
  return (M.screen_text(nrows):gsub("[\128-\255]", function(c)
    return string.char(c:byte() & 0x7F)
  end))
end

-- True if any character in the given screen rows is inverse-video, i.e.
-- something is highlighted.
function M.has_highlight(from_row, to_row)
  for r = from_row, to_row do
    for _, b in ipairs({ M.screen_row(r):byte(1, -1) }) do
      if b >= 0x80 then return true end
    end
  end
  return false
end

-- ------------------------------------------------------------------ panel
-- The keyboard lives in its OWN pair of tilemaps, held still by an HDMA split
-- while the text scrolls above it. So it is read from there, not from the text
-- map — which is exactly the separation that stops it being corrupted.
M.BG2_PANEL, M.BG1_PANEL = 0x0800, 0x0C00
M.PANEL_TOP, M.PANEL_ROWS = 22, 6

function M.panel_row(row) return maps_row(M.BG2_PANEL, M.BG1_PANEL, row) end

function M.panel_text()
  local out = {}
  for r = M.PANEL_TOP, M.PANEL_TOP + M.PANEL_ROWS - 1 do out[#out + 1] = M.panel_row(r) end
  return table.concat(out, "\n")
end

function M.panel_text_plain()
  return (M.panel_text():gsub("[\128-\255]", function(c)
    return string.char(c:byte() & 0x7F)
  end))
end

-- True if any character in the panel is inverse-video, i.e. a key is selected.
function M.panel_has_highlight()
  for r = M.PANEL_TOP, M.PANEL_TOP + M.PANEL_ROWS - 1 do
    for _, b in ipairs({ M.panel_row(r):byte(1, -1) }) do
      if b >= 0x80 then return true end
    end
  end
  return false
end

-- ------------------------------------------------------------- assertions
function M.new_report(name)
  local r = { name = name, failures = {} }
  function r.check(ok, msg) if not ok then r.failures[#r.failures + 1] = msg end end
  function r.finish()
    if #r.failures == 0 then
      print("RESULT PASS " .. r.name)
      emu.stop(0)
    else
      for _, f in ipairs(r.failures) do print("  ! " .. f) end
      print("RESULT FAIL " .. r.name)
      emu.stop(1)
    end
  end
  return r
end

return M
