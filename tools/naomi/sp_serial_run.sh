#!/bin/sh
# What is plugged into the System SP's serial ports
# (core/hw/naomi/systemsp_serial.c), by itself.
#
#   sh tools/naomi/sp_serial_run.sh [extra C flags]
#
# sp_serial_test.c is built with systemsp_serial.c, with the C flags the
# core is built with, and stands in for the rest of the machine; what it
# prints must be what it always was (sp_serial_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/sp_serial_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/systemsp_serial.o 2>/dev/null | grep 'systemsp_serial\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*systemsp_serial\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/systemsp_serial.c core/libretro-common/compat/compat_strl.c \
		tools/naomi/sp_serial_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/sp_serial_test" "$WORK"/*.o
"$WORK/sp_serial_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/sp_serial_expected.txt \
   || { echo "sp_serial_test: not as it was (tools/naomi/sp_serial_expected.txt)"; exit 1; }
echo "sp_serial_test: as before"
