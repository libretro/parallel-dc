#!/bin/sh
# The register allocator's lists (ShilRegList) against the std::set they replace
# (reglist_test.cpp): the same order, so the same code.
#
#   sh tools/sh4/reglist_test.sh [trials]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${TMPDIR:-/tmp}/reglist_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CXX:-c++} -O2 -std=c++11 -Wall -I"$ROOT/core" -I"$ROOT/core/deps" -I"$ROOT/core/libretro-common/include" \
   -o "$WORK/reglist_test" "$ROOT/tools/sh4/reglist_test.cpp"
"$WORK/reglist_test" "$@"
