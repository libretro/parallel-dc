#!/bin/sh
# What core/hw/naomi/naomi.c keeps of a NAOMI and an AtomisWave, by itself.
#
#   sh tools/naomi/cart_run.sh [extra C flags]
#
# board_test.c is built with it and the serial EEPROMs' chip (x76f100.c),
# with the C flags the core is built with, and stands in for the rest of
# the machine; what it prints must be what it always was
# (board_expected.txt). Extra flags, such as -fsanitize=address,undefined,
# go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/board_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/naomi.o 2>/dev/null | grep 'naomi\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*naomi\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/naomi.c core/hw/flashrom/x76f100.c \
		core/libretro-common/compat/compat_strl.c tools/naomi/board_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/board_test" "$WORK"/*.o
"$WORK/board_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/board_expected.txt \
   || { echo "board_test: not as it was (tools/naomi/board_expected.txt)"; exit 1; }
echo "board_test: as before"
