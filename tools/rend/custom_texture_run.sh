#!/bin/sh
# Custom textures (core/rend/custom_texture.c) by themselves, the loader
# thread and all, on a file system of the test's own.
#
#   sh tools/rend/custom_texture_run.sh [extra C flags]
#
# custom_texture_test.c is built with custom_texture.c, with the C flags
# the core is built with. Extra flags, such as -fsanitize=thread or
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/custom_texture_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/rend/custom_texture.o 2>/dev/null | grep 'custom_texture\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*custom_texture\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}
L=core/libretro-common

OBJS=
for SRC in core/rend/custom_texture.c tools/rend/custom_texture_test.c \
      $L/rthreads/rthreads.c $L/rthreads/retro_eventcount.c \
      $L/compat/compat_strl.c $L/string/stdstring.c $L/encodings/encoding_utf.c; do
   O="$WORK/$(basename "$SRC" .c).o"
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$O\""
   OBJS="$OBJS $O"
done
"$CC" $EXTRA -o "$WORK/custom_texture_test" $OBJS -lpthread
"$WORK/custom_texture_test"
