#!/bin/sh
# The texture cache's map (rend/tex_map.h): see tex_map_test.c. Built with
# ASan and UBSan where the compiler has them - every allocation filled with
# garbage, so that a texture the map does not make shows - and again
# optimised; the core's types and log stood in for.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/tex_map_test.$$
mkdir -p "$WORK/inc/log"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
cat > "$WORK/inc/types.h" <<'H'
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#include <stdbool.h>
H
printf '%s\n' 'extern int warned;' '#define WARN_LOG(type, ...) (warned++)' > "$WORK/inc/log/Log.h"
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.c"
$CC $SAN -o "$WORK/san" "$WORK/san.c" 2>/dev/null || SAN=
for opt in "-O1 -g $SAN -fno-sanitize-recover=all" "-O2"; do
   $CC -Wall -Wextra $opt -I"$WORK/inc" -Icore -Icore/libretro-common/include -c -o "$WORK/tex_map.o" core/rend/tex_map.c
   $CC -Wall -Wextra $opt -I"$WORK/inc" -Icore -Icore/libretro-common/include -o "$WORK/tex_map_test" tools/rend/tex_map_test.c "$WORK/tex_map.o"
   ASAN_OPTIONS=detect_leaks=1:max_malloc_fill_size=1073741824 "$WORK/tex_map_test"
done
