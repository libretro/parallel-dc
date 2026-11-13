#!/bin/sh
# The games' fixes and widescreen cheats (core/cheats.c), by themselves.
#
#   sh tools/cheats/cheats_run.sh [extra C flags]
#
# cheats_test.c is built with cheats.c, with the C flags the core is
# built with, and stands in for the rest of the machine; see the test for
# what it checks. Extra flags, such as -fsanitize=address,undefined, go to
# the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/cheats_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/cheats.o 2>/dev/null | grep 'cheats\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*cheats\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/cheats.c tools/cheats/cheats_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/cheats_test" "$WORK"/*.o
"$WORK/cheats_test"
