#!/bin/sh
# What the render-to-texture watch asks of a renderer (rend/rtt_watch.cpp):
# see rtt_watch_test.cpp. Built with ASan and UBSan where the compiler has
# them.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/rtt_watch_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CXX=${CXX:-c++}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.cpp"
$CXX $SAN -o "$WORK/san" "$WORK/san.cpp" 2>/dev/null || SAN=
$CXX -std=c++11 -O1 -g -Wall -Wno-unused-function -Wno-uninitialized $SAN -fno-sanitize-recover=all -DTARGET_NO_THREADS -DTARGET_LINUX_x64 \
   -Icore -Icore/deps -Icore/libretro-common/include \
   -o "$WORK/rtt_watch_test" tools/rend/rtt_watch_test.cpp core/rend/rtt_watch.cpp
"$WORK/rtt_watch_test"
