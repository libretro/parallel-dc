#!/bin/sh
# The frontend's input as the machine has it (core/libretro/lr_input.c),
# driven at random.
#
#   sh tools/libretro/lr_input_run.sh [extra C flags]
#
# lr_input_test.c is built with lr_input.c, with the C flags the core is
# built with; what it prints must be what it always was
# (lr_input_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/lr_input_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/libretro/lr_input.o 2>/dev/null | grep 'lr_input\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*lr_input\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/libretro/lr_input.c tools/libretro/lr_input_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/lr_input_test" "$WORK"/*.o -lm
"$WORK/lr_input_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/libretro/lr_input_expected.txt \
   || { diff "$WORK/out.txt" tools/libretro/lr_input_expected.txt | head -n 8; echo "lr_input_test: not as it was (tools/libretro/lr_input_expected.txt)"; exit 1; }
echo "lr_input_test: as before"
