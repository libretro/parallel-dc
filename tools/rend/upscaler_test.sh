#!/bin/sh
# The temporal upscaler's side in the core (rend/upscaler.h): the strips
# of a frame matched with the frame before's, the request's size, the
# jitter, the inputs handed over. See upscaler_test.c. Also with
# mingw-w64 where it is installed, as the Windows build compiles it.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/upscaler_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CFLAGS="-std=c89 -Wall -Werror -O1 -Icore/libretro-common/include"
CFLAGS_U="$CFLAGS -pedantic -Wno-long-long -Wno-variadic-macros -D__LIBRETRO__ -DHOST_CPU=0x20000004 -Icore -Icore/libretro -Icore/deps"
${CC:-cc} $CFLAGS -c -o "$WORK/ta_list.o" core/hw/pvr/ta_list.c
${CC:-cc} $CFLAGS_U -c -o "$WORK/upscaler.o" core/rend/upscaler.c
${CC:-cc} $CFLAGS_U -Wno-unused-function -o "$WORK/upscaler_test" tools/rend/upscaler_test.c "$WORK/upscaler.o" "$WORK/ta_list.o" -lm
"$WORK/upscaler_test"
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
   x86_64-w64-mingw32-gcc $CFLAGS -c -o "$WORK/ta_list_w.o" core/hw/pvr/ta_list.c
   x86_64-w64-mingw32-gcc $CFLAGS_U -c -o "$WORK/upscaler_w.o" core/rend/upscaler.c
   x86_64-w64-mingw32-gcc $CFLAGS_U -Wno-unused-function -static -o "$WORK/upscaler_test.exe" tools/rend/upscaler_test.c "$WORK/upscaler_w.o" \
      "$WORK/ta_list_w.o"
   if command -v wine >/dev/null 2>&1; then
      WINEDEBUG=-all wine "$WORK/upscaler_test.exe"
   fi
fi
