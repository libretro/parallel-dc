#!/bin/sh
# The NAOMI's JVS I/O boards (core/hw/maple/jvs_board.c) on requests,
# inputs, cabinets and games made at random.
#
#   sh tools/maple/jvs_board_run.sh [extra C flags]
#
# jvs_board_test.c is built with jvs_board.c, with the C flags the core is
# built with; what it prints must be what it always was
# (jvs_board_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/jvs_board_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/maple/jvs_board.o 2>/dev/null | grep 'jvs_board\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*jvs_board\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/maple/jvs_board.c tools/maple/jvs_board_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/jvs_board_test" "$WORK/jvs_board.o" "$WORK/jvs_board_test.o" -lm
"$WORK/jvs_board_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/maple/jvs_board_expected.txt \
   || { diff "$WORK/out.txt" tools/maple/jvs_board_expected.txt | head -n 8; echo "jvs_board_test: not as it was (tools/maple/jvs_board_expected.txt)"; exit 1; }
echo "jvs_board_test: as before"
