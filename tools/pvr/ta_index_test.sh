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
# (and with the test of many vertices at once a vertex at a time, as a
# machine with neither SSE2 nor NEON has it)
for v in "" -DTA_BAD_SCALAR; do
   $CC -std=c89 -pedantic -Wall -Wextra -Werror -Wno-long-long -O2 -g $SAN -fno-sanitize-recover=all $v \
      -Icore/hw/pvr -Icore/libretro-common/include \
      -o "$WORK/ta_index_test" tools/pvr/ta_index_test.c core/hw/pvr/ta_index.c core/hw/pvr/ta_list.c
   "$WORK/ta_index_test"
done
# NEON's, where there is an AArch64 compiler with its C library (Debian's
# gcc-aarch64-linux-gnu and libc6-dev-arm64-cross) and a way to run what it
# makes (qemu-user). NEON=required: its absence is a failure, not a skip.
A64_CC=${A64_CC:-aarch64-linux-gnu-gcc}
QEMU=$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)
printf '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\nint main(void){return 0;}\n' > "$WORK/a64.c"
if [ -n "$QEMU" ] && $A64_CC -static -o "$WORK/a64" "$WORK/a64.c" 2>/dev/null && "$QEMU" "$WORK/a64"; then
   $A64_CC -std=c89 -pedantic -Wall -Wextra -Werror -Wno-long-long -O2 -static \
      -Icore/hw/pvr -Icore/libretro-common/include \
      -o "$WORK/ta_index_test_a64" tools/pvr/ta_index_test.c core/hw/pvr/ta_index.c core/hw/pvr/ta_list.c
   "$QEMU" "$WORK/ta_index_test_a64"
elif [ "${NEON:-}" = required ]; then
   echo "ta_index_test: FAIL: no AArch64 compiler with its C library, or no qemu-user, for NEON's"
   exit 1
else
   echo "ta_index_test: NEON's not tested (no AArch64 compiler with its C library, or no qemu-user)"
fi
