#!/bin/sh
# The render-to-texture watch (core/rend/rtt_watch.c) by itself, with the
# threads and without them.
#
#   sh tools/rend/rtt_watch_run.sh [extra C flags]
#
# rtt_watch_rand.c is built with rtt_watch.c, with the C flags the core is
# built with; what it prints must be what it always was
# (rtt_watch_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/rtt_watch_rand.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/rend/rtt_watch.o 2>/dev/null | grep 'rtt_watch\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*rtt_watch\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}
L=core/libretro-common

for SRC in core/rend/rtt_watch.c tools/rend/rtt_watch_rand.c core/libretro/emu_baton.c \
      $L/rthreads/rthreads.c $L/rthreads/retro_eventcount.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/rtt_watch_rand" "$WORK/rtt_watch.o" "$WORK/rtt_watch_rand.o" \
   "$WORK/emu_baton.o" "$WORK/rthreads.o" "$WORK/retro_eventcount.o" -lpthread
for SRC in core/rend/rtt_watch.c tools/rend/rtt_watch_rand.c; do
   eval "$CFLAGS_T -DTARGET_NO_THREADS $EXTRA \"$SRC\" -o \"$WORK/nt_$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/rtt_watch_rand_nt" "$WORK/nt_rtt_watch.o" "$WORK/nt_rtt_watch_rand.o"
{ echo "threads:"; "$WORK/rtt_watch_rand"; echo "no threads:"; "$WORK/rtt_watch_rand_nt"; } > "$WORK/out.txt" \
   || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/rend/rtt_watch_expected.txt \
   || { diff "$WORK/out.txt" tools/rend/rtt_watch_expected.txt | head -n 8; echo "rtt_watch_rand: not as it was (tools/rend/rtt_watch_expected.txt)"; exit 1; }
echo "rtt_watch_rand: as before"
