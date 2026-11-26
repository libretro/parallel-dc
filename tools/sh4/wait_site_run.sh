#!/bin/sh
# The wait sites by themselves (core/hw/sh4/dyna/wait_site.c).
#
#   sh tools/sh4/wait_site_run.sh [blocks] [extra C flags]
#   WINDOWS=1 sh tools/sh4/wait_site_run.sh [blocks]
#
# wait_site_test.c is built with wait_site.c, with the C flags the core is
# built with; it hashes which random blocks get a site, and what
# sh4_wait_check() makes of random passes, which must be what the C++ made
# of them (wait_site_expected.txt holds 20000 and 100000 blocks). Extra
# flags, such as -fsanitize=address,undefined, go to the compiler and the
# link. WINDOWS=1 builds it for Windows with mingw-w64 and runs it under
# wine.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-100000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/wait_site_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc"
   LIBS="-static"
   EXE="$WORK/wait_site_test.exe"
   RUN="wine"
else
   MAKEARGS=""
   LIBS=""
   EXE="$WORK/wait_site_test"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/dyna/wait_site.c tools/sh4/wait_site_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$EXE" "$WORK"/*.o $LIBS
$RUN "$EXE" "$N" 2>"$WORK/err.txt" | tr -d '\r' > "$WORK/out.txt"
cat "$WORK/out.txt"
[ -n "$WINDOWS" ] || cat "$WORK/err.txt" >&2
[ -s "$WORK/out.txt" ] && grep -qxF "$(cat "$WORK/out.txt")" tools/sh4/wait_site_expected.txt \
   || { echo "wait_site_test: the sites are not as they were (tools/sh4/wait_site_expected.txt)"; exit 1; }
echo "wait_site_test: as before"
