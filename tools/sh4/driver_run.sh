#!/bin/sh
# The recompiler's driver with the block manager (core/hw/sh4/dyna/driver.c).
#
#   sh tools/sh4/driver_run.sh [blocks] [extra C flags]
#   WINDOWS=1 sh tools/sh4/driver_run.sh [blocks]
#
# driver_test.c is built with driver.c, blockmanager.c and node_pool.c,
# with the C flags the core is built with; it hashes what comes of random
# code compiled, linked and failing its checks, which must be what came of
# it with the C++ driver (driver_expected.txt holds 20000 and 200000 steps).
# Extra
# flags, such as -fsanitize=address,undefined, go to the compiler and the
# link. WINDOWS=1 builds it for Windows with mingw-w64 and runs it under
# wine.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-200000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/driver_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++"
   LIBS="-static"
   EXE="$WORK/driver_test.exe"
   RUN="wine"
else
   MAKEARGS=""
   LIBS=""
   EXE="$WORK/driver_test"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/dyna/driver.c core/hw/sh4/dyna/blockmanager.c core/node_pool.c core/libretro-common/encodings/encoding_xxh32.c tools/sh4/driver_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$EXE" "$WORK"/*.o $LIBS
$RUN "$EXE" "$N" 2>"$WORK/err.txt" | tr -d '\r' > "$WORK/out.txt"
cat "$WORK/out.txt"
[ -n "$WINDOWS" ] || cat "$WORK/err.txt" >&2
[ -s "$WORK/out.txt" ] && [ -z "$(grep -vxF -f tools/sh4/driver_expected.txt "$WORK/out.txt")" ] \
   || { echo "driver_test: the driver does not do as it did (tools/sh4/driver_expected.txt)"; exit 1; }
echo "driver_test: as before"
