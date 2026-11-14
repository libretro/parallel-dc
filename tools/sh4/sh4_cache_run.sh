#!/bin/sh
# The SH4's instruction and operand caches by themselves
# (core/hw/sh4/sh4_cache.c).
#
#   sh tools/sh4/sh4_cache_run.sh [extra C flags]
#
# sh4_cache_test.c is built with sh4_cache.c, with the C flags the core is
# built with and STRICT_MODE, and stands in for memory, the TLB and the
# registers; what it hashes must be what it always was
# (sh4_cache_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/sh4_cache_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/sh4/sh4_cache.o 2>/dev/null | grep 'sh4_cache\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_cache\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/sh4_cache.c tools/sh4/sh4_cache_test.c; do
   eval "$CFLAGS_T -DSTRICT_MODE $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/sh4_cache_test" "$WORK"/*.o
"$WORK/sh4_cache_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/sh4/sh4_cache_expected.txt \
   || { echo "sh4_cache_test: the caches are not as they were (tools/sh4/sh4_cache_expected.txt)"; exit 1; }
echo "sh4_cache_test: as before"
