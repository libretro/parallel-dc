#!/bin/sh
# The SH4's serial port with the touch panel on it
# (core/hw/sh4/modules/serial.c, core/hw/naomi/touchscreen.c), by
# themselves.
#
#   sh tools/naomi/scif_touch_run.sh [extra C flags]
#
# scif_touch_test.c is built with the two, with the C flags the core is
# built with, and stands in for the rest of the machine; what it hashes
# must be what it always was (scif_touch_expected.txt). Extra flags, such
# as -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/scif_touch_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/sh4/modules/serial.o 2>/dev/null | grep 'serial\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*serial\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/modules/serial.c core/hw/naomi/touchscreen.c tools/naomi/scif_touch_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/scif_touch_test" "$WORK"/*.o
"$WORK/scif_touch_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/scif_touch_expected.txt \
   || { echo "scif_touch_test: not as it was (tools/naomi/scif_touch_expected.txt)"; exit 1; }
echo "scif_touch_test: as before"
