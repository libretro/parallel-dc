#!/bin/sh
# The ELF reader (core/deps/libelf) by itself; see elf_test.c.
#
#   sh tools/libelf/elf_test.sh
#
# Built C89 and run under the address and undefined-behaviour sanitizers;
# with WINDOWS=1 for 64-bit Windows instead (CC default
# x86_64-w64-mingw32-gcc, run with $WINE, default wine).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WORK=${TMPDIR:-/tmp}/libelf_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

SRC="tools/libelf/elf_test.c core/deps/libelf/elf.c core/deps/libelf/elf32.c core/deps/libelf/elf64.c"
STRICT="-std=c89 -pedantic-errors -Wno-long-long -Werror=declaration-after-statement -Icore -Icore/libretro-common/include"
if [ "$WINDOWS" = 1 ]; then
   ${CC:-x86_64-w64-mingw32-gcc} $STRICT -O2 -o "$WORK/t.exe" $SRC
   ${WINE-wine} "$WORK/t.exe"
else
   ${CC:-cc} $STRICT -D_DEFAULT_SOURCE -O1 -g -fsanitize=address,undefined \
      -fno-sanitize-recover=all -o "$WORK/t" $SRC
   "$WORK/t"
fi
