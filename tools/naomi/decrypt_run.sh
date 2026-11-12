#!/bin/sh
# The 315-5881 decryption chip (decrypt.c) by itself.
#
#   sh tools/naomi/decrypt_run.sh [extra C flags]
#
# decrypt_test.c is built with decrypt.c, with the C flags the core is
# built with, and decrypts made-up cartridges with a set of keys; the hash
# of each stream must be the one in decrypt_expected.txt. Should the
# decryption change on purpose, the new lines go in decrypt_expected.txt
# with it, from this test's own output.
#
# Extra flags, such as -fsanitize=address,undefined, go to the compiler and
# the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/naomi_decrypt.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/naomi/decrypt.o 2>/dev/null | grep 'decrypt\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_D=$(echo "$CCMD" | sed 's| [^ ]*decrypt\.c||; s/ -MMD -MP//')
CC=${CFLAGS_D%% *}

for SRC in core/hw/naomi/decrypt.c tools/naomi/decrypt_test.c; do
   eval "$CFLAGS_D $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/decrypt_test" "$WORK"/*.o

"$WORK/decrypt_test" > "$WORK/out.txt"
if ! cmp -s "$WORK/out.txt" tools/naomi/decrypt_expected.txt; then
   echo "FAIL: the streams differ from decrypt_expected.txt for these keys:" >&2
   diff tools/naomi/decrypt_expected.txt "$WORK/out.txt" | grep '^>' | head -n 10 >&2
   exit 1
fi
echo "decrypt: ok ($(wc -l < tools/naomi/decrypt_expected.txt) streams, as before)"
