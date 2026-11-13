#!/bin/sh
# Dinosaur King's card codec (core/hw/naomi/dinokich_card.c) by itself.
#
#   sh tools/naomi/dinokich_card_run.sh [extra C flags]
#
# dinokich_card_test.c is built with dinokich_card.c, with the C flags the
# core is built with; what it makes must be what it always made
# (dinokich_card_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/dinokich_card_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/dinokich_card.o 2>/dev/null | grep 'dinokich_card\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*dinokich_card\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/dinokich_card.c tools/naomi/dinokich_card_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/dinokich_card_test" "$WORK"/*.o
"$WORK/dinokich_card_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/dinokich_card_expected.txt \
   || { echo "dinokich_card_test: not what it always made (tools/naomi/dinokich_card_expected.txt)"; exit 1; }
echo "dinokich_card_test: as before"
