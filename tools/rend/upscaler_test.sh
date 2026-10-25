#!/bin/sh
# The temporal upscaler's side in the core (rend/upscaler.h): the strips
# of a frame matched with the frame before's, the request's size, the
# jitter, the inputs handed over. See upscaler_test.cpp. Also with
# mingw-w64 where it is installed, as the Windows build compiles it.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/upscaler_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
FLAGS="-std=c++11 -Wall -Werror -Wno-unused-function -O1 -fno-rtti -fno-operator-names -D__LIBRETRO__ -DNO_VERIFY -DHOST_CPU=0x20000004
   -Icore -Icore/libretro -Icore/deps -Icore/libretro-common/include"
${CXX:-c++} $FLAGS -o "$WORK/upscaler_test" tools/rend/upscaler_test.cpp core/rend/upscaler.cpp
"$WORK/upscaler_test"
if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
   x86_64-w64-mingw32-g++ $FLAGS -static -o "$WORK/upscaler_test.exe" tools/rend/upscaler_test.cpp core/rend/upscaler.cpp
   if command -v wine >/dev/null 2>&1; then
      WINEDEBUG=-all wine "$WORK/upscaler_test.exe"
   fi
fi
