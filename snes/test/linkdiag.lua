-- The link diagnostic's panel (LINKDIAG=1, kernel/linkdiag.c) under the
-- adapter model of test/hwport.lua: the reference a photo of the cart is
-- read against. Boots, leaves the console at its prompt for CONFIG.seconds,
-- prints the panel's rows and the model's own count of exchanges, and checks
-- the panel is there and agrees with the model: with the adapter answering
-- the link is up and every exchange it saw is one the console counted; with
-- LDIAG="adapter=false" the probes are clocked and nothing checksums.
-- `make LINK_HW=1 LINKDIAG=1 test-linkdiag` (also with SA1=1).
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")
local P = dofile("test/hwport.lua")

local CONFIG = {}
-- @CONFIG@ (the Makefile inserts `CONFIG = {...}` here)
local adapter = CONFIG.adapter ~= false
local gw = G.new{addr = "10.0.2.99", frame = 64, mss = 1460, rto_polls = 150}
local port = adapter and P.new(gw, {}) or { exchanges = 0 }
local r = L.new_report("linkdiag")
local ROW, ROWS = 13, 9

local function field(text, pat) return tonumber(text:match(pat)) end

local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if frame < (CONFIG.seconds or 10) * 60 then return end
  local rows = {}
  for i = 0, ROWS - 1 do rows[#rows + 1] = L.display_row(ROW + i) end
  for _, t in ipairs(rows) do print("| " .. t) end
  print(string.format("model: %d exchanges", port.exchanges))
  local all = table.concat(rows, "\n")
  r.check(rows[1]:match("^LINKDIAG"), "no panel at row " .. ROW)
  local clk, up = field(all, " clk (%d+)"), field(all, " up(%d)")
  local ok, net = field(all, "\nok (%d+)"), field(all, " net (%d+)")
  r.check(clk and net and net > 0, "the NMI never ran net_service")
  if adapter then
    r.check(up == 1, "link not up")
    r.check(ok and ok > 0, "no reply checksummed")
    -- The panel is a second old at most: it can lag the model, never lead it.
    r.check(clk and clk <= port.exchanges and clk + 150 >= port.exchanges,
            string.format("clk %s against %d exchanges", tostring(clk), port.exchanges))
  else
    r.check(up == 0, "link up with no adapter")
    r.check(clk and clk > 0, "no probe clocked")
    r.check(ok == 0, "a reply checksummed with no adapter")
  end
  r.finish()
end, emu.eventType.endFrame)
