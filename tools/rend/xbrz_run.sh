#!/bin/sh
# xBRZ (core/deps/xbrz/xbrz.c) by itself (xbrz_test.c), with the C flags
# the core is built with: what it makes must be what it always was
# (xbrz_expected.txt, from xBRZ 1.5's C++).
#
#   sh tools/rend/xbrz_run.sh [extra flags]
#
# Extra flags, such as -fsanitize=address,undefined, go to the compiler
# and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/xbrz_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/deps/xbrz/xbrz.o 2>/dev/null | grep 'xbrz\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*xbrz\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/deps/xbrz/xbrz.c tools/rend/xbrz_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/xbrz_test" "$WORK/xbrz.o" "$WORK/xbrz_test.o" -lm
"$WORK/xbrz_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/rend/xbrz_expected.txt \
   || { diff "$WORK/out.txt" tools/rend/xbrz_expected.txt | head -n 8; echo "xbrz_test: not as it was (tools/rend/xbrz_expected.txt)"; exit 1; }
echo "xbrz_test: as before"
