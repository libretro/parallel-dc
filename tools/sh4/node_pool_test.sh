#!/bin/sh
# The recompilers' pool (node_pool.h): see node_pool_test.cpp. Built with
# ASan and UBSan where the compiler has them, and again optimised.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/node_pool_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CXX=${CXX:-c++}
SAN=-fsanitize=address,undefined
echo 'int main(){return 0;}' > "$WORK/san.cpp"
$CXX $SAN -o "$WORK/san" "$WORK/san.cpp" 2>/dev/null || SAN=
for opt in "-O1 -g $SAN -fno-sanitize-recover=all" "-O2"; do
   $CXX -std=c++11 -Wall -Wextra $opt -Icore -o "$WORK/node_pool_test" tools/sh4/node_pool_test.cpp
   ASAN_OPTIONS=detect_leaks=1 "$WORK/node_pool_test"
done
