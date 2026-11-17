#!/bin/sh
# The temporal upscaler's side in the core (core/rend/upscaler.c) on frames
# made at random.
#
#   sh tools/rend/upscaler_run.sh [extra C flags]
#
# upscaler_rand.c is built with upscaler.c, with the C flags the core is
# built with; what it prints must be what it always was
# (upscaler_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/upscaler_rand.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/rend/upscaler.o 2>/dev/null | grep 'upscaler\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*upscaler\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/rend/upscaler.c tools/rend/upscaler_rand.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/upscaler_rand" "$WORK/upscaler.o" "$WORK/upscaler_rand.o" -lm
"$WORK/upscaler_rand" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/rend/upscaler_expected.txt \
   || { diff "$WORK/out.txt" tools/rend/upscaler_expected.txt | head -n 8; echo "upscaler_rand: not as it was (tools/rend/upscaler_expected.txt)"; exit 1; }
echo "upscaler_rand: as before"
