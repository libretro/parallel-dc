#!/bin/sh
# A NAOMI GD-ROM game's DIMM board (core/hw/naomi/gdcartridge.c), by
# itself.
#
#   sh tools/naomi/gd_run.sh [extra C flags]
#
# gd_test.c is built with it, what every cartridge has (cartridge.c) and
# what the DIMM board's firmware leaves in the NAOMI's memory
# (naomi_dimm.c), with the C flags the core is built with, and stands in
# for the rest of the machine, its disc included; what it prints must be
# what it always was (gd_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/gd_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/cartridge.o 2>/dev/null | grep 'cartridge\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*cartridge\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/gdcartridge.c core/hw/naomi/cartridge.c core/hw/naomi/decrypt.c \
		core/hw/naomi/naomi_dimm.c core/libretro-common/compat/compat_strl.c tools/naomi/gd_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/gd_test" "$WORK"/*.o
"$WORK/gd_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/gd_expected.txt \
   || { echo "gd_test: not as it was (tools/naomi/gd_expected.txt)"; exit 1; }
echo "gd_test: as before"
