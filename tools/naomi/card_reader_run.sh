#!/bin/sh
# The card readers (core/hw/naomi/card_reader.c), by themselves.
#
#   sh tools/naomi/card_reader_run.sh [extra C flags]
#
# card_reader_test.c is built with card_reader.c, with the C flags the core is
# built with, and stands in for the rest of the machine; what it prints
# must be what it always was (card_reader_expected.txt). Extra flags, such
# as -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/card_reader_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/card_reader.o 2>/dev/null | grep 'card_reader\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*card_reader\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/naomi/card_reader.c tools/naomi/card_reader_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/card_reader_test" "$WORK"/*.o
"$WORK/card_reader_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/naomi/card_reader_expected.txt \
   || { echo "card_reader_test: not as it was (tools/naomi/card_reader_expected.txt)"; exit 1; }
echo "card_reader_test: as before"
