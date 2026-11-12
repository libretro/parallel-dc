#!/bin/sh
# The sound chip's channels against what they made before.
#
#   sh tools/aica/sgc_run.sh [extra C flags]
#
# sgc_test.c is built with the channels (sgc_if.c), the rest of the sound
# chip and its DSP's interpreter, and nothing else of the core, with the C
# flags the core is built with. It runs 200 programs of random register
# writes over random sound memory, and the CRC of each one's output has to
# be the one in sgc_expected.txt. Those were made with the C++ channels
# sgc_if.c replaced, from the same programs: the C ones have to give the
# same sound, sample for sample.
#
# Should the sound change on purpose, the new figures go in sgc_expected.txt
# with it, from this test's own output (the lines after FAIL).
#
# Extra flags, such as -fsanitize=address,undefined, go to the compiler and
# the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/aica_sgc.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# the compiler and flags the core compiles its C with
CCMD=$(make -n -B core/hw/aica/sgc_if.o 2>/dev/null | grep 'sgc_if\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_SGC=$(echo "$CCMD" | sed 's| [^ ]*sgc_if\.c||; s/ -MMD -MP//')
CC=${CFLAGS_SGC%% *}

for SRC in core/hw/aica/sgc_if.c core/hw/aica/aica.c core/hw/aica/aica_mem.c \
           core/hw/aica/dsp.c core/hw/aica/dsp_interp.c \
           core/libretro-common/encodings/encoding_crc32.c tools/aica/sgc_test.c; do
   eval "$CFLAGS_SGC -DFEAT_DSPREC=DYNAREC_NONE $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/sgc_test" "$WORK"/*.o -lm

"$WORK/sgc_test" > "$WORK/out.txt"
if ! cmp -s "$WORK/out.txt" tools/aica/sgc_expected.txt; then
   echo "FAIL: the channels' sound differs from sgc_expected.txt in these programs:" >&2
   diff tools/aica/sgc_expected.txt "$WORK/out.txt" | grep '^>' | head -n 10 >&2
   exit 1
fi
echo "sgc: ok ($(wc -l < tools/aica/sgc_expected.txt) programs, the same sound as before)"
