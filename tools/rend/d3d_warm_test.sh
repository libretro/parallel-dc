#!/bin/sh
# The Direct3D renderers' thread ahead and their file of compiled shaders,
# with no Direct3D (tools/rend/d3d_warm_test.c): with threads, under the
# thread sanitizer and under the address and undefined-behaviour ones;
# and without threads.
#
#   sh tools/rend/d3d_warm_test.sh     PASS when every one passes
#
# CC (default cc) builds it. With WINDOWS=1, CC a mingw-w64 one (default
# x86_64-w64-mingw32-gcc), built for Windows with no sanitizer and run
# with $WINE (default wine; "" on Windows).
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/d3d_warm_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
LRC=core/libretro-common
OURS="tools/rend/d3d_warm_test.c core/rend/d3d_warm.c core/rend/d3d_shader_cache.c"
THEIRS="$LRC/queues/retro_spsc.c $LRC/rthreads/rthreads.c"
STRICT="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror -Wdeclaration-after-statement"
INC="-Icore/rend -I$LRC/include"

# @1 output, @2 "threads" or not, then the compiler's flags: what is here
# warning-free and C89, what it uses from libretro-common as it is
build() {
   out=$1; threads=$2; shift 2
   objs=
   if [ "$threads" = threads ]; then
      for f in $THEIRS; do
         o="$WORK/$(basename "$out")_$(basename "$f" .c).o"
         $CC -g -O1 -DHAVE_THREADS $INC "$@" -c -o "$o" "$f"
         objs="$objs $o"
      done
      set -- -DHAVE_THREADS "$@"
   fi
   $CC $STRICT -g -O1 $INC "$@" -o "$out" $OURS $objs $LIBS
}

run() {
   name=$1; shift
   "$@" || { echo "d3d_warm_test: $name failed" >&2; exit 1; }
}

if [ "$WINDOWS" = 1 ]; then
   CC=${CC:-x86_64-w64-mingw32-gcc}
   LIBS=
   build "$WORK/t.exe" threads
   build "$WORK/nt.exe" none
   run threads ${WINE-wine} "$WORK/t.exe"
   run "no threads" ${WINE-wine} "$WORK/nt.exe"
   exit 0
fi
CC=${CC:-cc}
LIBS=-pthread
build "$WORK/tsan" threads -fsanitize=thread
build "$WORK/asan" threads -fsanitize=address,undefined -fno-sanitize-recover=all
build "$WORK/nt" none -fsanitize=address,undefined -fno-sanitize-recover=all
TSAN_OPTIONS=halt_on_error=1 run tsan "$WORK/tsan"
run asan "$WORK/asan"
run "no threads" "$WORK/nt"
