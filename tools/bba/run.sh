#!/bin/sh
# The broadband adapter (core/hw/bba) by itself.
#
#   sh tools/bba/run.sh [extra C flags]
#
# bba_test.c is built with bba.c and rtl8139c.c, with the C flags the core
# is built with, and the network and the rest of the machine stubbed; see
# the test for what it checks. Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/bba_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/bba/bba.o 2>/dev/null | grep 'bba\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_BBA=$(echo "$CCMD" | sed 's| [^ ]*bba\.c||; s/ -MMD -MP//')
CC=${CFLAGS_BBA%% *}

for SRC in core/hw/bba/bba.c core/hw/bba/rtl8139c.c \
      core/libretro-common/encodings/encoding_crc32.c tools/bba/bba_test.c; do
   eval "$CFLAGS_BBA $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/bba_test" "$WORK"/*.o
"$WORK/bba_test"
