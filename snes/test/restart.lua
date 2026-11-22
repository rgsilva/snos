-- Restart through the shell, with live process/network state and a saved file.
-- Keep each transcript: mb_init resets both rings, so an old banner alone
-- cannot prove that a second boot happened. No emu.reset() is used here.
local L = dofile("test/lib.lua")
local G = dofile("test/gwip.lua")
local SRAM_KB = tonumber("@SRAM_KB@") -- spliced by the Makefile, as in fs.lua

local r = L.new_report("restart")
local frame, step, at = 0, 1, 240
local gw, sock = nil, nil
local boot_text, boot_screen, before_restart
local watching, reset_seen, old_windex = false, false, 0
local transcripts = {}

local function send(cmd, settle)
  L.feed(cmd)
  at = frame + settle
end

local function contains(needle)
  return L.ring_text():find(needle, 1, true) ~= nil
end

local function restart()
  before_restart = L.ring_text()
  transcripts[#transcripts + 1] = before_restart
  old_windex = L.u16(L.MB + 4)
  reset_seen, watching = false, true
  gw = nil -- the link mailbox is also reinitialised; re-claim after boot
  send("ReStArT\n", 360)
end

local function check_boot()
  watching = false
  r.check(reset_seen, "mailbox write index never reset")
  r.check(L.magic() == 0xCAFE, "mailbox magic lost across restart")
  r.check(L.u16(L.MB + 6) == 0 and L.u16(L.MB + 8) == 0,
          "input ring inherited the previous session")
  r.check(L.ring_text() == boot_text,
          "second boot transcript differs (banner, storage, prompt or stack)")
  r.check(L.screen_text_plain(8) == boot_screen,
          "restarted console differs from power-on in VRAM")
  r.check(L.u16(0x1A16) == 0 and L.u16(0x1A18) == 0 and
          L.u16(0x1A22) == 0, "exec/scheduler state survived restart")
  r.check(L.u16(0xE000) == 0, "link mailbox still claims the old peer")
  transcripts[#transcripts + 1] = L.ring_text()
end

local function dial_echo(payload)
  r.check(gw.hello_seen, "fresh network stack did not send HELLO")
  r.check(gw.fwd_port == 7777, "background server did not listen")
  if gw.fwd_port then
    sock = gw:dial(gw.fwd_port)
    sock.on_established = function(t) t:send(payload) end
  end
end

emu.addEventCallback(function()
  frame = frame + 1
  if watching and L.u16(L.MB + 4) < old_windex then reset_seen = true end
  if frame > 6500 or L.status() == L.STATUS_PANIC then
    print(L.ring_text())
    r.check(false, "timeout or kernel panic")
    r.finish()
    return
  end
  if L.magic() ~= 0xCAFE then return end
  if gw then gw:service() end
  if frame < at then return end

  if step == 1 then
    boot_text, boot_screen = L.ring_text(), L.screen_text_plain(8)
    r.check(contains("v0.1") and contains(SRAM_KB .. "K SRAM") and
            contains("type 'help' for commands"), "initial boot incomplete")
    gw = G.new{}
    gw:claim()
    send("write RESTART.TXT survived both restarts\n", 240)
    step = 2; return
  end
  if step == 2 then
    r.check(contains("wrote RESTART.TXT"), "fixture was not written")
    send("run netecho 7777 &\n", 360)
    step = 3; return
  end
  if step == 3 then
    dial_echo("before restart\n")
    send("jobs\n", 240)
    step = 4; return
  end
  if step == 4 then
    r.check(contains("netecho  running"), "no live job before restart")
    r.check(sock and sock.rx:find("before restart\n", 1, true),
            "no live TCP session before restart")
    restart()
    step = 5; return
  end
  if step == 5 then
    check_boot()
    send("jobs\ncat RESTART.TXT\ndebug syscheck\n", 300)
    step = 6; return
  end
  if step == 6 then
    r.check(contains("no background jobs"), "background job survived restart")
    r.check(contains("survived both restarts"), "file lost on first restart")
    r.check(contains("syscheck done"), "COP/prompt broken after restart")
    gw = G.new{}
    gw:claim()
    sock = nil
    at = frame + 120 -- let the new peer's IP configuration reach the console
    step = 6.5; return
  end
  if step == 6.5 then
    send("run netecho 7777 &\n", 360)
    step = 7; return
  end
  if step == 7 then
    dial_echo("after restart\n")
    send("jobs\n", 240)
    step = 8; return
  end
  if step == 8 then
    r.check(contains("netecho  running"), "process slot cannot be reused")
    r.check(sock and sock.rx:find("after restart\n", 1, true),
            "TCP did not recover after restart")
    send("run pad\n", 180)
    step = 9; return
  end
  if step == 9 then
    r.check(contains("PAD-RAN"), "foreground app did not start")
    emu.write16(L.MB + 0x00E, 1, emu.memType.snesWorkRam)
    at = frame + 180
    step = 10; return
  end
  if step == 10 then
    r.check(contains("[pad killed]"), "keyboard break did not unwind the app")
    restart()
    step = 11; return
  end
  if step == 11 then
    check_boot()
    send("jobs\ncat RESTART.TXT\nwrite AFTER.TXT writable again\n", 300)
    step = 12; return
  end
  if step == 12 then
    r.check(contains("no background jobs"), "job survived second restart")
    r.check(contains("survived both restarts"), "file lost on second restart")
    r.check(contains("wrote AFTER.TXT"), "filesystem cannot write after restart")
    send("cat AFTER.TXT\nrun hello\n", 300)
    step = 13; return
  end

  r.check(contains("writable again"), "new file cannot be read back")
  r.check(contains("[hello exited 42]"), "app exec/exit broken after restart")
  transcripts[#transcripts + 1] = L.ring_text()
  for i, text in ipairs(transcripts) do
    print("--- session " .. i .. " ---"); print(text)
  end
  r.finish()
end, emu.eventType.endFrame)
