#!/bin/sh
# What the core reads of a disc's IP.BIN (core/reios/reios.h), by itself.
#
#   sh tools/reios/ip_meta_run.sh [extra C flags]
#
# ip_meta_test.c is built with the C flags the core's C files are built
# with (nullDC.c's), and as C++ with the C++ files' (reios.cpp's): the
# header is read by both. Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/ip_meta_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/nullDC.o 2>/dev/null | grep 'nullDC\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*nullDC\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

XCMD=$(make -n -B core/reios/reios.o 2>/dev/null | grep 'reios\.cpp' | tail -n 1)
[ -n "$XCMD" ] || { echo "cannot get the core's C++ compile flags from make" >&2; exit 1; }
XCMD=${XCMD% -o *}
CXXFLAGS_T=$(echo "$XCMD" | sed 's| [^ ]*reios\.cpp||; s/ -MMD -MP//')
CXX=${CXXFLAGS_T%% *}

eval "$CFLAGS_T $EXTRA tools/reios/ip_meta_test.c -o \"$WORK/ip_meta_c.o\""
"$CC" $EXTRA -o "$WORK/ip_meta_c" "$WORK/ip_meta_c.o"
"$WORK/ip_meta_c"
eval "$CXXFLAGS_T $EXTRA -x c++ tools/reios/ip_meta_test.c -o \"$WORK/ip_meta_cxx.o\""
"$CXX" $EXTRA -o "$WORK/ip_meta_cxx" "$WORK/ip_meta_cxx.o"
"$WORK/ip_meta_cxx"
