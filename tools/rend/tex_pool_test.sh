#!/bin/sh
# What the texture pool makes at the start (rend/tex_pool.h):
# see tex_pool_test.cpp. Built with ASan and UBSan where the compiler has
# them.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/tex_pool_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CXX=${CXX:-c++}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.cpp"
$CXX $SAN -o "$WORK/san" "$WORK/san.cpp" 2>/dev/null || SAN=
$CXX -std=c++11 -O1 -g -Wall -Wno-unused-function -Wno-uninitialized $SAN -fno-sanitize-recover=all -DTARGET_NO_THREADS -DTARGET_LINUX_x64 \
   -Icore -Icore/deps -Icore/libretro-common/include \
   -o "$WORK/tex_pool_test" tools/rend/tex_pool_test.cpp
"$WORK/tex_pool_test"
