#!/bin/sh
# The locks on video memory (core/rend/vram_lock.c) by themselves.
#
#   sh tools/rend/vram_lock_run.sh [extra C flags]
#
# vram_lock_test.c is built with vram_lock.c, with the C flags the core is
# built with; what it prints must be what it always was
# (vram_lock_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/vram_lock_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/rend/vram_lock.o 2>/dev/null | grep 'vram_lock\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*vram_lock\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/rend/vram_lock.c tools/rend/vram_lock_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/vram_lock_test" "$WORK/vram_lock.o" "$WORK/vram_lock_test.o"
{ "$WORK/vram_lock_test" 0 && "$WORK/vram_lock_test" 1; } > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/rend/vram_lock_expected.txt \
   || { diff "$WORK/out.txt" tools/rend/vram_lock_expected.txt | head -n 8; echo "vram_lock_test: not as it was (tools/rend/vram_lock_expected.txt)"; exit 1; }
echo "vram_lock_test: as before"
