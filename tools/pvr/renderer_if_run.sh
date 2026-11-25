#!/bin/sh
# The renderer interface (core/hw/pvr/Renderer_if.c and
# renderer_dispatch.c) by itself, with renderers that only say what they
# are asked (renderer_if_shim.cpp: C++ classes, rend/renderer_class.h).
#
#   sh tools/pvr/renderer_if_run.sh [extra flags]
#
# renderer_if_test.c is built with them, with the C and C++ flags the core
# is built with; what it prints must be what it always was
# (renderer_if_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compilers and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/renderer_if_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/pvr/Renderer_if.o 2>/dev/null | grep 'Renderer_if\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*Renderer_if\.c||; s/ -MMD -MP//')
XCMD=$(make -s print-cxx-compile 2>/dev/null)
[ -n "$XCMD" ] || { echo "cannot get the core's C++ compile flags from make" >&2; exit 1; }
CXXFLAGS_T=$(echo "$XCMD" | sed 's/-DGIT_VERSION=[^ ]* [^ ]*//')
CXX=${CXXFLAGS_T%% *}

for SRC in core/hw/pvr/Renderer_if.c core/hw/pvr/renderer_dispatch.c tools/pvr/renderer_if_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
for SRC in tools/pvr/renderer_if_shim.cpp; do
   eval "$CXXFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .cpp).o\""
done
"$CXX" $EXTRA -o "$WORK/renderer_if_test" "$WORK/Renderer_if.o" "$WORK/renderer_if_test.o" \
   "$WORK/renderer_dispatch.o" "$WORK/renderer_if_shim.o"
"$WORK/renderer_if_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/pvr/renderer_if_expected.txt \
   || { diff "$WORK/out.txt" tools/pvr/renderer_if_expected.txt | head -n 8; echo "renderer_if_test: not as it was (tools/pvr/renderer_if_expected.txt)"; exit 1; }
echo "renderer_if_test: as before"
