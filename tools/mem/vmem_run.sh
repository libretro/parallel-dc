#!/bin/sh
# The SH4's address space (core/hw/mem/_vmem.c) by itself.
#
#   sh tools/mem/vmem_run.sh [extra C flags]
#
# vmem_test.c is built with _vmem.c, with the C flags the core is built
# with; what it hashes must be what it always was (vmem_expected.txt).
# Extra flags, such as -fsanitize=address,undefined, go to the compiler
# and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/vmem_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/mem/_vmem.o 2>/dev/null | grep '_vmem\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*_vmem\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/mem/_vmem.c tools/mem/vmem_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/vmem_test" "$WORK"/*.o
"$WORK/vmem_test" > "$WORK/out.txt"
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/mem/vmem_expected.txt \
   || { echo "vmem_test: not as it was (tools/mem/vmem_expected.txt)"; exit 1; }
echo "vmem_test: as before"
