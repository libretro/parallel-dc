#!/bin/sh
# The content's discs (core/libretro/lr_disk.c): playlists, the frontend's
# disc control and the disc's words in a state, driven at random.
#
#   sh tools/libretro/lr_disk_run.sh [extra C flags]
#
# lr_disk_test.c is built with lr_disk.c and libretro-common's paths, with
# the C flags the core is built with; what it prints must be what it always
# was (lr_disk_expected.txt). Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/lr_disk_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/libretro/lr_disk.o 2>/dev/null | grep 'lr_disk\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*lr_disk\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/libretro/lr_disk.c core/libretro-common/file/file_path.c \
      core/libretro-common/compat/compat_strl.c core/libretro-common/time/rtime.c \
      core/libretro-common/encodings/encoding_utf.c \
      tools/libretro/lr_disk_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/lr_disk_test" "$WORK"/*.o -lm
"$WORK/lr_disk_test" > "$WORK/out.txt" || { cat "$WORK/out.txt"; exit 1; }
cmp -s "$WORK/out.txt" tools/libretro/lr_disk_expected.txt \
   || { diff "$WORK/out.txt" tools/libretro/lr_disk_expected.txt | head -n 8; echo "lr_disk_test: not as it was (tools/libretro/lr_disk_expected.txt)"; exit 1; }
echo "lr_disk_test: as before"
