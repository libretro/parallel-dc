#!/bin/sh
# ACES 2.0 (Full)'s cube, baked by a worker of its own off the frame, is
# the cube baked in place (hdr_cube.c); with threads (under TSan with
# SANITIZE=thread, ASan with SANITIZE=address) and without, where it is
# baked a fill at a time, the two dumps the same to the byte.
#
#   sh tools/hdr/hdr_cube.sh
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/hdr_cube.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
SAN=
[ -n "$SANITIZE" ] && SAN="-fsanitize=$SANITIZE -fno-omit-frame-pointer"
HDR="core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c"
LRC=core/libretro-common
$CC -O2 -g $SAN -Wall -I$LRC/include -o "$WORK/hdr_cube" tools/hdr/hdr_cube.c $HDR \
   $LRC/rthreads/rthreads.c $LRC/rthreads/retro_eventcount.c -lpthread -lm
$CC -O2 -g $SAN -Wall -DTARGET_NO_THREADS -I$LRC/include -o "$WORK/hdr_cube_nt" tools/hdr/hdr_cube.c $HDR -lm
"$WORK/hdr_cube"
"$WORK/hdr_cube_nt"
"$WORK/hdr_cube" --dump "$WORK/t.bin"
"$WORK/hdr_cube_nt" --dump "$WORK/nt.bin"
cmp "$WORK/t.bin" "$WORK/nt.bin"
echo "hdr_cube.sh: passed"
