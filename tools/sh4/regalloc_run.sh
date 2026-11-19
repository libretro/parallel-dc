#!/bin/sh
# The register allocator by itself (core/hw/sh4/dyna/ssa_regalloc.c).
#
#   sh tools/sh4/regalloc_run.sh [blocks] [extra C flags]
#   WINDOWS=1 sh tools/sh4/regalloc_run.sh [blocks]
#
# regalloc_test.c is built with ssa_regalloc.c, ssa.c, shil.c, decoder.c and
# sh4_opcode_list.c, with the C flags the core is built with and what else
# they call stubbed; it hashes where the allocator puts each register at
# each operation, and the loads and write-backs it asks for, which must be
# what the C++ allocator did (regalloc_expected.txt holds 5000 and 20000
# blocks). Extra flags, such as -fsanitize=address,undefined, go to
# the compiler and the link. WINDOWS=1 builds it for Windows with mingw-w64
# and runs it under wine.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-20000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/regalloc_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++"
   LIBS="-static"
   EXE="$WORK/regalloc_test.exe"
   RUN="wine"
else
   MAKEARGS=""
   LIBS="-lm"
   EXE="$WORK/regalloc_test"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/sh4_opcode_list.c core/hw/sh4/dyna/decoder.c core/hw/sh4/dyna/shil.c core/hw/sh4/dyna/ssa.c core/hw/sh4/dyna/ssa_regalloc.c tools/sh4/regalloc_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
# The interpreter's handlers, and what the operations' C calls of the rest
# of the core: each the address of a return
{ nm -u "$WORK/sh4_opcode_list.o"; nm -u "$WORK/shil.o"; } | awk '{print $2}' \
   | grep '^_\{0,1\}\(i[01]\|reios_trap$\|iNotImplemented$\|UpdateFPSCR$\|UpdateSR$\|do_sqw_mmu$\|ngen_CC_\)' \
   | sort -u > "$WORK/handlers.txt"
{
   echo ".text"
   while read s; do echo ".globl $s"; echo "$s: ret"; done < "$WORK/handlers.txt"
   [ -n "$WINDOWS" ] || echo '.section .note.GNU-stack,"",@progbits'
} > "$WORK/handlers.S"
"$CC" -c "$WORK/handlers.S" -o "$WORK/handlers.o"
"$CC" $EXTRA -o "$EXE" "$WORK"/*.o $LIBS
$RUN "$EXE" "$N" 2>"$WORK/err.txt" | tr -d '\r' > "$WORK/out.txt"
cat "$WORK/out.txt"
[ -n "$WINDOWS" ] || cat "$WORK/err.txt" >&2
[ -s "$WORK/out.txt" ] && grep -qxF "$(cat "$WORK/out.txt")" tools/sh4/regalloc_expected.txt \
   || { echo "regalloc_test: the allocator does not do as it did (tools/sh4/regalloc_expected.txt)"; exit 1; }
echo "regalloc_test: as before"
