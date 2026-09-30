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
# NO --target=snes for C, deliberately. The SNES target inlines multiply and
# divide on the S-CPU hardware ALU ($4202-$4217) guarded by SEI — and NMI is
# not maskable. An NMI landing in the ~10-cycle result window destroys the
# product: the vblank auto-joypad poll clobbers $4216/$4217 in hardware, and
# proc_tick's own multiplies would clobber it from C anyway. The symptom was
# `i * 7u` evaluating to 0 in fstest — deterministic at one code layout,
# invisible at the next, because it needs the (deterministic) NMI to land on
# exactly that instruction. Bare 65816 emits soft _Mul16/_Div16 calls whose
# state lives in DP pseudo-registers, which the four-direct-page scheme
# already makes interrupt-safe. See docs/calypsi-interrupt-hazard.md.
CFLAGS = --core=65816 $(MODEL) -O0 -I$(K) -I$(K)/uip \
         --list-file=$(@:.o=.lst)
# The real-hardware ROM: the port-2 serial backend (kernel/link_serial.c)
# instead of the emulated mailbox, LINK_FRAME 64, no foreground link poll.
# See docs/pico-adapter.md. `make LINK_HW=1` builds into build-hw/ so the
# emulator ROM the suite tests is never overwritten by it.
ifdef LINK_HW
CFLAGS += -DLINK_HW=1
BUILD := build-hw
endif
AFLAGS := --core=65816 --target=SNES $(MODEL)
# Generic clib to match the bare-target C objects (the -snes variants carry a
# target attribute the linker refuses to mix). Verified to contain no
# $4202/$4204 hardware-ALU references, same as the -snes-noppu one did.
LNFLAGS = calypsi/linker.scm --output-format=raw --rtattr exit=simplified \
          $(CALDIR)/lib/clib-lc-ld.a

KERNEL_C := $(K)/main.c $(K)/traps.c $(K)/kcon.c $(K)/console.c $(K)/mailbox.c \
            $(K)/input.c $(K)/shell.c $(K)/app.c $(K)/sram.c $(K)/fs.c $(K)/net.c \
            $(K)/link.c $(K)/link_wram.c $(K)/link_serial.c $(K)/proc.c $(K)/ip.c \
            $(K)/uip/uip.c
KERNEL_S := calypsi/header.s $(K)/stack.s $(K)/syscall.s $(K)/exec.s \
            $(K)/netasm.s $(K)/linkxfer.s
KERNEL_O := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(KERNEL_C))) \
            $(patsubst %.s,$(BUILD)/%.o,$(notdir $(KERNEL_S))) \
            $(BUILD)/fontvram.o

.PHONY: all kernel apps bridge test test-host test-boot test-input test-history test-shadow test-shell test-apps test-fs test-restart test-basic test-panel test-editor test-mtask test-bg test-telnetd test-ip test-net test-netreset test-pager test-httpd test-irc test-vnc test-mc test-kbd test-wire test-speedtest clean
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
$(BUILD)/test/%.lua: test/%.lua test/lib.lua test/gwip.lua tools/mktest.py | $(BUILD)
	python3 tools/mktest.py $< test/lib.lua $@

