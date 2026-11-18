#!/bin/sh
# The recompilers' pool (node_pool.h): see node_pool_test.c. Built with
# ASan and UBSan where the compiler has them, and again optimised.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/node_pool_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.c"
$CC $SAN -o "$WORK/san" "$WORK/san.c" 2>/dev/null || SAN=
for opt in "-O1 -g $SAN -fno-sanitize-recover=all" "-O2"; do
   $CC -std=c89 -pedantic -Wall -Wextra $opt -Icore -o "$WORK/node_pool_test" tools/sh4/node_pool_test.c core/node_pool.c
   ASAN_OPTIONS=detect_leaks=1 "$WORK/node_pool_test"
done
