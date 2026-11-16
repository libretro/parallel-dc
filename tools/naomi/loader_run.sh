#!/bin/sh
# The NAOMI and AtomisWave romset loader (core/hw/naomi/naomi_cart.c), by
# itself.
#
#   sh tools/naomi/cart_run.sh [extra C flags]
#
# loader_test.c is built with it, how a ROM is spread over two
# (naomi_interleave.c) and libretro-common's paths, with the C flags the
# core is built with, and stands in for the archives, the cartridges and
# the rest of the machine; what it prints must be what it always was
# (loader_expected.txt). Extra flags, such as -fsanitize=address,undefined,
# go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/loader_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/naomi_cart.o 2>/dev/null | grep 'naomi_cart\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*naomi_cart\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/naomi_cart.c core/hw/naomi/naomi_interleave.c \
		core/libretro-common/file/file_path.c core/libretro-common/compat/compat_strl.c \
		tools/naomi/loader_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/loader_test" "$WORK"/*.o
"$WORK/loader_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/loader_expected.txt \
   || { echo "loader_test: not as it was (tools/naomi/loader_expected.txt)"; exit 1; }
echo "loader_test: as before"
