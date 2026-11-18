#!/bin/sh
# What is plugged into the maple bus (core/hw/maple/maple_cfg.c) by itself,
# with devices that say what is done to them (maple_cfg_shim.cpp).
#
#   sh tools/maple/maple_cfg_run.sh [extra flags]
#
# maple_cfg_test.c is built with maple_cfg.c and maple_helper.c, with the
# C and C++ flags the core is built with; what it prints must be what it
# always was (maple_cfg_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compilers and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/maple_cfg_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/maple/maple_cfg.o 2>/dev/null | grep 'maple_cfg\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*maple_cfg\.c||; s/ -MMD -MP//')
XCMD=$(make -n -B core/libretro/libretro.o 2>/dev/null | grep 'libretro\.cpp' | tail -n 1)
[ -n "$XCMD" ] || { echo "cannot get the core's C++ compile flags from make" >&2; exit 1; }
XCMD=${XCMD% -o *}
CXXFLAGS_T=$(echo "$XCMD" | sed 's| [^ ]*libretro\.cpp||; s/ -MMD -MP//; s/-DGIT_VERSION=[^ ]* [^ ]*//')
CXX=${CXXFLAGS_T%% *}

for SRC in core/hw/maple/maple_cfg.c core/hw/maple/maple_helper.c tools/maple/maple_cfg_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
eval "$CXXFLAGS_T $EXTRA tools/maple/maple_cfg_shim.cpp -o \"$WORK/maple_cfg_shim.o\""
"$CXX" $EXTRA -o "$WORK/maple_cfg_test" "$WORK/maple_cfg.o" "$WORK/maple_helper.o" \
   "$WORK/maple_cfg_test.o" "$WORK/maple_cfg_shim.o" -lm
"$WORK/maple_cfg_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/maple/maple_cfg_expected.txt \
   || { diff "$WORK/out.txt" tools/maple/maple_cfg_expected.txt | head -n 8; echo "maple_cfg_test: not as it was (tools/maple/maple_cfg_expected.txt)"; exit 1; }
echo "maple_cfg_test: as before"
