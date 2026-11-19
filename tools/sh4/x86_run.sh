#!/bin/sh
# The 32-bit x86 recompiler (core/rec-x86/rec_x86.c).
#
#   sh tools/sh4/x86_run.sh [blocks] [extra C flags]
#
# x64_test.c, built for 32-bit x86 with i686-linux-gnu-gcc (or gcc -m32)
# and the C flags the core is built with there, with the recompiler and
# what it compiles from; it hashes the code made of random blocks, which
# must be what the C++ recompiler made of them (x86_expected.txt holds
# 20000 and 5000 blocks). Only does anything on x86-64 and x86 Linux.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
case $(uname -m) in
   x86_64|amd64|i?86) ;;
   *) echo "x86_test: skipped (the code under test is for x86)"; exit 0 ;;
esac
N=${1:-20000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/x86_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if command -v i686-linux-gnu-gcc >/dev/null 2>&1; then
   X86CC=i686-linux-gnu-gcc
elif echo 'int main(void){return 0;}' | gcc -m32 -x c - -o "$WORK/m32" >/dev/null 2>&1; then
   X86CC=gcc
else
   echo "x86_test: skipped (no compiler for 32-bit x86)"
   exit 0
fi

CCMD=$(make -n -B platform=unix ARCH=i686 CC=$X86CC core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')

for SRC in core/hw/sh4/sh4_opcode_list.c core/hw/sh4/dyna/decoder.c core/hw/sh4/dyna/shil.c core/hw/sh4/dyna/ssa.c \
      core/hw/sh4/dyna/ssa_regalloc.c core/hw/sh4/sh4_core_regs.c core/rec-x86/rec_x86.c tools/sh4/x64_test.c; do
   eval "$CFLAGS_T -m32 $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
nm -u "$WORK/sh4_opcode_list.o" | awk '{print $2}' \
   | grep '^\(i[01]\|reios_trap$\|iNotImplemented$\)' \
   | sort -u > "$WORK/handlers.txt"
{
   echo ".text"
   while read s; do echo ".globl $s"; echo "$s: ret"; done < "$WORK/handlers.txt"
   echo '.section .note.GNU-stack,"",@progbits'
} > "$WORK/handlers.S"
$X86CC -m32 -c "$WORK/handlers.S" -o "$WORK/handlers.o"
$X86CC -m32 $EXTRA -static -o "$WORK/x86_test" "$WORK"/*.o -lm
"$WORK/x86_test" "$N" 2>"$WORK/err.txt" > "$WORK/out.txt" || true
cat "$WORK/out.txt"
cat "$WORK/err.txt" >&2
[ -s "$WORK/out.txt" ] && grep -qxF "$(cat "$WORK/out.txt")" tools/sh4/x86_expected.txt \
   || { echo "x86_test: the recompiler does not make what it made (tools/sh4/x86_expected.txt)"; exit 1; }
echo "x86_test: as before"
