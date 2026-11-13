#!/bin/sh
# The SH4 opcode table by itself (core/hw/sh4/sh4_opcode_list.c).
#
#   sh tools/sh4/opcode_table_run.sh [extra C flags]
#
# opcode_table_test.c is built with sh4_opcode_list.c, with the C flags the
# core is built with, and every handler the table names stubbed; it hashes
# what the table says of each opcode, which must be what it always said
# (opcode_table_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/opcode_table_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/sh4/sh4_opcode_list.o 2>/dev/null | grep 'sh4_opcode_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*sh4_opcode_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/sh4/sh4_opcode_list.c tools/sh4/opcode_table_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
# The handlers: each the address of a return, all that is compared of them
nm -u "$WORK/sh4_opcode_list.o" | awk '{print $2}' | grep '^i[01]\|^dec_i\|^reios_trap$\|^iNotImplemented$\|^os_DebugBreak$' \
   > "$WORK/handlers.txt"
{
   echo ".text"
   while read s; do echo ".globl $s"; echo "$s: ret"; done < "$WORK/handlers.txt"
   echo '.section .note.GNU-stack,"",@progbits'
} > "$WORK/handlers.S"
"$CC" -c "$WORK/handlers.S" -o "$WORK/handlers.o"
"$CC" $EXTRA -o "$WORK/opcode_table_test" "$WORK"/*.o
"$WORK/opcode_table_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/sh4/opcode_table_expected.txt \
   || { echo "opcode_table_test: the table is not as it was (tools/sh4/opcode_table_expected.txt)"; exit 1; }
echo "opcode_table_test: as before"
