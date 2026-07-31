#!/bin/sh
# Build one SNOS application into a raw bank image.
#
# Each app is a separate link into its own ROM bank — that is the whole model
# (PLAN.md §1). It shares only two things with the kernel: the ABI header, and
# kernel/syscall.s, which it links its own copy of so its syscall path is
# byte-identical to the kernel's.
#
# Usage: buildapp.sh <name> <bankhex> <caldir> <builddir> [min-kernel-abi] \
#                    [arena-lo-hex] [arena-hi-hex]
#
# The arena bounds (M10 step 4, e.g. 7f0000 7f3bff) place the app's DATA the
# way the bank places its CODE: at build time, by declaration. The first 256
# bytes of the region are the near segment, the rest far; the linker's own
# range check is the enforcement — an app that outgrows its declared slot
# fails RIGHT HERE, not in a neighbour's memory at runtime.
set -e
NAME=$1
BANK=$2
CALDIR=$3
BUILD=$4
MINABI=${5:-1}
ARENA_LO=${6:-7f0000}
ARENA_HI=${7:-7fffff}

# Fail loudly on a malformed declaration. Silence here was dangerous three
# ways: a one-word APP_ARENA_x fell back to 7fffff for HI; an inverted range
# made the header's page count 0, which the kernel reads as "claims all
# 64 KB"; a non-page-aligned bound truncated in the header while the linker
# enforced exact bytes, so the disjointness check and the link disagreed.
case "$ARENA_LO" in 7f??00) ;; *) echo "buildapp: $NAME ARENA_LO '$ARENA_LO' must be 7fXX00" >&2; exit 1;; esac
case "$ARENA_HI" in 7f??ff) ;; *) echo "buildapp: $NAME ARENA_HI '$ARENA_HI' must be 7fXXff" >&2; exit 1;; esac
if [ $((0x$ARENA_HI)) -le $((0x$ARENA_LO)) ]; then
    echo "buildapp: $NAME arena range inverted ($ARENA_LO..$ARENA_HI)" >&2
    exit 1
fi

ANEAR_LO=$ARENA_LO
ANEAR_HI=$(printf '%06x' $((0x$ARENA_LO + 0xFF)))
AFAR_LO=$(printf '%06x' $((0x$ARENA_LO + 0x100)))
AFAR_HI=$ARENA_HI
# The header's two-byte arena descriptor: start page within bank $7F, and
# page count with 0 meaning all 256 (the whole arena in one byte).
ARENA_PAGE=$(( (0x$ARENA_LO >> 8) & 0xFF ))
ARENA_PAGES=$(( ((0x$ARENA_HI - 0x$ARENA_LO + 1) >> 8) & 0xFF ))

OUT="$BUILD/apps/$NAME"
SRC="../apps/$NAME"
mkdir -p "$OUT"

MODEL="--code-model=large --data-model=large"
# No --target=snes: it inlines mul/div on the hardware ALU, which an NMI
# corrupts mid-computation (see the CFLAGS comment in ../Makefile and
# docs/calypsi-interrupt-hazard.md). Apps run with NMI live, same as the
# kernel, so they need the same soft-multiply discipline.
CFLAGS="--core=65816 $MODEL -O0 -Ikernel -I../apps/lib"
AFLAGS="--core=65816 --target=SNES $MODEL"

sed -e "s/@BANK@/$BANK/g" -e "s/@ANEAR_LO@/$ANEAR_LO/g" \
    -e "s/@ANEAR_HI@/$ANEAR_HI/g" -e "s/@AFAR_LO@/$AFAR_LO/g" \
    -e "s/@AFAR_HI@/$AFAR_HI/g" ../apps/app.scm.in > "$OUT/app.scm"

# crt0 zeroes only this app's arena slot; same declaration, second consumer.
AZLO=$(printf '%04x' $(( 0x$ARENA_LO & 0xFFFF )))
AZEND=$(printf '%04x' $(( (0x$ARENA_HI + 1) & 0xFFFF )))
sed -e "s/@AZLO@/$AZLO/g" -e "s/@AZEND@/$AZEND/g" ../apps/crt0.s > "$OUT/crt0.s"
python3 tools/mkappinfo.py "$NAME" 1 "$OUT/appinfo.s" "$MINABI" \
    "$ARENA_PAGE" "$ARENA_PAGES"

"$CALDIR/bin/as65816" -o "$OUT/appinfo.o" "$OUT/appinfo.s" $AFLAGS
"$CALDIR/bin/as65816" -o "$OUT/crt0.o"    "$OUT/crt0.s"     $AFLAGS
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
    "$CALDIR/lib/clib-lc-ld.a"

echo "  app $NAME -> bank \$$BANK ($(wc -c < "$OUT/$NAME.raw") bytes)"
