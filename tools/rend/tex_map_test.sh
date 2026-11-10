#!/bin/sh
# The texture cache's map (rend/tex_map.h): see tex_map_test.cpp. Built with
# ASan and UBSan where the compiler has them - every allocation filled with
# garbage, so that a texture not value-initialized shows - and again
# optimised; the core's types and log stood in for.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/tex_map_test.$$
mkdir -p "$WORK/inc/log"
trap 'rm -rf "$WORK"' EXIT
CXX=${CXX:-c++}
cat > "$WORK/inc/types.h" <<'H'
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
H
echo '#define WARN_LOG(type, ...) (warned++)' > "$WORK/inc/log/Log.h"
SAN=-fsanitize=address,undefined
echo 'int main(){return 0;}' > "$WORK/san.cpp"
$CXX $SAN -o "$WORK/san" "$WORK/san.cpp" 2>/dev/null || SAN=
for opt in "-O1 -g $SAN -fno-sanitize-recover=all" "-O2"; do
   $CXX -std=c++11 -Wall -Wextra $opt -I"$WORK/inc" -Icore -o "$WORK/tex_map_test" tools/rend/tex_map_test.cpp
   ASAN_OPTIONS=detect_leaks=1:max_malloc_fill_size=1073741824 "$WORK/tex_map_test"
done
