#!/bin/sh
# What the render-to-texture watch asks of a renderer (rend/rtt_watch.c):
# see rtt_watch_test.c. Built with ASan and UBSan where the compiler has
# them.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/rtt_watch_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.c"
$CC $SAN -o "$WORK/san" "$WORK/san.c" 2>/dev/null || SAN=
DEFS="-DTARGET_NO_THREADS -DTARGET_LINUX_x64 -Icore -Icore/deps -Icore/libretro-common/include"
C89="-std=c89 -pedantic -Werror=declaration-after-statement -Wno-long-long -Wno-variadic-macros"
$CC $C89 -O1 -g -Wall $SAN -fno-sanitize-recover=all $DEFS \
   -c core/rend/rtt_watch.c -o "$WORK/rtt_watch.o"
$CC $C89 -O1 -g -Wall -Wno-unused-function $SAN -fno-sanitize-recover=all $DEFS \
   -o "$WORK/rtt_watch_test" tools/rend/rtt_watch_test.c "$WORK/rtt_watch.o"
"$WORK/rtt_watch_test"
