#!/bin/sh
# The block manager by itself (core/hw/sh4/dyna/blockmanager.c).
#
#   sh tools/sh4/blockmanager_run.sh [blocks] [extra C flags]
#   WINDOWS=1 sh tools/sh4/blockmanager_run.sh [blocks]
#
# blockmanager_test.c is built with blockmanager.c and node_pool.c, with
# the C flags the core is built with; it hashes what the block manager does
# over random blocks made, linked, discarded and written over, which must be
# what the C++ did (blockmanager_expected.txt holds 20000 and 200000 steps,
# each of the three ways main memory is laid out). Extra
# flags, such as -fsanitize=address,undefined, go to the compiler and the
# link. WINDOWS=1 builds it for Windows with mingw-w64 and runs it under
# wine.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-200000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/blockmanager_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc"
   LIBS="-static"
   EXE="$WORK/blockmanager_test.exe"
   RUN="wine"
else
   MAKEARGS=""
   LIBS=""
   EXE="$WORK/blockmanager_test"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/dyna/blockmanager.c core/node_pool.c tools/sh4/blockmanager_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$EXE" "$WORK"/*.o $LIBS
$RUN "$EXE" "$N" 2>"$WORK/err.txt" | tr -d '\r' > "$WORK/out.txt"
cat "$WORK/out.txt"
[ -n "$WINDOWS" ] || cat "$WORK/err.txt" >&2
[ -s "$WORK/out.txt" ] && [ -z "$(grep -vxF -f tools/sh4/blockmanager_expected.txt "$WORK/out.txt")" ] \
   || { echo "blockmanager_test: the block manager does not do as it did (tools/sh4/blockmanager_expected.txt)"; exit 1; }
echo "blockmanager_test: as before"
