#!/bin/sh
# The Dreamcast's modem (core/hw/modem/modem.c), with V.42 (v42.c,
# v42bis.c), through its registers.
#
#   sh tools/modem/modem_run.sh [extra C flags]
#
# modem_test.c is built with them, with the C flags the core is built with,
# and stands in for the scheduler and the network; what it prints must be
# what it always was (modem_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/modem_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/modem/v42bis.o 2>/dev/null | grep 'v42bis\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*v42bis\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/hw/modem/modem.c core/hw/modem/v42.c core/hw/modem/v42bis.c tools/modem/modem_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/modem_test" "$WORK"/*.o
"$WORK/modem_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/modem/modem_expected.txt \
   || { echo "modem_test: not as it was (tools/modem/modem_expected.txt)"; exit 1; }
echo "modem_test: as before"
