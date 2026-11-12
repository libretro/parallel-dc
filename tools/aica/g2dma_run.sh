#!/bin/sh
# The G2 bus's expansion DMA channels (aica_if.c) by themselves.
#
#   sh tools/aica/g2dma_run.sh [extra C flags]
#
# g2dma_test.c is built with aica_if.c, with the C flags the core is built
# with, and the rest of the machine stubbed; see the test for what it checks.
# Extra flags, such as -fsanitize=address,undefined, go to the compiler and
# the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/aica_g2dma.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/aica/aica_if.o 2>/dev/null | grep 'aica_if\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_G2=$(echo "$CCMD" | sed 's| [^ ]*aica_if\.c||; s/ -MMD -MP//')
CC=${CFLAGS_G2%% *}

for SRC in core/hw/aica/aica_if.c tools/aica/g2dma_test.c; do
   eval "$CFLAGS_G2 $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/g2dma_test" "$WORK"/*.o
"$WORK/g2dma_test"
