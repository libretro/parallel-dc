#!/bin/sh
# The BIOS ROM, the SRAM and the flash memory (core/hw/flashrom/flashrom.c) by
# themselves.
#
#   sh tools/flashrom/flashrom_run.sh [extra C flags]
#
# flashrom_test.c is built with flashrom.c, with the C flags the
# core is built with; what it makes must be what it always made
# (flashrom_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/flashrom_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/flashrom/flashrom.o 2>/dev/null | grep 'flashrom\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*flashrom\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/flashrom/flashrom.c tools/flashrom/flashrom_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/flashrom_test" "$WORK"/*.o
"$WORK/flashrom_test" "$WORK" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/flashrom/flashrom_expected.txt \
   || { echo "flashrom_test: not what it always made (tools/flashrom/flashrom_expected.txt)"; exit 1; }
echo "flashrom_test: as before"
