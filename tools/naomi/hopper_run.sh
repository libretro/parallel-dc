#!/bin/sh
# The hopper boards (core/hw/naomi/hopper.c), by themselves.
#
#   sh tools/naomi/hopper_run.sh [extra C flags]
#
# hopper_test.c is built with hopper.c, with the C flags the core is
# built with, and stands in for the rest of the machine; what it prints
# must be what it always was (hopper_expected.txt). Extra flags, such
# as -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/hopper_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/hopper.o 2>/dev/null | grep 'hopper\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*hopper\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/hopper.c tools/naomi/hopper_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/hopper_test" "$WORK"/*.o
"$WORK/hopper_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/hopper_expected.txt \
   || { echo "hopper_test: not as it was (tools/naomi/hopper_expected.txt)"; exit 1; }
echo "hopper_test: as before"
