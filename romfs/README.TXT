SNOS - a native 65816 operating system for the Super Nintendo.

This file lives in the cartridge ROM and is read straight out of it: there
is no disk, and nothing was copied into RAM to show it to you.

Applications work the same way. A program is simply code sitting in a ROM
bank, and running it means pointing the stack and data bank at it and
jumping. That is why 'run' is instant.

Files you create yourself go to the 256 KB of battery-backed cartridge
SRAM, and survive a reset. Try:

  write NOTES.TXT hello there
  ls
  cat NOTES.TXT

Hold START+SELECT to kill a running application.
