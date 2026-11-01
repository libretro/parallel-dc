#!/bin/sh
# The Vulkan per-pixel renderer's bands (rend/oit_bands.h): see
# oit_bands_test.c. Built as C89, with ASan and UBSan where the compiler has
# them, and again optimised.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/oit_bands_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.c"
$CC $SAN -o "$WORK/san" "$WORK/san.c" 2>/dev/null || SAN=
for opt in "-O1 -g $SAN -fno-sanitize-recover=all" "-O2"; do
   $CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long $opt -Icore/libretro-common/include \
      -o "$WORK/oit_bands_test" tools/rend/oit_bands_test.c
   "$WORK/oit_bands_test"
done
