#!/bin/sh
# The host's side of the SH4's address space (core/libretro/vmem_utils.c)
# by itself.
#
#   sh tools/mem/vmem_platform_run.sh [extra C flags]
#   WINDOWS=1 sh tools/mem/vmem_platform_run.sh
#
# vmem_platform_test.c is built with vmem_utils.c, with the C flags the
# core is built with, and run; every check it makes must pass. Extra
# flags, such as -fsanitize=undefined, go to the compiler and the link.
# WINDOWS=1 builds it for Windows with mingw-w64 and runs it under wine.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/vmem_platform_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc"
   LIBS="-static"
   EXE="$WORK/vmem_platform_test.exe"
   RUN="wine"
else
   MAKEARGS=""
   LIBS="-lrt"
   EXE="$WORK/vmem_platform_test"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/libretro/vmem_utils.o 2>/dev/null | grep 'vmem_utils\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*vmem_utils\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/libretro/vmem_utils.c tools/mem/vmem_platform_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$EXE" "$WORK"/*.o $LIBS
$RUN "$EXE" > "$WORK/out.txt" 2>/dev/null || true
tr -d '\r' < "$WORK/out.txt"
tr -d '\r' < "$WORK/out.txt" | grep -qx "vmem_platform: all passed"
