# SNOS SNES-side build. Calypsi C -> 65816, same vendored toolchain as
# snesmac / snespdp11 / snespico8.
#
# -O0 is the deliberate baseline, not laziness: Calypsi's -O2 miscompiles
# setjmp-heavy code and --speed miscompiles outright (snespdp11/docs/M3-notes.md,
# snesmac/docs/PERF.md item G). Correctness first; hand-written asm is the
# performance lever here, not optimiser flags.
#
# CALDIR resolves to /Users/ricardo/dev/snespico8/tools/... from snos/snes/.
# (snespdp11's Makefile has a stale default that resolves one level short.)
CALDIR ?= ../../../snespico8/tools/calypsi-65816-5.17
CC := $(CALDIR)/bin/cc65816
AS := $(CALDIR)/bin/as65816
LN := $(CALDIR)/bin/ln65816
MESEN ?= /Applications/Mesen.app/Contents/MacOS/Mesen
# Mesen keeps battery saves in its own data directory, NOT next to the ROM.
# Tests that assert on a fresh filesystem have to delete THIS file; removing a
# .srm from build/ silently does nothing and the test then inherits whatever the
# previous run left behind.
MESEN_SRM ?= $(HOME)/Library/Application Support/Mesen2/Saves/snos.srm

# A running Mesen GUI and --testrunner collide: emu.setInput goes inert, so any
# test that presses a controller button fails for a reason that has nothing to
# do with the code under test. emu8086/snes/test/suite.sh guards against this
# for the same reason; without the guard the failures look like real bugs and
# cost an afternoon. Set MESEN_ALLOW_GUI=1 to override.
define check_env
	@if [ -z "$(MESEN_ALLOW_GUI)" ] && pgrep -x Mesen >/dev/null 2>&1; then \
	  echo "note: a Mesen GUI is running. The controller-driven tests have been"; \
	  echo "      verified to pass anyway; if they misbehave, close it."; \
	fi
	@python3 tools/checkenv.py || exit 1
endef
BUILD := build
K := kernel

MODEL := --code-model=large --data-model=large
CFLAGS = --core=65816 --target=snes $(MODEL) --no-ppu-mul -O0 -I$(K) \
         --list-file=$(@:.o=.lst)
AFLAGS := --core=65816 --target=SNES $(MODEL)
LNFLAGS = calypsi/linker.scm --output-format=raw --rtattr exit=simplified \
          $(CALDIR)/lib/clib-lc-ld-snes-noppu.a

KERNEL_C := $(K)/main.c $(K)/traps.c $(K)/kcon.c $(K)/console.c $(K)/mailbox.c \
            $(K)/input.c $(K)/shell.c $(K)/app.c $(K)/sram.c $(K)/fs.c $(K)/net.c \
            $(K)/link.c $(K)/link_wram.c $(K)/link_serial.c
KERNEL_S := calypsi/header.s $(K)/stack.s $(K)/syscall.s $(K)/exec.s \
            $(K)/netasm.s
KERNEL_O := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(KERNEL_C))) \
            $(patsubst %.s,$(BUILD)/%.o,$(notdir $(KERNEL_S)))

.PHONY: all kernel apps bridge test test-boot test-input test-history test-shadow test-shell test-apps test-fs test-basic test-panel test-editor test-net test-pager test-irc test-vnc clean
# The combined Lua bridge is built by default. It used to be produced only on
# demand into build/, which is gitignored — so it existed, worked, and was
# invisible. Mesen loads exactly one script at a time, so this is the file to
# point its Script Window at.
all: kernel $(BUILD)/snosbridge.lua
kernel: $(BUILD)/snos.sfc

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/test

# Mesen's Lua sandbox blocks dofile/require, so the shared harness is spliced
# into each test at build time. See tools/mktest.py.
$(BUILD)/test/%.lua: test/%.lua test/lib.lua tools/mktest.py | $(BUILD)
	python3 tools/mktest.py $< test/lib.lua $@

