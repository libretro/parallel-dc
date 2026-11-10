#!/bin/sh
# The threads a texture scaled up is shared out to (core/rend/band_pool.h,
# tools/rend/band_pool_test.c): with threads, under the thread sanitizer
# and under the address and undefined-behaviour ones; and without threads.
# Given a built core, also that it needs no OpenMP runtime (libgomp).
#
#   sh tools/rend/band_pool_test.sh [core.so]     PASS when every one passes
#
# CC (default cc) builds it. With WINDOWS=1, CC a mingw-w64 one (default
# x86_64-w64-mingw32-gcc), built for Windows with no sanitizer and run
# with $WINE (default wine; "" on Windows).
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/band_pool_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
LRC=core/libretro-common
OURS="tools/rend/band_pool_test.c core/rend/band_pool.c"
THEIRS="$LRC/rthreads/rthreads.c $LRC/rthreads/retro_eventcount.c"
STRICT="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror -Wdeclaration-after-statement"
INC="-Icore/rend -I$LRC/include"

if [ -n "$1" ]; then
   if ${OBJDUMP:-objdump} -p "$1" | grep -qi "gomp"; then
      echo "band_pool_test: $1 needs OpenMP's runtime" >&2
      exit 1
   fi
fi

# @1 output, @2 "threads" or not, then the compiler's flags
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
   "$@" || { echo "band_pool_test: $name failed" >&2; exit 1; }
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
