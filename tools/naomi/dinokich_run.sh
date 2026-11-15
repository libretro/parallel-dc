#!/bin/sh
# The IC card reader of dinokich and loveber3cn (core/hw/naomi/dinokich.c),
# by itself.
#
#   sh tools/naomi/dinokich_run.sh [extra C flags]
#
# dinokich_test.c is built with dinokich.c and the card's codec
# (dinokich_card.c), with the C flags the core is built with, and stands in
# for the rest of the machine; what it prints must be what it always was
# (dinokich_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/dinokich_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/dinokich.o 2>/dev/null | grep 'dinokich\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*dinokich\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/dinokich.c core/hw/naomi/dinokich_card.c \
		core/libretro-common/compat/compat_strl.c tools/naomi/dinokich_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/dinokich_test" "$WORK"/*.o
"$WORK/dinokich_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/dinokich_expected.txt \
   || { echo "dinokich_test: not as it was (tools/naomi/dinokich_expected.txt)"; exit 1; }
echo "dinokich_test: as before"
