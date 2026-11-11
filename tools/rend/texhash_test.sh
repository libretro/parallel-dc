#!/bin/sh
# The textures' hashes as xxHash's XXH32 made them (tools/rend/texhash_test.c),
# under the address and undefined-behaviour sanitizers; with WINDOWS=1 for
# Windows too (CC default x86_64-w64-mingw32-gcc, run with $WINE, default
# wine).
#
#   sh tools/rend/texhash_test.sh
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/texhash_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
SRC="tools/rend/texhash_test.c core/libretro-common/encodings/encoding_xxh32.c"
STRICT="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror -Icore/libretro-common/include"
if [ "$WINDOWS" = 1 ]; then
   ${CC:-x86_64-w64-mingw32-gcc} $STRICT -O2 -o "$WORK/t.exe" $SRC
   ${WINE-wine} "$WORK/t.exe"
else
   ${CC:-cc} $STRICT -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o "$WORK/t" $SRC
   "$WORK/t"
fi
