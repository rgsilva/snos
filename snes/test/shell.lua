-- M2 shell test: drive the shell through the mailbox and check each builtin,
-- with `syscheck` as the real target — it is the only thing that exercises the
-- COP trap end to end (signature read-back, frame decoding, return value).
local L = dofile("test/lib.lua")

local CMDS = {
  "debug ver\n",
  "mem\n",
  "echo hello from the shell\n",
  "debug syscheck\n",
  "bogus\n",
  "help\n",
  -- Deliberately mangled case: typing uppercase means holding shift on an
  -- on-screen keyboard, so the shell must not care.
  "EcHo MIXEDCASE WORKS\n",
  "LS\n",
}

local frame = 0
local sent = 0
local next_at = nil
local done_at = nil

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL shell - no mailbox magic"); emu.stop(1) end
    return
  end

  if next_at == nil then next_at = frame + 120 end

  -- One command at a time, with enough slack for the console to settle (it
  -- flushes one row per frame).
  if sent < #CMDS then
    if frame >= next_at then
      L.feed(CMDS[sent + 1])
      sent = sent + 1
      next_at = frame + 200
    end
    return
  end

  if done_at == nil then done_at = frame + 240 end
  if frame < done_at then return end

  local r = L.new_report("shell")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(text:find("v0.1", 1, true) ~= nil, "kernel banner missing")

  -- builtins
  -- Version-agnostic: pinning the number here breaks this test on every ABI
  -- bump, which has now happened three times across three different files.
  r.check(text:find("kernel ABI ", 1, true) ~= nil, "ver did not report the ABI version")
  r.check(text:find("bank $00", 1, true) ~= nil, "mem did not report the bank-$00 map")
  r.check(text:find("hello from the shell", 1, true) ~= nil, "echo did not echo")
  r.check(text:find("unknown command: bogus", 1, true) ~= nil,
          "unknown command was not rejected")
  r.check(text:find("debug", 1, true) ~= nil, "help did not list commands")

  -- syscheck: every one of these came back through a COP trap.
  -- `syscheck` compares the syscall's answer against the header it was compiled
  -- against and prints "ok" or "BAD", so matching on "ok" checks the thing that
  -- matters without hard-coding the number a second time.
  r.check(text:find("version=", 1, true) ~= nil and text:find(" ok", 1, true) ~= nil,
          "SYS_VERSION returned the wrong value - COP signature read-back is broken")
  r.check(text:find("putc=Z", 1, true) ~= nil, "SYS_PUTC did not print")
  r.check(text:find("puts=puts-ok", 1, true) ~= nil,
          "SYS_PUTS did not print - far pointer argument passing is broken")
  r.check(text:find("putu=12345", 1, true) ~= nil, "SYS_PUTU wrong")
  r.check(text:find("puthex=BEEF", 1, true) ~= nil, "SYS_PUTHEX wrong")
  r.check(text:find("syscheck done", 1, true) ~= nil, "syscheck did not complete")

  -- SYS_TRACE writes straight to the mailbox, bypassing the console: proves a
  -- syscall can reach a kernel service the console never touches.
  r.check(text:find("SYSCHECK-TRACE", 1, true) ~= nil, "SYS_TRACE did not reach the mailbox")

  -- The frame counter must be advancing, i.e. NMI is alive under the shell.
  local f = text:match("frames=(%d+)")
  r.check(f ~= nil and tonumber(f) > 0, "SYS_FRAMES returned 0 - is NMI running?")

  -- Commands are case-insensitive, like filenames and application names.
  r.check(text:find("MIXEDCASE WORKS", 1, true) ~= nil,
          "mixed-case 'EcHo' was not recognised as a command")
  r.check(text:find("unknown command: LS", 1, true) == nil,
          "uppercase 'LS' was rejected")
  r.check(text:find("README.TXT", 1, true) ~= nil, "uppercase 'LS' did not list files")

  r.finish()
end, emu.eventType.endFrame)
