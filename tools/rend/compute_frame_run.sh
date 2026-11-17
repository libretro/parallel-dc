#!/bin/sh
# The compute renderer's frame words (core/rend/tile/compute_frame.c) for
# frames, registers and renderers made at random.
#
#   sh tools/rend/compute_frame_run.sh [extra C flags]
#
# compute_frame_test.c is built with compute_frame.c, with the C flags the
# core is built with; what it prints must be what it always was
# (compute_frame_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/compute_frame_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/rend/tile/compute_frame.o 2>/dev/null | grep 'compute_frame\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*compute_frame\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/rend/tile/compute_frame.c tools/rend/compute_frame_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/compute_frame_test" "$WORK/compute_frame.o" "$WORK/compute_frame_test.o" -lm
"$WORK/compute_frame_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/rend/compute_frame_expected.txt \
   || { diff "$WORK/out.txt" tools/rend/compute_frame_expected.txt | head -n 8; echo "compute_frame_test: not as it was (tools/rend/compute_frame_expected.txt)"; exit 1; }
echo "compute_frame_test: as before"
