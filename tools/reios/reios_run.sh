#!/bin/sh
# The HLE BIOS (core/reios/reios.c), by itself.
#
#   sh tools/reios/reios_run.sh [extra C flags]
#   WINDOWS=1 sh tools/reios/reios_run.sh
#
# reios_test.c is built with reios.c, the flash's code (flashrom.c), the
# descrambler (descrambl.c) and strlcpy, with the C flags the core is built
# with, and stands in for the rest of the machine; what it prints has to
# be reios_expected.txt. Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
# WINDOWS=1 builds it for Windows with mingw-w64 and runs it under wine.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/reios_test.$$
mkdir -p "$WORK/game"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$WINDOWS" ]; then
   MAKEARGS="platform=win CC=x86_64-w64-mingw32-gcc"
   LIBS="-static"
   EXE="$WORK/reios_test.exe"
   GAME=$(winepath -w "$WORK/game" 2>/dev/null)
   RUN="wine"
else
   MAKEARGS=""
   LIBS=""
   EXE="$WORK/reios_test"
   GAME="$WORK/game"
   RUN=""
fi

CCMD=$(make -n -B $MAKEARGS core/reios/reios.o 2>/dev/null | grep 'reios\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T=$(echo "$CCMD" | sed 's| [^ ]*reios\.c||; s/ -MMD -MP//')
CC=${CFLAGS_T%% *}

for SRC in core/reios/reios.c core/hw/flashrom/flashrom.c core/reios/descrambl.c \
		core/libretro-common/compat/compat_strl.c tools/reios/reios_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$EXE" "$WORK"/*.o $LIBS
$RUN "$EXE" "$GAME" 2>/dev/null | tr -d '\r' > "$WORK/out.txt"
if ! cmp -s "$WORK/out.txt" tools/reios/reios_expected.txt; then
   diff tools/reios/reios_expected.txt "$WORK/out.txt" | head -40
   echo "reios: FAILED"
   exit 1
fi
echo "reios: all passed"
