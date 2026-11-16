#!/bin/sh
# The tile accelerator's front and its frames (core/hw/pvr/ta.c,
# ta_ctx.c) by themselves.
#
#   sh tools/pvr/ta_run.sh [extra C flags]
#
# ta_test.c is built with them and ta_list.c, with the C flags the core is
# built with, and stands in for the rest of the machine; what it prints
# must be what it always was (ta_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/ta_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/pvr/ta_list.o 2>/dev/null | grep 'ta_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*ta_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/pvr/ta.c core/hw/pvr/ta_ctx.c core/hw/pvr/ta_list.c tools/pvr/ta_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/ta_test" "$WORK"/*.o -Wl,--wrap=posix_memalign
"$WORK/ta_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/pvr/ta_expected.txt \
   || { echo "ta_test: not as it was (tools/pvr/ta_expected.txt)"; exit 1; }
echo "ta_test: as before"
