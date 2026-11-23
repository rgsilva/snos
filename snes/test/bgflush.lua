-- A background launch flushes the console before its one-second argv wait,
-- so the new job's first datagram is not held while a row is still owed.
--
-- With the screen full every new line scrolls it, so `help` right before the
-- launch leaves rows owed when it begins. `get` sends its DNS query at
-- once; the peer swallows it and notes the frame it arrived on. Unflushed,
-- the query waits out the whole argv wait (60 frames and more).
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")

local gw = G.new{}
local frame, step, at = 0, 0, 200
local fed, seen = nil, nil
local gaps = {}
local BOUND = 30
local r = L.new_report("bgflush")

gw.observer = function(p)
  if p:byte(10) == 17 and fed and not seen then seen = frame end
  return true
end

-- help's last line; once it is out the launch line is next.
local function helps() return select(2, L.ring_text():gsub("debug       diagnostics", "")) end
local base

local function launch(cmd)
  fed, seen, base = nil, nil, helps()
  L.feed(cmd)
  at = frame + 250
end

emu.addEventCallback(function()
  frame = frame + 1
  if frame > 3000 or L.status() == L.STATUS_PANIC then
    print(L.ring_text())
    r.check(false, "timeout or panic at step " .. step)
    r.finish()
    return
  end
  if L.magic() ~= 0xCAFE then return end
  if step > 0 then gw:service() end
  if base and not fed and helps() > base then fed = frame end
  if frame < at then return end

  if step == 0 then gw:claim(); at = frame + 200; step = 0.5; return end
  if step == 0.5 then L.feed("help\nhelp\n"); at = frame + 200; step = 1; return end
  if step == 1 then launch("help\nrun get http://a.test/ - &\n"); step = 2; return end
  if step == 2 then
    gaps[1] = seen and seen - fed
    L.feed("kill 1\njobs\n"); at = frame + 150; step = 3; return
  end
  if step == 3 then launch("help\nget http://b.test/ - &\n"); step = 4; return end
  if step == 4 then
    gaps[2] = seen and seen - fed
    L.feed("kill 1\njobs\n"); at = frame + 150; step = 5; return
  end

  print("first datagram after help finished: run form " .. tostring(gaps[1]) ..
        " frames, bare form " .. tostring(gaps[2]) .. " frames")
  for i, name in ipairs{"run NAME &", "NAME &"} do
    r.check(gaps[i] ~= nil, name .. ": no DNS query ever reached the peer")
    r.check(gaps[i] and gaps[i] < BOUND, name .. ": first datagram took " ..
            tostring(gaps[i]) .. " frames, bound " .. BOUND)
  end
  r.finish()
end, emu.eventType.endFrame)
