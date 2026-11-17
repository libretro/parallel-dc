#!/bin/sh
# The NAOMI's I/O board bridge (core/hw/maple/jvs_bridge.c), with its
# boards (jvs_board.c), on maple frames made at random.
#
#   sh tools/maple/jvs_bridge_run.sh [extra C flags]
#
# jvs_bridge_test.c is built with jvs_bridge.c, jvs_board.c and
# libretro-common's xxh32, with the C flags the core is built with; what
# it prints must be what it always was (jvs_bridge_expected.txt, a 64-bit
# build's: a state has a size_t). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/jvs_bridge_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/maple/jvs_bridge.o 2>/dev/null | grep 'jvs_bridge\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*jvs_bridge\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/maple/jvs_bridge.c core/hw/maple/jvs_board.c \
      core/libretro-common/encodings/encoding_xxh32.c tools/maple/jvs_bridge_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/jvs_bridge_test" "$WORK/jvs_bridge.o" "$WORK/jvs_board.o" \
   "$WORK/encoding_xxh32.o" "$WORK/jvs_bridge_test.o" -lm
"$WORK/jvs_bridge_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/maple/jvs_bridge_expected.txt \
   || { diff "$WORK/out.txt" tools/maple/jvs_bridge_expected.txt | head -n 8; echo "jvs_bridge_test: not as it was (tools/maple/jvs_bridge_expected.txt)"; exit 1; }
echo "jvs_bridge_test: as before"
