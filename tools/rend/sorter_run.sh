#!/bin/sh
# The translucent polygons sorted (core/rend/sorter.c) by themselves, and
# the control words' fields as C has them (core/hw/pvr/ta_words.h).
#
#   sh tools/rend/sorter_run.sh [extra C flags]
#
# sorter_test.c is built with sorter.c and ta_list.c, with the C flags the
# core is built with; what it prints must be what it always was
# (sorter_expected.txt). tools/pvr/ta_words_test.cpp reads and changes
# every field both ways, ta_structs.h's unions and ta_words.h's macros.
# Extra flags, such as -fsanitize=address,undefined, go to the compiler and
# the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/sorter_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/pvr/ta_list.o 2>/dev/null | grep 'ta_list\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*ta_list\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}
XCMD=$(make -s print-cxx-compile 2>/dev/null)
[ -n "$XCMD" ] || { echo "cannot get the core's C++ compile flags from make" >&2; exit 1; }
CXXFLAGS_T=$(echo "$XCMD" | sed 's/-DGIT_VERSION=[^ ]* [^ ]*//')
CXX=${CXXFLAGS_T%% *}

eval "$CXXFLAGS_T $EXTRA tools/pvr/ta_words_test.cpp -o \"$WORK/ta_words_test.o\""
"$CXX" $EXTRA -o "$WORK/ta_words_test" "$WORK/ta_words_test.o"
"$WORK/ta_words_test"

for SRC in core/rend/sorter.c core/hw/pvr/ta_list.c tools/rend/sorter_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/sorter_test" "$WORK/sorter.o" "$WORK/ta_list.o" "$WORK/sorter_test.o"
"$WORK/sorter_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cat "$WORK/out.txt"
cmp -s "$WORK/out.txt" tools/rend/sorter_expected.txt \
   || { echo "sorter_test: not as it was (tools/rend/sorter_expected.txt)"; exit 1; }
echo "sorter_test: as before"
