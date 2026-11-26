#!/bin/sh
# The x86-64 recompiler (core/rec-x64/rec_x64.c).
#
#   sh tools/sh4/x64_run.sh [blocks] [extra C flags]
#   WINDOWS=1 sh tools/sh4/x64_run.sh [blocks]
#
# x64_test.c is built with the recompiler and what it compiles from (the
# decoder, the SSA optimizer, the register allocator), with the C flags
# the core is built with; it hashes the code made of random blocks, which
# must be what it made when last changed on purpose (x64_expected.txt holds
# 20000 and 5000 blocks, for Linux and macOS and for Windows, whose
# registers are not the same). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link. WINDOWS=1
# builds it for Windows with mingw-w64 and runs it under wine. Only does
# anything on x86-64.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
case $(uname -m) in
   x86_64|amd64) ;;
   *) echo "x64_test: skipped (the code under test is for x86-64)"; exit 0 ;;
esac
N=${1:-20000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/x64_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc"
   LIBS="-static"
   EXE="$WORK/x64_test.exe"
   RUN="wine"
else
   MAKEARGS=""
   LIBS="-lm"
   EXE="$WORK/x64_test"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/sh4_opcode_list.c core/hw/sh4/dyna/decoder.c core/hw/sh4/dyna/shil.c core/hw/sh4/dyna/ssa.c \
      core/hw/sh4/dyna/ssa_regalloc.c core/hw/sh4/sh4_core_regs.c core/rec-x64/rec_x64.c tools/sh4/x64_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
nm -u "$WORK/sh4_opcode_list.o" | awk '{print $2}' \
   | grep '^_\{0,1\}\(i[01]\|reios_trap$\|iNotImplemented$\)' \
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
# (one line, all of it the one expected: grep takes each line for a pattern)
[ "$(wc -l < "$WORK/out.txt")" -eq 1 ] && grep -qxF "$(cat "$WORK/out.txt")" tools/sh4/x64_expected.txt \
   || { echo "x64_test: the recompiler does not make what it made (tools/sh4/x64_expected.txt)"; exit 1; }
echo "x64_test: as before"
