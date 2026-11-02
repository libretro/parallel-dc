#!/bin/sh
# A strip's indices (core/hw/pvr/ta_index.c) against the parse's loop as it
# was: see ta_index_test.c. Built as C89, with ASan and UBSan where the
# compiler has them.
#
#   sh tools/pvr/ta_index_test.sh
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/ta_index_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
SAN=-fsanitize=address,undefined
echo 'int main(void){return 0;}' > "$WORK/san.c"
$CC $SAN -o "$WORK/san" "$WORK/san.c" 2>/dev/null || SAN=
$CC -std=c89 -pedantic -Wall -Wextra -Werror -Wno-long-long -O2 -g $SAN -fno-sanitize-recover=all \
   -Icore/hw/pvr -Icore/libretro-common/include \
   -o "$WORK/ta_index_test" tools/pvr/ta_index_test.c core/hw/pvr/ta_index.c core/hw/pvr/ta_list.c
"$WORK/ta_index_test"
