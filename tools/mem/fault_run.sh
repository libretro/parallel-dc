#!/bin/sh
# The host's faults the core takes for its own (core/libretro/common.c), by
# themselves.
#
#   sh tools/mem/fault_run.sh [extra C flags]
#   WINDOWS=1 sh tools/mem/fault_run.sh
#
# fault_test.c is built with common.c, with the C flags the core is built
# with, and run; every check it makes must pass. Extra flags, such as
# -fsanitize=undefined, go to the compiler and the link. WINDOWS=1 builds
# it for Windows with mingw-w64 and runs it under wine.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/fault_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++"
   LIBS="-static"
   EXE="$WORK/fault_test.exe"
   RUN="wine"
else
   MAKEARGS=""
   LIBS=""
   EXE="$WORK/fault_test"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/libretro/common.o 2>/dev/null | grep 'common\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*libretro/common\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/libretro/common.c tools/mem/fault_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$EXE" "$WORK"/*.o $LIBS
$RUN "$EXE" > "$WORK/out.txt" 2>/dev/null || true
tr -d '\r' < "$WORK/out.txt"
tr -d '\r' < "$WORK/out.txt" | grep -qx "fault: all passed"
