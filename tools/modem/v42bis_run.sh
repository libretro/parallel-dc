#!/bin/sh
# V.42 bis compression (core/hw/modem/v42bis.c) by itself.
#
#   sh tools/modem/v42bis_run.sh [extra C flags]
#
# v42bis_test.c is built with v42bis.c, with the C flags the core is built
# with; what it gives back must be what went in, and what it hashes what it
# always was (v42bis_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/v42bis_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/modem/v42bis.o 2>/dev/null | grep 'v42bis\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*v42bis\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/modem/v42bis.c tools/modem/v42bis_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/v42bis_test" "$WORK"/*.o
"$WORK/v42bis_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/modem/v42bis_expected.txt \
   || { echo "v42bis_test: not as it was (tools/modem/v42bis_expected.txt)"; exit 1; }
echo "v42bis_test: as before"
