#!/bin/sh
# The cartridges' ROM boards (core/hw/naomi/cartridge.c, m1cartridge.c,
# m4cartridge.c, awcartridge.c), by themselves.
#
#   sh tools/naomi/cart_run.sh [extra C flags]
#
# cart_test.c is built with them and the M2 board's decryption chip
# (decrypt.c), with the C flags the core is built with, and stands in for
# the rest of the machine; what it prints must be what it always was
# (cart_expected.txt). Extra flags, such as -fsanitize=address,undefined,
# go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/cart_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/cartridge.o 2>/dev/null | grep 'cartridge\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*cartridge\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/cartridge.c core/hw/naomi/m1cartridge.c core/hw/naomi/m4cartridge.c \
		core/hw/naomi/awcartridge.c core/hw/naomi/decrypt.c tools/naomi/cart_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/cart_test" "$WORK"/*.o
"$WORK/cart_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/cart_expected.txt \
   || { echo "cart_test: not as it was (tools/naomi/cart_expected.txt)"; exit 1; }
echo "cart_test: as before"
