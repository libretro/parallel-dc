#!/bin/sh
# The devices on the maple bus (core/hw/maple/maple_devs.c) on machines,
# buses and frames made at random.
#
#   sh tools/maple/maple_devs_run.sh [extra C flags]
#
# maple_devs_test.c is built with maple_devs.c, what it has its devices
# with (jvs_bridge.c, jvs_board.c, maple_helper.c, libretro-common's
# inflater, CRC and xxh32), with the C flags the core is built with; what
# it prints must be what it always was (maple_devs_expected.txt, a 64-bit
# build's: the I/O board bridge's state has a size_t). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/maple_devs_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/maple/maple_devs.o 2>/dev/null | grep 'maple_devs\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*maple_devs\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/maple/maple_devs.c core/hw/maple/jvs_bridge.c core/hw/maple/jvs_board.c \
      core/hw/maple/maple_helper.c core/libretro-common/encodings/encoding_deflate.c \
      core/libretro-common/encodings/encoding_crc32.c core/libretro-common/encodings/encoding_xxh32.c \
      tools/maple/maple_devs_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/maple_devs_test" "$WORK"/*.o -lm
"$WORK/maple_devs_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/maple/maple_devs_expected.txt \
   || { diff "$WORK/out.txt" tools/maple/maple_devs_expected.txt | head -n 8; echo "maple_devs_test: not as it was (tools/maple/maple_devs_expected.txt)"; exit 1; }
echo "maple_devs_test: as before"
