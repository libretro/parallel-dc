#!/bin/sh
# The translucent sorter's sort (rend/sort_pairs.h): see sort_pairs_test.c.
# Built as C89, with ASan and UBSan where the compiler has them, and again
# optimised.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/sort_pairs_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.c"
$CC $SAN -o "$WORK/san" "$WORK/san.c" 2>/dev/null || SAN=
for opt in "-O1 -g $SAN -fno-sanitize-recover=all" "-O2"; do
   $CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long $opt -Icore/libretro-common/include \
      -o "$WORK/sort_pairs_test" tools/rend/sort_pairs_test.c
   "$WORK/sort_pairs_test"
done
