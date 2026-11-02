#!/bin/sh
# The parse's arena (core/rend/parse_arena.c): see parse_arena_test.c.
# Built as C89, with ASan and UBSan where the compiler has them.
#
#   sh tools/rend/parse_arena_test.sh
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/parse_arena_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.c"
$CC $SAN -o "$WORK/san" "$WORK/san.c" 2>/dev/null || SAN=
$CC -std=c89 -pedantic -Wall -Wextra -Werror -Wno-long-long -O2 -g $SAN -fno-sanitize-recover=all \
   -Icore/rend -Icore/hw/pvr -Icore/libretro-common/include \
   -o "$WORK/parse_arena_test" tools/rend/parse_arena_test.c core/rend/parse_arena.c core/hw/pvr/ta_list.c
"$WORK/parse_arena_test"
