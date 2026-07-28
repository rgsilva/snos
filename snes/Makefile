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
BUILD := build
K := kernel

MODEL := --code-model=large --data-model=large
CFLAGS = --core=65816 --target=snes $(MODEL) --no-ppu-mul -O0 -I$(K) \
         --list-file=$(@:.o=.lst)
AFLAGS := --core=65816 --target=SNES $(MODEL)
LNFLAGS = calypsi/linker.scm --output-format=raw --rtattr exit=simplified \
          $(CALDIR)/lib/clib-lc-ld-snes-noppu.a

KERNEL_C := $(K)/main.c $(K)/traps.c $(K)/kcon.c $(K)/console.c $(K)/mailbox.c \
            $(K)/input.c $(K)/shell.c $(K)/app.c
KERNEL_S := calypsi/header.s $(K)/stack.s $(K)/syscall.s $(K)/exec.s
KERNEL_O := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(KERNEL_C))) \
            $(patsubst %.s,$(BUILD)/%.o,$(notdir $(KERNEL_S)))

.PHONY: all kernel apps test test-boot test-input test-shell test-apps clean
all: kernel
kernel: $(BUILD)/snos.sfc

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/test

# Mesen's Lua sandbox blocks dofile/require, so the shared harness is spliced
# into each test at build time. See tools/mktest.py.
$(BUILD)/test/%.lua: test/%.lua test/lib.lua tools/mktest.py | $(BUILD)
	python3 tools/mktest.py $< test/lib.lua $@

$(BUILD)/%.o: $(K)/%.c | $(BUILD)
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
APPS := hello pad
APP_BANK_hello := c8
APP_BANK_pad   := c9
APP_AT_hello   := 0x080000
APP_AT_pad     := 0x090000

APP_RAWS := $(foreach a,$(APPS),$(BUILD)/apps/$(a)/$(a).raw)
APP_BLOBS := $(foreach a,$(APPS),--at $(APP_AT_$(a)) $(BUILD)/apps/$(a)/$(a).raw)

.PHONY: apps
apps: $(APP_RAWS)

$(BUILD)/apps/%/%.raw:
	@true

define APP_template
$(BUILD)/apps/$(1)/$(1).raw: ../apps/$(1)/$(1).c ../apps/crt0.s ../apps/app.scm.in \
                             $(K)/syscall.s $(K)/snos_abi.h tools/mkappinfo.py \
                             tools/buildapp.sh | $(BUILD)
	sh tools/buildapp.sh $(1) $(APP_BANK_$(1)) $(CALDIR) $(BUILD)
endef
$(foreach a,$(APPS),$(eval $(call APP_template,$(a))))

$(BUILD)/snos.sfc: $(BUILD)/snos.raw $(APP_RAWS) tools/raw2sfc.py
	python3 tools/raw2sfc.py $< $@ $(APP_BLOBS)

test: test-boot test-input test-shell test-apps

test-boot: $(BUILD)/snos.sfc $(BUILD)/test/boot.lua
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/boot.lua

test-input: $(BUILD)/snos.sfc $(BUILD)/test/input.lua
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/input.lua

test-shell: $(BUILD)/snos.sfc $(BUILD)/test/shell.lua
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/shell.lua

test-apps: $(BUILD)/snos.sfc $(BUILD)/test/apps.lua
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/apps.lua

clean:
	rm -rf $(BUILD)
