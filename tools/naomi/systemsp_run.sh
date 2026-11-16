#!/bin/sh
# The System SP's ROM board (core/hw/naomi/systemsp.c), by itself.
#
#   sh tools/naomi/systemsp_run.sh [extra C flags]
#
# systemsp_test.c is built with it, the M4 board it is (m4cartridge.c,
# cartridge.c), with the C flags the core is built with, and stands in for
# the rest of the machine, the CompactFlash card and the serial devices
# included; what it prints must be what it always was
# (systemsp_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/systemsp_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/cartridge.o 2>/dev/null | grep 'cartridge\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*cartridge\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/systemsp.c core/hw/naomi/m4cartridge.c core/hw/naomi/cartridge.c core/hw/naomi/decrypt.c \
		core/libretro-common/compat/compat_strl.c tools/naomi/systemsp_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/systemsp_test" "$WORK"/*.o
"$WORK/systemsp_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/systemsp_expected.txt \
   || { echo "systemsp_test: not as it was (tools/naomi/systemsp_expected.txt)"; exit 1; }
echo "systemsp_test: as before"