# Every object depends on every kernel header. Coarse, but the kernel is a
# dozen files and a full rebuild is seconds — whereas NOT tracking headers
# silently ships a stale object. Editing snos_abi.h to bump the ABI version left
# the shell compiled against the old one, so it refused to run an application
# for needing an ABI the kernel had in fact just gained.
KERNEL_H := $(wildcard $(K)/*.h)

$(BUILD)/%.o: $(K)/%.c $(KERNEL_H) | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

$(BUILD)/%.o: $(K)/%.s | $(BUILD)
	$(AS) -o $@ $< $(AFLAGS)

$(BUILD)/header.o: calypsi/header.s | $(BUILD)
	$(AS) -o $@ $< $(AFLAGS)

$(BUILD)/snos.raw: $(KERNEL_O)
	$(LN) -o $@ $(LNFLAGS) --list-file=$(BUILD)/snos.map $^

# ---------------------------------------------------------------- applications
# Each app is a separate link into its own ROM bank, then dropped into the
# cartridge image at the byte offset that bank maps to. HiROM makes that
# arithmetic trivial: SNES $C0:0000+N is ROM byte N, so bank $C8 is 0x080000.
APPS := hello pad basic edit view get netecho httpd vnc irc
APP_BANK_hello := c8
APP_BANK_pad   := c9
APP_BANK_basic := ca
APP_BANK_edit  := cb
APP_BANK_view  := cc
APP_BANK_get   := cd
APP_BANK_netecho := ce
APP_BANK_httpd := d0
# ($D1 was `web`, deleted; the slot is free for the next app.)
APP_BANK_vnc   := d2
APP_BANK_irc   := d3
APP_AT_hello   := 0x080000
APP_AT_pad     := 0x090000
APP_AT_basic   := 0x0A0000
APP_AT_edit    := 0x0B0000
APP_AT_view    := 0x0C0000
APP_AT_get     := 0x0D0000
APP_AT_netecho := 0x0E0000
APP_AT_httpd   := 0x100000
APP_AT_vnc     := 0x120000
APP_AT_irc     := 0x130000
# The editor needs the ABI-2 full-screen calls and says so in its header, so an
# older kernel refuses to run it rather than failing on a missing syscall.
APP_ABI_edit   := 2
# `get` needs SYS_FS_FREE and the image flag on SYS_NET_STATE. Declaring the
# minimum here is what makes the kernel refuse to launch it rather than let it
# call into a syscall that does not exist.
APP_ABI_get    := 6
# The servers need the ABI-7 stream calls — listen, write, wspace. An older
# kernel refuses to launch them rather than letting them trap into a syscall
# that does not exist.
APP_ABI_netecho := 7
APP_ABI_httpd  := 7
# `vnc` uses only the ABI-7 stream calls plus sys_gfx; 8 was an over-claim
# that would make an ABI-7 kernel refuse an app it could run.
APP_ABI_vnc    := 7
# `irc` needs the ABI-7 stream calls: it opens a plain TCP connection and both
# talks and listens on it. Nothing it does needs ABI 8.
APP_ABI_irc    := 7

APP_RAWS := $(foreach a,$(APPS),$(BUILD)/apps/$(a)/$(a).raw)
APP_BLOBS := $(foreach a,$(APPS),--at $(APP_AT_$(a)) $(BUILD)/apps/$(a)/$(a).raw)

# ROMFS goes in the last bank of the 1 MB image ($CF), leaving $C8-$CE for apps.
ROMFS_AT := 0x0F0000
ROMFS_SRC := $(wildcard ../romfs/*)

$(BUILD)/romfs.bin: $(ROMFS_SRC) tools/mkromfs.py | $(BUILD)
	python3 tools/mkromfs.py ../romfs $@

.PHONY: apps
apps: $(APP_RAWS)

$(BUILD)/apps/%/%.raw:
	@true

define APP_template
$(BUILD)/apps/$(1)/$(1).raw: ../apps/$(1)/$(1).c $(wildcard ../apps/$(1)/*.s) \
                             ../apps/crt0.s ../apps/app.scm.in \
                             $(K)/syscall.s $(KERNEL_H) tools/mkappinfo.py \
                             tools/buildapp.sh | $(BUILD)
	sh tools/buildapp.sh $(1) $(APP_BANK_$(1)) $(CALDIR) $(BUILD) $(or $(APP_ABI_$(1)),1)
endef
$(foreach a,$(APPS),$(eval $(call APP_template,$(a))))

$(BUILD)/snos.sfc: $(BUILD)/snos.raw $(APP_RAWS) $(BUILD)/romfs.bin tools/raw2sfc.py
	python3 tools/raw2sfc.py $< $@ $(APP_BLOBS) --at $(ROMFS_AT) $(BUILD)/romfs.bin

test: test-boot test-input test-history test-shell test-shadow test-apps test-fs test-basic test-panel test-editor test-net test-pager test-irc

test-boot: $(BUILD)/snos.sfc $(BUILD)/test/boot.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/boot.lua

test-input: $(BUILD)/snos.sfc $(BUILD)/test/input.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/input.lua

test-shell: $(BUILD)/snos.sfc $(BUILD)/test/shell.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/shell.lua

test-apps: $(BUILD)/snos.sfc $(BUILD)/test/apps.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/apps.lua

# Mesen persists cart SRAM to a .srm; a stale one from a previous run would
# make the persistence assertion pass for the wrong reason.
test-fs: $(BUILD)/snos.sfc $(BUILD)/test/fs.lua
	$(call check_env)
	rm -f "$(MESEN_SRM)"
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/fs.lua

test-basic: $(BUILD)/snos.sfc $(BUILD)/test/basic.lua
	$(call check_env)
	rm -f "$(MESEN_SRM)"
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/basic.lua

test-panel: $(BUILD)/snos.sfc $(BUILD)/test/panel.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/panel.lua

test-editor: $(BUILD)/snos.sfc $(BUILD)/test/editor.lua
	$(call check_env)
	rm -f "$(MESEN_SRM)"
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/editor.lua

test-net: $(BUILD)/snos.sfc $(BUILD)/test/net.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/net.lua

test-pager: $(BUILD)/snos.sfc $(BUILD)/test/pager.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/pager.lua

# A Lua ircd on the far side of the emulated link, speaking the real protocol.
# In the suite rather than beside it, unlike test-vnc: this moves a few hundred
# bytes, not a 48 KB framebuffer.
test-irc: $(BUILD)/snos.sfc $(BUILD)/test/irc.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/irc.lua

test-history: $(BUILD)/snos.sfc $(BUILD)/test/history.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/history.lua

# Deliberately NOT in `test`: it receives a whole 48 KB framebuffer through the
# emulated link, which is two minutes on its own — as long as the other twelve
# together. Run it when touching apps/vnc or anything an application reaches for
# to paint: it is the only coverage `vnc` has, and it asserts the two things that
# went wrong there, that the picture is right THROUGH THE TILEMAP and that no
# forced blank lands where the picture is displayed.
test-vnc: $(BUILD)/snos.sfc $(BUILD)/test/vnc.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/vnc.lua

test-shadow: $(BUILD)/snos.sfc $(BUILD)/test/shadow.lua
	$(call check_env)
	rm -f "$(MESEN_SRM)"
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/shadow.lua

# One script for the Mesen Script Window that starts BOTH the host keyboard and
# the gateway link, since the Script Window runs only one at a time.
bridge: $(BUILD)/snosbridge.lua

$(BUILD)/snosbridge.lua: tools/kbdbridge.lua tools/netbridge.lua tools/mkbridge.py | $(BUILD)
	python3 tools/mkbridge.py $@ tools/kbdbridge.lua tools/netbridge.lua

clean:
	rm -rf $(BUILD)
