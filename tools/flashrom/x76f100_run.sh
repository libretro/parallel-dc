#!/bin/sh
# The X76F100 serial flash (x76f100.c) by itself.
#
#   sh tools/flashrom/x76f100_run.sh [extra C flags]
#
# x76f100_test.c is built with x76f100.c, with the C flags the core is
# built with; see the test for what it checks. What the chip says to lines
# driven at random must hash to x76f100_expected.txt. Should the chip
# change on purpose, the new hash goes in with it, from this test's output.
#
# Extra flags, such as -fsanitize=address,undefined, go to the compiler and
# the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/x76f100_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/flashrom/x76f100.o 2>/dev/null | grep 'x76f100\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_X=$(echo "$CCMD" | sed 's| [^ ]*x76f100\.c||; s/ -MMD -MP//')
CC=${CFLAGS_X%% *}

for SRC in core/hw/flashrom/x76f100.c tools/flashrom/x76f100_test.c; do
   eval "$CFLAGS_X $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/x76f100_test" "$WORK"/*.o

"$WORK/x76f100_test" > "$WORK/out.txt"
if ! cmp -s "$WORK/out.txt" tools/flashrom/x76f100_expected.txt; then
   echo "FAIL: what the chip said differs from x76f100_expected.txt: $(cat "$WORK/out.txt")" >&2
   exit 1
fi
echo "x76f100: ok (the protocol, its state, and lines at random as before)"
