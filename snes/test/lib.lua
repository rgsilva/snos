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
  local n = M.u16(M.MB + 0x004)
  if n > M.RING_SIZE then n = M.RING_SIZE end
  local out = {}
  for i = 0, n - 1 do
    out[#out + 1] = string.char(emu.read(M.RING + (i % M.RING_SIZE), emu.memType.snesWorkRam))
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
-- console.c layout: Mode 5, BG2 tilemap at word $0000 (EVEN text columns),
-- BG1 tilemap at word $0400 (ODD columns). Cell entry = (char << 1) | $2000,
-- so char = (entry & 0x1FF) >> 1. 32 cells x 32 rows, used as a scroll ring.
M.BG2_MAP, M.BG1_MAP = 0x0000, 0x0400
M.COLS, M.ROWS, M.RING_ROWS = 64, 28, 32

local function map_char(mapword, ring_row, cell)
  local w = mapword + ring_row * 32 + cell
  return (emu.read16(w * 2, emu.memType.snesVideoRam) & 0x1FF) >> 1
end

-- One screen row as text. ring_row is the tilemap row, which for an unscrolled
-- console is the same as the screen row.
function M.screen_row(ring_row)
  local out = {}
  for cell = 0, 31 do
    out[#out + 1] = string.char(map_char(M.BG2_MAP, ring_row, cell))
    out[#out + 1] = string.char(map_char(M.BG1_MAP, ring_row, cell))
  end
  return (table.concat(out):gsub("%s+$", ""))
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
