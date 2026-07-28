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
define check_no_gui
	@if [ -z "$(MESEN_ALLOW_GUI)" ] && pgrep -x Mesen >/dev/null 2>&1; then \
	  echo "ERROR: a Mesen GUI is running (pid $$(pgrep -x Mesen | tr '\n' ' '))."; \
	  echo "       --testrunner collides with it and emu.setInput stops working,"; \
	  echo "       so controller-driven tests fail for no real reason."; \
	  echo "       Close Mesen, or run with MESEN_ALLOW_GUI=1 to skip this check."; \
	  exit 1; \
	fi
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
KERNEL_S := calypsi/header.s $(K)/stack.s $(K)/syscall.s $(K)/exec.s
KERNEL_O := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(KERNEL_C))) \
            $(patsubst %.s,$(BUILD)/%.o,$(notdir $(KERNEL_S)))

.PHONY: all kernel apps test test-boot test-input test-shell test-apps test-fs test-basic test-panel test-editor test-net clean
all: kernel
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
APPS := hello pad basic edit
APP_BANK_hello := c8
APP_BANK_pad   := c9
APP_BANK_basic := ca
APP_BANK_edit  := cb
APP_AT_hello   := 0x080000
APP_AT_pad     := 0x090000
APP_AT_basic   := 0x0A0000
APP_AT_edit    := 0x0B0000
# The editor needs the ABI-2 full-screen calls and says so in its header, so an
# older kernel refuses to run it rather than failing on a missing syscall.
APP_ABI_edit   := 2

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
$(BUILD)/apps/$(1)/$(1).raw: ../apps/$(1)/$(1).c ../apps/crt0.s ../apps/app.scm.in \
                             $(K)/syscall.s $(KERNEL_H) tools/mkappinfo.py \
                             tools/buildapp.sh | $(BUILD)
	sh tools/buildapp.sh $(1) $(APP_BANK_$(1)) $(CALDIR) $(BUILD) $(or $(APP_ABI_$(1)),1)
endef
$(foreach a,$(APPS),$(eval $(call APP_template,$(a))))

$(BUILD)/snos.sfc: $(BUILD)/snos.raw $(APP_RAWS) $(BUILD)/romfs.bin tools/raw2sfc.py
	python3 tools/raw2sfc.py $< $@ $(APP_BLOBS) --at $(ROMFS_AT) $(BUILD)/romfs.bin

test: test-boot test-input test-shell test-apps test-fs test-basic test-panel test-editor test-net

test-boot: $(BUILD)/snos.sfc $(BUILD)/test/boot.lua
	$(call check_no_gui)
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/boot.lua

test-input: $(BUILD)/snos.sfc $(BUILD)/test/input.lua
	$(call check_no_gui)
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/input.lua

test-shell: $(BUILD)/snos.sfc $(BUILD)/test/shell.lua
	$(call check_no_gui)
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/shell.lua

test-apps: $(BUILD)/snos.sfc $(BUILD)/test/apps.lua
	$(call check_no_gui)
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/apps.lua

# Mesen persists cart SRAM to a .srm; a stale one from a previous run would
# make the persistence assertion pass for the wrong reason.
test-fs: $(BUILD)/snos.sfc $(BUILD)/test/fs.lua
	$(call check_no_gui)
	rm -f "$(MESEN_SRM)"
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/fs.lua

test-basic: $(BUILD)/snos.sfc $(BUILD)/test/basic.lua
	$(call check_no_gui)
	rm -f "$(MESEN_SRM)"
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/basic.lua

test-panel: $(BUILD)/snos.sfc $(BUILD)/test/panel.lua
	$(call check_no_gui)
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/panel.lua

test-editor: $(BUILD)/snos.sfc $(BUILD)/test/editor.lua
	$(call check_no_gui)
	rm -f "$(MESEN_SRM)"
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/editor.lua

test-net: $(BUILD)/snos.sfc $(BUILD)/test/net.lua
	$(call check_no_gui)
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/net.lua

clean:
	rm -rf $(BUILD)
