#!/bin/sh
# The AArch64 recompiler (core/rec-ARM64/rec_arm64.c).
#
#   sh tools/sh4/arm64_run.sh [blocks] [extra C flags]
#
# x64_test.c, built for AArch64 with aarch64-linux-gnu-gcc (or gcc on an
# AArch64 host) and the C flags the core is built with there, with the
# recompiler and what it compiles from, and run (under qemu-aarch64 on
# another host); it hashes the code made of random blocks, which must be
# what the C++ recompiler made of them (arm64_expected.txt holds 20000 and
# 5000 blocks). What the recompiler takes the address of as a number is put
# at an address of its own: the interpreter's handlers, the functions
# never called, sin_table and sh4_write_gen. Only does anything on Linux.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-20000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/arm64_test.$$
mkdir -p "$WORK"
[ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT

case $(uname -s)-$(uname -m) in
   Linux-aarch64|Linux-arm64) A64CC=gcc; RUN="" ;;
   Linux-*)
      command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || { echo "arm64_test: skipped (no aarch64-linux-gnu-gcc)"; exit 0; }
      if command -v qemu-aarch64-static >/dev/null 2>&1; then RUN=qemu-aarch64-static
      elif command -v qemu-aarch64 >/dev/null 2>&1; then RUN=qemu-aarch64
      else echo "arm64_test: skipped (no qemu-aarch64)"; exit 0; fi
      A64CC=aarch64-linux-gnu-gcc ;;
   *) echo "arm64_test: skipped (not Linux)"; exit 0 ;;
esac

CCMD=$(make -n -B platform=unix ARCH=aarch64 CC=$A64CC core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')

REC=core/rec-ARM64/rec_arm64.c
[ -n "$REC_OBJ" ] && REC=
for SRC in core/hw/sh4/sh4_opcode_list.c core/hw/sh4/dyna/decoder.c core/hw/sh4/dyna/shil.c core/hw/sh4/dyna/ssa.c \
      core/hw/sh4/dyna/ssa_regalloc.c core/hw/sh4/sh4_core_regs.c $REC tools/sh4/x64_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
# (REC_OBJ: another build of the recompiler, such as the C++ one)
LIBS=-lm
[ -n "$REC_OBJ" ] && { cp "$REC_OBJ" "$WORK/rec_arm64.o"; LIBS="-lstdc++ -lm"; }
nm -u "$WORK/sh4_opcode_list.o" | awk '{print $2}' \
   | grep '^\(i[01]\|reios_trap$\|iNotImplemented$\)' \
   | sort -u > "$WORK/handlers.txt"
for s in Do_Exception ngen_blockcheckfail ngen_LinkBlock_Generic_stub ngen_LinkBlock_cond_Branch_stub \
      ngen_LinkBlock_cond_Next_stub ngen_FailedToFindBlock_mmu ngen_FailedToFindBlock_nommu UpdateSystem; do
   echo "$s" >> "$WORK/handlers.txt"
done
{
   echo '.section .handlers,"ax",@progbits'
   while read s; do echo ".globl $s"; echo "$s: ret"; done < "$WORK/handlers.txt"
   echo '.section .note.GNU-stack,"",@progbits'
} > "$WORK/handlers.S"
$A64CC -c "$WORK/handlers.S" -o "$WORK/handlers.o"
$A64CC $EXTRA -static -o "$WORK/arm64_test" "$WORK"/*.o $LIBS \
   -Wl,--section-start=.handlers=0x03000000 -Wl,--defsym=sin_table=0x6C000000 -Wl,--defsym=sh4_write_gen=0x6C100000
$RUN "$WORK/arm64_test" "$N" 2>"$WORK/err.txt" > "$WORK/out.txt" || true
cat "$WORK/out.txt"
cat "$WORK/err.txt" >&2
# (one line, all of it the one expected: grep takes each line for a pattern)
[ "$(wc -l < "$WORK/out.txt")" -eq 1 ] && grep -qxF "$(cat "$WORK/out.txt")" tools/sh4/arm64_expected.txt \
   || { echo "arm64_test: the recompiler does not make what it made (tools/sh4/arm64_expected.txt)"; exit 1; }
echo "arm64_test: as before"
