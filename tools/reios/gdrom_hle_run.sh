#!/bin/sh
# The HLE BIOS's GD-ROM calls (core/reios/gdrom_hle.c), by themselves.
#
#   sh tools/reios/gdrom_hle_run.sh [extra C flags]
#
# gdrom_hle_test.c is built with gdrom_hle.c, with the C flags the core is
# built with, and stands in for the rest of the machine; see the test for
# what it checks. Extra flags, such as -fsanitize=address,undefined, go to
# the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/gdrom_hle_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/reios/gdrom_hle.o 2>/dev/null | grep 'gdrom_hle\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*gdrom_hle\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/reios/gdrom_hle.c tools/reios/gdrom_hle_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/gdrom_hle_test" "$WORK"/*.o
"$WORK/gdrom_hle_test"