# Every object depends on every kernel header. Coarse, but the kernel is a
# dozen files and a full rebuild is seconds — whereas NOT tracking headers
# silently ships a stale object. Editing snos_abi.h to bump the ABI version left
# the shell compiled against the old one, so it refused to run an application
# for needing an ABI the kernel had in fact just gained.
KERNEL_H := $(wildcard $(K)/*.h)

$(BUILD)/%.o: $(K)/%.c $(KERNEL_H) | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

# Vendored uIP (kernel/uip/). Same flags as the rest of the kernel, including
# the no---target=snes rule: uIP does 16-bit arithmetic everywhere and every
# multiply of it would otherwise land on the interrupt-hostile hardware ALU.
$(BUILD)/%.o: $(K)/uip/%.c $(KERNEL_H) $(wildcard $(K)/uip/*.h) | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

$(BUILD)/%.o: $(K)/%.s | $(BUILD)
	$(AS) -o $@ $< $(AFLAGS)

$(BUILD)/header.o: calypsi/header.s | $(BUILD)
	$(AS) -o $@ $< $(AFLAGS)

# The console fonts, baked at build time so con_init DMAs them from ROM
# instead of computing 12K port writes on every app exit. See tools/mkfont.py.
$(BUILD)/fontvram.c: $(K)/font8x8_basic.h tools/mkfont.py | $(BUILD)
	python3 tools/mkfont.py $< $@

$(BUILD)/fontvram.o: $(BUILD)/fontvram.c
	$(CC) -o $@ $< $(CFLAGS)

# linker.scm IS the kernel memory map; editing it without this prerequisite
# shipped a stale ROM silently -- the same bug class KERNEL_H exists to stop.
$(BUILD)/snos.raw: $(KERNEL_O) calypsi/linker.scm
	$(LN) -o $@ $(LNFLAGS) --list-file=$(BUILD)/snos.map $(KERNEL_O)

# ---------------------------------------------------------------- applications
# Each app is a separate link into its own ROM bank, then dropped into the
# cartridge image at the byte offset that bank maps to. HiROM makes that
# arithmetic trivial: SNES $C0:0000+N is ROM byte N, so bank $C8 is 0x080000.
APPS := hello pad basic edit view get netecho httpd vnc irc telnetd mc wire speedtest
APP_BANK_hello := c8
APP_BANK_pad   := c9
APP_BANK_basic := ca
APP_BANK_edit  := cb
APP_BANK_view  := cc
APP_BANK_get   := cd
APP_BANK_netecho := ce
APP_BANK_httpd := d0
APP_BANK_telnetd := d1   # the slot `web` left behind
APP_BANK_vnc   := d2
APP_BANK_irc   := d3
APP_BANK_mc    := d4
APP_BANK_wire  := d5   # the port-2 cable test (docs/pico-adapter.md)
APP_BANK_speedtest := d6
APP_AT_hello   := 0x080000
APP_AT_pad     := 0x090000
APP_AT_basic   := 0x0A0000
APP_AT_edit    := 0x0B0000
APP_AT_view    := 0x0C0000
APP_AT_get     := 0x0D0000
APP_AT_netecho := 0x0E0000
APP_AT_httpd   := 0x100000
APP_AT_telnetd := 0x110000
APP_AT_vnc     := 0x120000
APP_AT_irc     := 0x130000
APP_AT_mc      := 0x140000   # bank $d4
APP_AT_wire    := 0x150000   # bank $d5
APP_AT_speedtest := 0x160000
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
# telnetd needs ABI 11: SYS_CONROW (read a console row) and SYS_KBDPUSH
# (inject a keystroke) make the mirror possible; SYS_CONDIRTY (changed-rows
# mask) is what makes polling it every frame affordable.
APP_ABI_telnetd := 11
# `irc` needs the ABI-7 stream calls: it opens a plain TCP connection and both
# talks and listens on it. Nothing it does needs ABI 8.
APP_ABI_irc    := 7
APP_ABI_mc     := 12
APP_ABI_speedtest := 12

# Arena slots (M10 step 4): measured need, declared placement, build-time
# enforcement — the app's data links inside exactly this range and the link
# FAILS if it no longer fits. Two apps may co-run iff their regions are
# disjoint; the kernel reads the slot from the app header and refuses
# overlapping spawns. Measured usage that sized these regions (of 64 KB):
# servers <1%, basic 11%, edit 20%, irc 66%, vnc 78%.
#   A  7f0000-7f3bff  15 KB  foreground UI apps (never co-run with each other)
#   C  7f3c00-7f3fff   1 KB  small background servers/downloads
#   B  7f4000-7fffff  48 KB  irc — disjoint from A and C by construction:
#                            edit+irc is the M10 demo pairing
#   X  7f0000-7fffff  64 KB  vnc — genuinely needs the whole arena, co-runs
#                            with nothing (and is meaningless backgrounded)
APP_ARENA_hello   := 7f0000 7f3bff
APP_ARENA_pad     := 7f0000 7f3bff
APP_ARENA_basic   := 7f0000 7f3bff
APP_ARENA_edit    := 7f0000 7f3bff
APP_ARENA_view    := 7f0000 7f3bff
APP_ARENA_get     := 7f3c00 7f3fff
APP_ARENA_netecho := 7f3c00 7f3fff
APP_ARENA_httpd   := 7f3c00 7f3fff
# telnetd holds a 1792-byte copy of the screen; 4 KB is room to spare. It sits
# outside region A, so it mirrors while any foreground UI app runs — which is
# the whole point. It OVERLAPS irc's region deliberately: with one background
# slot they can never co-run anyway, and the kernel refuses the pairing by
# name rather than letting the arithmetic rot silently.
APP_ARENA_telnetd := 7f4000 7f4fff
APP_ARENA_irc     := 7f4000 7fffff
APP_ARENA_vnc     := 7f0000 7fffff
# mc carries TWO sessions' framing state (the ping/login overlap), which
# outgrows the 1 KB small-server slot. It overlaps irc's region: they can
# never co-run, and the kernel refuses the pairing by name.
APP_ARENA_mc      := 7f5000 7f57ff
APP_ARENA_wire    := 7f0000 7f3bff
APP_ARENA_speedtest := 7f0000 7f3bff

APP_RAWS := $(foreach a,$(APPS),$(BUILD)/apps/$(a)/$(a).raw)
APP_BLOBS := $(foreach a,$(APPS),--at $(APP_AT_$(a)) $(BUILD)/apps/$(a)/$(a).raw)

# ROMFS lives in bank $CF; apps run $C8-$CE and continue at $D0+ (the image
# grew past 1 MB when they did).
ROMFS_AT := 0x0F0000
# The baked Minecraft session (M13): banks $e0.., far from the app banks.
# MCDATA_AT is the ROM file offset; mkmc's --base is the matching 24-bit
# cart address, and build/mcdata.h is generated against it - the two are
# stated together here because disagreeing silently is garbage chunks.
MCDATA_AT := 0x200000
MCDATA_BASE := 0xE00000
ROMFS_SRC := $(wildcard ../romfs/*)

$(BUILD)/romfs.bin: $(ROMFS_SRC) tools/mkromfs.py | $(BUILD)
	python3 tools/mkromfs.py ../romfs $@

apps: $(APP_RAWS)

# (No catch-all %.raw rule here: GNU make patterns take exactly one stem, so a
# two-% target never matched anything — but as a prerequisite-satisfying no-op
# it could have handed raw2sfc a stale file for an app missing from the
# template below. The per-app rules from APP_template are the only builders.)

define APP_template
$(BUILD)/apps/$(1)/$(1).raw: ../apps/$(1)/$(1).c $(wildcard ../apps/$(1)/*.s) \
                             ../apps/crt0.s ../apps/app.scm.in \
                             ../apps/lib/snoslib.c ../apps/lib/snoslib.h \
                             $(K)/syscall.s $(KERNEL_H) tools/mkappinfo.py \
                             tools/buildapp.sh | $(BUILD)
	sh tools/buildapp.sh $(1) $(APP_BANK_$(1)) $(CALDIR) $(BUILD) $(or $(APP_ABI_$(1)),1) $(or $(APP_ARENA_$(1)),7f0000 7fffff)
endef
$(foreach a,$(APPS),$(eval $(call APP_template,$(a))))

$(BUILD)/mcdata.bin $(BUILD)/mcdata.h &: tools/mkmc.py | $(BUILD)
	python3 tools/mkmc.py --rom $(BUILD)/mcdata.bin \
	  --header $(BUILD)/mcdata.h --base $(MCDATA_BASE)

# mc compiles against the generated index; the app rule itself is the
# generic template, so only the extra dependency is stated here.
$(BUILD)/apps/mc/mc.raw: $(BUILD)/mcdata.h

$(BUILD)/snos.sfc: $(BUILD)/snos.raw $(APP_RAWS) $(BUILD)/romfs.bin $(BUILD)/mcdata.bin tools/raw2sfc.py
	python3 tools/raw2sfc.py $< $@ $(APP_BLOBS) --at $(ROMFS_AT) $(BUILD)/romfs.bin --at $(MCDATA_AT) $(BUILD)/mcdata.bin

test: test-host test-boot test-input test-history test-shell test-shadow test-apps test-fs test-restart test-basic test-panel test-editor test-mtask test-bg test-telnetd test-ip test-net test-netreset test-pager test-httpd test-irc test-mc test-kbd test-wire test-speedtest

# Host-side, no Mesen: the gateway's file-relay discipline driven by a
# scripted peer doing what netbridge.lua does to the files. First because it
# is the fastest suite and its failure modes (a broken gateway) would
# otherwise surface as mysterious timeouts in test-net.
test-host:
	python3 test/host/bridge_test.py
	python3 test/host/slirp_test.py
	python3 test/host/mkmc_test.py
	python3 test/host/speedtest_test.py

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

# Start with room for the persistence fixture; neither restart in the test
# deletes the save or resets the emulator.
test-restart: $(BUILD)/snos.sfc $(BUILD)/test/restart.lua
	$(call check_env)
	rm -f "$(MESEN_SRM)"
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/restart.lua

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

# Downloads write the SRAM volume, and SRAMFS is a bump allocator: without a
# wipe, the accumulated DLTEST/BIG/WINLOSS of every previous run eventually
# fill the 256 K and `get` starts refusing with "not enough room" — a failure
# of the HARNESS state, not of the code under test.
test-net: $(BUILD)/snos.sfc $(BUILD)/test/net.lua
	$(call check_env)
	rm -f "$(MESEN_SRM)"
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/net.lua

test-netreset: $(BUILD)/snos.sfc $(BUILD)/test/netreset.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/netreset.lua

test-speedtest: $(BUILD)/snos.sfc $(BUILD)/test/speedtest.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/speedtest.lua

test-pager: $(BUILD)/snos.sfc $(BUILD)/test/pager.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/pager.lua

# M10 step 1: the context-switch primitive, proven by two never-yielding
# kernel workers both making progress under the NMI round-robin.
test-mtask: $(BUILD)/snos.sfc $(BUILD)/test/mtask.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/mtask.lua

# telnetd v2: the screen-diff mirror, driven through the real link protocol.
test-telnetd: $(BUILD)/snos.sfc $(BUILD)/test/telnetd.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/telnetd.lua

test-ip: $(BUILD)/snos.sfc $(BUILD)/test/ip.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/ip.lua

# M10 step 5: the milestone -- a background server answers over the link
# WHILE a foreground app runs, plus the arena-refusal and kill/reap story.
test-bg: $(BUILD)/snos.sfc $(BUILD)/test/bg.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/bg.lua

# A Lua ircd on the far side of the emulated link, speaking the real protocol.
# In the suite rather than beside it, unlike test-vnc: this moves a few hundred
# bytes, not a 48 KB framebuffer.
test-httpd: $(BUILD)/snos.sfc $(BUILD)/test/httpd.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/httpd.lua

# M13: the Minecraft server, driven by a scripted protocol-754 client. The
# 146 KB join burst rides uIP stop-and-wait, so this is a long test - the
# vnc class, not the netecho class.
test-mc: $(BUILD)/snos.sfc $(BUILD)/test/mc.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/mc.lua

# Keystrokes over the link (LINK_CH_KBD, the hardware adapter's keyboard path),
# driven through the same gwip peer the network tests use.
test-kbd: $(BUILD)/snos.sfc $(BUILD)/test/kbd.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/kbd.lua

# apps/wire, the port-2 cable test, run with no adapter: the loop, the stats
# line and the B exit are what the emulator can check.
test-wire: $(BUILD)/snos.sfc $(BUILD)/test/wire.lua
	$(call check_env)
	sh tools/runtest.sh "$(MESEN)" $(BUILD)/snos.sfc $(BUILD)/test/wire.lua

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
