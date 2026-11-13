#!/bin/sh
# Where the core writes its files (core/paths.c), by itself.
#
#   sh tools/paths/paths_run.sh [extra C flags]
#
# paths_test.c is built with paths.c, with the C flags the core is built
# with; see the test for what it checks. Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/paths_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/paths.o 2>/dev/null | grep 'paths\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*paths\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/paths.c tools/paths/paths_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/paths_test" "$WORK"/*.o
"$WORK/paths_test"
