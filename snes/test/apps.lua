-- M3 test: the gate.
--
-- Proves the whole premise of the design — that an application living in a
-- cartridge ROM bank can be entered, can call back into the kernel, and can
-- give control back — plus the thing that makes it safe to try: the kill
-- switch, which recovers the machine from an app that never asks to stop.
--
--   1. `apps`       the ROM is self-describing: apps found by scanning banks
--   2. `run hello`  exec, syscalls from a separate link, clean exit code 42
--   3. `run pad`    exec an app that loops forever...
--   4. START+SELECT ...and take the machine back from it without its help
--   5. `ver`        the shell is alive and sane afterwards
local L = dofile("test/lib.lua")

local frame = 0
local step = 1
local at = nil
local kill_from, kill_to = nil, nil

-- The kill switch requires the chord held for SNOS_KILL_FRAMES (45) frames, so
-- hold it well past that.
emu.addEventCallback(function()
  if kill_from and frame >= kill_from and frame < kill_to then
    emu.setInput({ start = true, select = true }, 0)
  end
end, emu.eventType.inputPolled)

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL apps - no mailbox magic"); emu.stop(1) end
    return
  end

  if at == nil then at = frame + 120 end
  if frame < at then return end

  if step == 1 then send("apps\n", 180); step = 2; return end
  -- uppercase on purpose: the app is registered as "hello"
  if step == 2 then send("run HELLO motd.txt\n", 240); step = 2.5; return end
  -- launched again with NO argument, to prove the buffer is cleared between runs
  if step == 2.5 then send("run hello\n", 240); step = 3; return end
  if step == 3 then
    send("run pad\n", 400)
    -- Let pad get going, then hold the kill chord.
    kill_from = frame + 150
    kill_to = kill_from + 120
    step = 4
    return
  end
  -- Kill a hung app with the KEYBOARD break rather than the controller chord.
  -- This is the path that matters: Ricardo runs with the controller unplugged,
  -- so the chord is not merely inconvenient, it is unavailable.
  if step == 4 then send("run pad\n", 220); step = 4.5; return end
  if step == 4.5 then
    emu.write16(L.MB + 0x00E, 1, emu.memType.snesWorkRam) -- what F12 writes
    at = frame + 220
    step = 4.75
    return
  end
  if step == 4.75 then send("debug ver\n", 200); step = 5; return end

  local r = L.new_report("apps")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")

  -- 1. bank scan
  r.check(text:find("hello  bank $00C8", 1, true) ~= nil,
          "app scan did not find 'hello' in bank $C8")
  r.check(text:find("pad  bank $00C9", 1, true) ~= nil,
          "app scan did not find 'pad' in bank $C9")
  -- The scan must not re-find the same apps in the ROM's mirrors.
  local _, hits = text:gsub("hello  bank", "")
  r.check(hits == 1, "app scan found 'hello' " .. hits .. " times - mirrored banks?")

  -- 2. exec, syscalls from a separately linked binary, and the exit code
  r.check(text:find("hello from a SNOS app", 1, true) ~= nil,
          "hello did not run, or SYS_PUTS from an app bank is broken")
  r.check(text:find("HELLO-RAN", 1, true) ~= nil, "hello's SYS_TRACE never arrived")

  -- ABI 4 launch arguments, and the half that is easy to get wrong: an app
  -- given no argument must see an empty string, not the previous launch's.
  r.check(text:find("args: [motd.txt]", 1, true) ~= nil,
          "launch argument did not reach the application")
  r.check(text:find("args: (none)", 1, true) ~= nil,
          "argument buffer not cleared between launches - stale argv")
  r.check(text:find("[hello exited 42]", 1, true) ~= nil,
          "hello's exit code did not survive the return to the shell")

  -- 3/4. the kill switch: pad never asks to stop, so this is the kernel
  -- taking the machine back on its own.
  r.check(text:find("PAD-RAN", 1, true) ~= nil, "pad never started")
  -- Twice: once by the controller chord, once by the keyboard break.
  local _, kills = text:gsub("%[pad killed%]", "")
  r.check(kills >= 2,
          "expected two kills (controller chord and keyboard break), saw " .. kills ..
          " - a wedged app would lock the machine for whichever path is missing")

  -- 5. the shell survived having an app killed out from under it
  -- Version-agnostic on purpose: this asserts the shell still RESPONDS after an
  -- app was killed out from under it. Pinning the ABI number here means every
  -- bump breaks a test that has nothing to do with the ABI, which has now
  -- happened twice.
  r.check(text:find("kernel ABI ", 1, true) ~= nil,
          "shell is not responding after the kill - the unwind corrupted it")

  r.finish()
end, emu.eventType.endFrame)
