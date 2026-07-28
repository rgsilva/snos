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
            $(K)/input.c
KERNEL_S := calypsi/header.s $(K)/stack.s
KERNEL_O := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(KERNEL_C))) \
            $(patsubst %.s,$(BUILD)/%.o,$(notdir $(KERNEL_S)))

.PHONY: all kernel test test-boot test-input clean
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

$(BUILD)/snos.sfc: $(BUILD)/snos.raw tools/raw2sfc.py
	python3 tools/raw2sfc.py $< $@

test: test-boot test-input

test-boot: $(BUILD)/snos.sfc $(BUILD)/test/boot.lua
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/boot.lua

test-input: $(BUILD)/snos.sfc $(BUILD)/test/input.lua
	$(MESEN) --testrunner $(BUILD)/snos.sfc $(BUILD)/test/input.lua

clean:
	rm -rf $(BUILD)
