# sa1probe: a standalone SA-1 test ROM (see docs/sa1-notes.md). Builds on its
# own; nothing here touches snes/.
#
#   make                  build/sa1probe.sfc
#   make test             both Mesen runs; ends in RESULT PASS
#
# Same variables as snes/Makefile. Mesen runs one instance per machine: wrap
# `make test` in `flock /tmp/snos-mesen.lock` where other agents share it.
CALDIR ?= ../../../snespico8/tools/calypsi-65816-5.17
AS := $(CALDIR)/bin/as65816
LN := $(CALDIR)/bin/ln65816
MESEN ?= /Applications/Mesen.app/Contents/MacOS/Mesen
# Mesen keeps the battery save in its own data directory, named after the ROM.
MESEN_SRM ?= $(HOME)/Library/Application Support/Mesen2/Saves/sa1probe.srm

BUILD := build
AFLAGS := --core=65816 --target=SNES -Isrc -I$(BUILD)
SRC := src/header.s src/main.s src/text.s src/checks.s src/bench.s src/sa1.s
OBJ := $(patsubst src/%.s,$(BUILD)/%.o,$(SRC)) $(BUILD)/assets.o
INC := src/regs.inc src/layout.inc

.PHONY: all test test-probe test-persist clean
all: $(BUILD)/sa1probe.sfc

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.s $(INC) $(BUILD)/assets.inc | $(BUILD)
	$(AS) -o $@ $< $(AFLAGS) --list-file=$(@:.o=.lst)

$(BUILD)/assets.s $(BUILD)/assets.inc: tools/mkassets.py ../snes/kernel/font8x8_basic.h | $(BUILD)
	python3 tools/mkassets.py ../snes/kernel/font8x8_basic.h $(BUILD)/assets.s

$(BUILD)/assets.o: $(BUILD)/assets.s
	$(AS) -o $@ $< $(AFLAGS)

$(BUILD)/sa1probe.raw: $(OBJ) linker.scm
	$(LN) -o $@ linker.scm --output-format=raw --list-file=$(BUILD)/sa1probe.map $(OBJ)

$(BUILD)/sa1probe.sfc: $(BUILD)/sa1probe.raw tools/mksfc.py
	python3 tools/mksfc.py $< $@

# The first run starts from no save file and expects boots=1; the second
# expects boots=2, which is what proves Mesen persisted BW-RAM.
test: test-probe test-persist
test-probe: $(BUILD)/sa1probe.sfc
	rm -f "$(MESEN_SRM)"
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/sa1probe.sfc test/probe.lua
test-persist: $(BUILD)/sa1probe.sfc test-probe
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/sa1probe.sfc test/persist.lua

clean:
	rm -rf $(BUILD)
