#!/bin/sh
# Build one SNOS application into a raw bank image.
#
# Each app is a separate link into its own ROM bank — that is the whole model
# (PLAN.md §1). It shares only two things with the kernel: the ABI header, and
# kernel/syscall.s, which it links its own copy of so its syscall path is
# byte-identical to the kernel's.
#
# Usage: buildapp.sh <name> <bankhex> <caldir> <builddir> [min-kernel-abi]
set -e
NAME=$1
BANK=$2
CALDIR=$3
BUILD=$4
MINABI=${5:-1}

OUT="$BUILD/apps/$NAME"
SRC="../apps/$NAME"
mkdir -p "$OUT"

MODEL="--code-model=large --data-model=large"
CFLAGS="--core=65816 --target=snes $MODEL --no-ppu-mul -O0 -Ikernel -I../apps/lib"
AFLAGS="--core=65816 --target=SNES $MODEL"

sed "s/@BANK@/$BANK/g" ../apps/app.scm.in > "$OUT/app.scm"
python3 tools/mkappinfo.py "$NAME" 1 "$OUT/appinfo.s" "$MINABI"

"$CALDIR/bin/as65816" -o "$OUT/appinfo.o" "$OUT/appinfo.s" $AFLAGS
"$CALDIR/bin/as65816" -o "$OUT/crt0.o"    ../apps/crt0.s    $AFLAGS
"$CALDIR/bin/as65816" -o "$OUT/syscall.o" kernel/syscall.s  $AFLAGS
# The shared app library, compiled per app: apps are separate links into
# their own banks, so there is no common address space to place one copy in.
# The linker keeps only what each app reaches.
"$CALDIR/bin/cc65816" -o "$OUT/snoslib.o" ../apps/lib/snoslib.c $CFLAGS \
    --list-file="$OUT/snoslib.lst"
"$CALDIR/bin/cc65816" -o "$OUT/$NAME.o"   "$SRC/$NAME.c"    $CFLAGS \
    --list-file="$OUT/$NAME.lst"

# An app may carry hand-written assembly of its own, as apps/vnc does for the
# Mode 3 transpose. Anything matching apps/<name>/*.s is assembled with the SAME
# flags as the kernel's and linked in alongside the C. The .lst is not optional:
# as65816 tracks neither register widths nor direct-page-vs-absolute, so the
# emitted bytes are the only way to check an addressing mode (see
# docs/calypsi-asm-syntax.md).
APPOBJ=""
for s in "$SRC"/*.s; do
    [ -e "$s" ] || continue
    b=$(basename "$s" .s)
    "$CALDIR/bin/as65816" -o "$OUT/$b.o" "$s" $AFLAGS --list-file="$OUT/$b.lst"
    APPOBJ="$APPOBJ $OUT/$b.o"
done

# No SNES clib: an app has no reset vector, no cartridge header and no C
# startup to run. The kernel has already set the stack, direct page and data
# bank before jumping in, so crt0.s only has to call main.
# --no-auto-libraries keeps Calypsi's C startup out. It would otherwise drag in
# a `reset` section holding a 16-bit vector, which cannot represent an address
# in bank $c8 and fails the link outright. An app has no reset vector anyway.
#
# The archive is still listed explicitly, so members the app genuinely needs are
# pulled individually: pseudoRegisters.o for the compiler's direct-page scratch
# (_Dp), and memcpy/memset when C code implies them. --program-root app_start
# is what stops the C startup from being dragged back in as a root.
"$CALDIR/bin/ln65816" -o "$OUT/$NAME.raw" "$OUT/app.scm" \
    --output-format=raw --no-auto-libraries --program-root app_start \
    --list-file="$OUT/$NAME.map" \
    "$OUT/appinfo.o" "$OUT/crt0.o" "$OUT/syscall.o" "$OUT/snoslib.o" \
    "$OUT/$NAME.o" $APPOBJ \
    "$CALDIR/lib/clib-lc-ld-snes-noppu.a"

echo "  app $NAME -> bank \$$BANK ($(wc -c < "$OUT/$NAME.raw") bytes)"
