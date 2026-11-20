#!/bin/sh
# The 32-bit ARM recompiler (core/rec-ARM/rec_arm.c).
#
#   sh tools/sh4/arm_run.sh [blocks] [extra C flags]
#
# x64_test.c, built for 32-bit ARM with arm-linux-gnueabihf-gcc (or gcc on
# such a host) and the C flags the core is built with there, with the
# recompiler and what it compiles from, and run (under qemu-arm on another
# host); it hashes the code made of random blocks, relinked and with its
# fast accesses rewritten, which must be what it made of them when last
# changed on purpose (arm_expected.txt holds 20000 and 5000 blocks). What the recompiler
# takes the address of as a number is put at an address of its own: the
# interpreter's handlers, the functions never called, sin_table,
# sh4_write_gen and the MMU's tables. Only does anything on Linux.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-20000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/arm_test.$$
mkdir -p "$WORK"
[ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT

case $(uname -s)-$(uname -m) in
   Linux-armv7*) A32CC=gcc; RUN="" ;;
   Linux-*)
      command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1 || { echo "arm_test: skipped (no arm-linux-gnueabihf-gcc)"; exit 0; }
      if command -v qemu-arm-static >/dev/null 2>&1; then RUN=qemu-arm-static
      elif command -v qemu-arm >/dev/null 2>&1; then RUN=qemu-arm
      else echo "arm_test: skipped (no qemu-arm)"; exit 0; fi
      A32CC=arm-linux-gnueabihf-gcc ;;
   *) echo "arm_test: skipped (not Linux)"; exit 0 ;;
esac

CCMD=$(make -n -B platform=unix ARCH=arm CC=$A32CC core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')
# (ARM code, with NEON, as the core's ARM builds are made: a call to Thumb
# code is made another way)
CFLAGS_T="$CFLAGS_T -marm -mfpu=neon-vfpv4"

REC=core/rec-ARM/rec_arm.c
[ -n "$REC_OBJ" ] && REC=
for SRC in core/hw/sh4/sh4_opcode_list.c core/hw/sh4/dyna/decoder.c core/hw/sh4/dyna/shil.c core/hw/sh4/dyna/ssa.c \
      core/hw/sh4/dyna/ssa_regalloc.c core/hw/sh4/sh4_core_regs.c $REC tools/sh4/x64_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
# (REC_OBJ: another build of the recompiler, such as the C++ one)
LIBS=-lm
[ -n "$REC_OBJ" ] && { cp "$REC_OBJ" "$WORK/rec_arm.o"; LIBS="-lstdc++ -lm"; }
nm -u "$WORK/sh4_opcode_list.o" | awk '{print $2}' \
   | grep '^\(i[01]\|reios_trap$\|iNotImplemented$\)' \
   | sort -u > "$WORK/handlers.txt"
for s in Do_Exception ngen_blockcheckfail ngen_LinkBlock_Generic_stub ngen_LinkBlock_cond_Branch_stub \
      ngen_LinkBlock_cond_Next_stub ngen_FailedToFindBlock_ no_update no_update_mmu intc_sched intc_sched_mmu; do
   echo "$s" >> "$WORK/handlers.txt"
done
{
   echo '.arm'
   echo '.section .handlers,"ax",%progbits'
   while read s; do echo ".globl $s"; echo ".type $s, %function"; echo "$s: bx lr"; done < "$WORK/handlers.txt"
   echo '.section .note.GNU-stack,"",%progbits'
} > "$WORK/handlers.S"
$A32CC -marm -c "$WORK/handlers.S" -o "$WORK/handlers.o"
$A32CC -marm $EXTRA -static -o "$WORK/arm_test" "$WORK"/*.o $LIBS \
   -Wl,--section-start=.handlers=0x00800000 -Wl,--defsym=sin_table=0x6C000000 -Wl,--defsym=sh4_write_gen=0x6C100000 \
   -Wl,--defsym=mmu_read_lut=0x6C200000 -Wl,--defsym=mmu_write_lut=0x6C600000
$RUN "$WORK/arm_test" "$N" 2>"$WORK/err.txt" > "$WORK/out.txt" || true
cat "$WORK/out.txt"
cat "$WORK/err.txt" >&2
# (one line, all of it the one expected: grep takes each line for a pattern)
[ "$(wc -l < "$WORK/out.txt")" -eq 1 ] && grep -qxF "$(cat "$WORK/out.txt")" tools/sh4/arm_expected.txt \
   || { echo "arm_test: the recompiler does not make what it made (tools/sh4/arm_expected.txt)"; exit 1; }
echo "arm_test: as before"
