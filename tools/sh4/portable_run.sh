#!/bin/sh
# The portable recompiler (core/rec-portable/rec_portable.c) against the
# interpreter, on blocks of random code (tools/sh4/portable_test.c):
#
#   sh tools/sh4/portable_run.sh [blocks] [compiler flags]
#
# It is C and runs on any processor; built here for this one.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-5000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/portable_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T="$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//') -DTARGET_PORTABLE_REC"
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/sh4_opcode_list.c core/hw/sh4/interpr/sh4_opcodes.c core/hw/sh4/interpr/sh4_fpu.c \
      core/hw/sh4/sh4_core_regs.c core/hw/sh4/dyna/decoder.c core/hw/sh4/dyna/shil.c core/hw/sh4/dyna/ssa.c \
      core/rec-portable/rec_portable.c tools/sh4/portable_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/portable_test" "$WORK"/*.o -lm
"$WORK/portable_test" "$N"
