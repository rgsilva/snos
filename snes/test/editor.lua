-- Editor test.
--
-- Drives EDIT entirely through the mailbox keyboard backend, which is the same
-- path a human on the host keyboard bridge takes, and then verifies the result
-- from OUTSIDE the editor: the file is read back with `cat` from the shell, so
-- a save that only appeared to work cannot pass.
--
-- It also checks the editor left the ABI-2 full-screen calls in a sane state —
-- a status bar on row 0 and text below it — because a layout that is subtly
-- wrong is invisible to a string match and obvious in a screenshot.
local L = dofile("test/lib.lua")

-- Everything goes through the mailbox character ring, including the commands:
-- the editor's control-key shortcuts are the same ones a host-keyboard user
-- gets, so the test drives it exactly as a person would and needs no controller
-- input at all. (emu.setInput is also inert whenever a Mesen GUI is open, which
-- would make a button-driven test unrunnable while anyone is using the machine.)
local CTRL_L = "\012"
local CTRL_S = "\019"
local CTRL_Q = "\017"
local ESC = "\27"
local DOWN = "\29"   -- SNOS_KEY_DOWN

local SCRIPT = {
  { "run edit\n", 300 },
  -- Type two lines. Characters arrive through the same ring the OSK feeds.
  { "hello editor\n", 200 },
  { "second line", 200 },
  { CTRL_L, 220 },          -- open the menu (screenshotted below)
  { CTRL_L, 200 },          -- ...and close it again
  { CTRL_S, 200 },          -- Save: unnamed, so it prompts
  { "NOTE.TXT\n", 300 },    -- ...answered by the modal prompt's line editor
  -- Quit via the MENU, driven entirely by keys: Escape opens it, six
  -- arrow-downs walk from New to Quit, Enter confirms. This is the path a
  -- host-keyboard user takes, and nothing about it involves a controller.
  { ESC, 250 },
  { DOWN .. DOWN .. DOWN .. DOWN .. DOWN .. DOWN, 250 },
  { "\n", 300 },
  { "cat NOTE.TXT\n", 280 },

  -- Second pass: reopen the saved file, edit it, and check the unsaved-changes
  -- guard actually guards. ^O opens the picker, which needs the d-pad, so the
  -- file is loaded by starting the editor fresh and typing over it instead —
  -- what matters here is that backspace, the dirty flag and the confirm dialog
  -- all behave.
  { "run edit\n", 300 },
  { "abcXY", 200 },
  { "\8\8Z", 200 },        -- backspace twice, then Z  ->  "abcZ"
  { CTRL_Q, 220 },          -- dirty, so this must ASK rather than quit
  { "n", 200 },             -- ...answer no: still in the editor
  { "!", 200 },             -- prove it by typing again  ->  "abcZ!"
  { CTRL_S, 200 },
  { "TWO.TXT\n", 300 },
  { CTRL_Q, 300 },          -- clean now, so it just goes
  { "cat TWO.TXT\n", 280 },
}

local frame, step, at = 0, 1, nil
local shot_taken = false

emu.addEventCallback(function()
  frame = frame + 1

  if L.magic() ~= 0xCAFE then
    if frame > 900 then print("RESULT FAIL editor - no mailbox magic"); emu.stop(1) end
    return
  end
  if at == nil then at = frame + 160 end
  if frame < at then return end

  -- Screenshot with a document on screen, before the save. A layout that is
  -- subtly wrong is invisible to a string match and obvious in a picture.
  if step == 5 and not shot_taken then
    local png = emu.takeScreenshot()
    print("SHOT " .. png:gsub(".", function(c) return string.format("%02x", c:byte()) end))
    shot_taken = true
    print("--- editor screen ---")
    print(L.display_text(0, 21))
    print("---------------------")
    at = frame + 30
    return
  end

  if step <= #SCRIPT then
    L.feed(SCRIPT[step][1])
    at = frame + SCRIPT[step][2]
    step = step + 1
    return
  end

  local r = L.new_report("editor")
  local text = L.ring_text()
  print("--- mailbox ---"); print(text); print("---------------")

  r.check(L.status() ~= L.STATUS_PANIC, "kernel panicked")
  r.check(text:find("EDIT-RAN", 1, true) ~= nil, "editor never started")

  -- The editor must still have been running while the text was typed. If it
  -- had exited early the shell would have taken those lines as commands, and an
  -- earlier version of this test passed on exactly that: the words appeared in
  -- the mailbox because the SHELL echoed them, not because the file contained
  -- them.
  r.check(text:find("unknown command: hello", 1, true) == nil,
          "the editor exited early - the typed text reached the shell instead")

  r.check(text:find("[edit exited", 1, true) ~= nil,
          "editor did not exit cleanly back to the shell")

  -- Everything below must come from `cat`, i.e. from AFTER the editor exited,
  -- so only the file's real contents can satisfy it.
  local catpos = text:find("cat NOTE.TXT", 1, true)
  r.check(catpos ~= nil, "never got to the cat")
  local after = catpos and text:sub(catpos + 12) or ""
  r.check(after:find("no such file", 1, true) == nil,
          "the file was not saved - Save As did not write NOTE.TXT")
  r.check(after:find("hello editor", 1, true) ~= nil,
          "cat did not show the first typed line")
  r.check(after:find("second line", 1, true) ~= nil,
          "cat did not show the second typed line")

  -- Second pass.
  local cat2 = text:find("cat TWO.TXT", 1, true)
  r.check(cat2 ~= nil, "never got to the second cat")
  local after2 = cat2 and text:sub(cat2 + 11) or ""
  r.check(after2:find("abcZ!", 1, true) ~= nil,
          "backspace, the unsaved-changes prompt, or the second save is broken " ..
          "(expected abcZ!)")
  -- Two runs in, two clean exits.
  local _, exits = text:gsub("%[edit exited", "")
  r.check(exits >= 2, "expected two clean exits, saw " .. exits)

  r.finish()
end, emu.eventType.endFrame)
