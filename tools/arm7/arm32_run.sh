#!/bin/sh
# The sound chip's ARM7 recompiler for 32-bit ARM hosts
# (core/hw/arm7/arm7_rec_arm32.c).
#
#   sh tools/arm7/arm32_run.sh [blocks] [extra C flags]
#
# arm32_test.c, built for 32-bit ARM with arm-linux-gnueabihf-gcc (or gcc
# on such a host) and the C flags the core is built with there, as ARM code
# with NEON as the core's ARM builds are, and run (under qemu-arm on another
# host): it hashes the code made of random programs, which must be what it
# made when last changed on purpose (arm32_expected.txt holds 20000 and
# 5000 blocks; tools/arm7/run.sh runs the code). Only does anything on Linux.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
N=${1:-20000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/arm32_test.$$
mkdir -p "$WORK"
[ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT

case $(uname -s)-$(uname -m) in
   Linux-armv7*) A32CC=gcc; RUN="" ;;
   Linux-*)
      command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1 || { echo "arm32_test: skipped (no arm-linux-gnueabihf-gcc)"; exit 0; }
      if command -v qemu-arm-static >/dev/null 2>&1; then RUN=qemu-arm-static
      elif command -v qemu-arm >/dev/null 2>&1; then RUN=qemu-arm
      else echo "arm32_test: skipped (no qemu-arm)"; exit 0; fi
      A32CC=arm-linux-gnueabihf-gcc ;;
   *) echo "arm32_test: skipped (not Linux)"; exit 0 ;;
esac

CCMD=$(make -n -B platform=unix ARCH=arm CC=$A32CC core/hw/arm7/arm7.o 2>/dev/null | grep 'arm7\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_T="$(echo "$CCMD" | sed 's| [^ ]*arm7\.c||; s/ -MMD -MP//') -marm -mfpu=neon-vfpv4"

REC=core/hw/arm7/arm7_rec_arm32.c
LIBS=
# (REC_OBJ: another build of the recompiler, such as the C++ one)
if [ -n "$REC_OBJ" ]; then REC=; LIBS=-lstdc++; fi
for SRC in core/hw/arm7/arm7.c core/hw/arm7/arm_mem.c $REC tools/arm7/arm32_test.c; do
   eval "$CFLAGS_T $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
[ -z "$REC_OBJ" ] || cp $REC_OBJ "$WORK/"
$A32CC -marm $EXTRA -static -o "$WORK/arm32_test" "$WORK"/*.o $LIBS -lm
$RUN "$WORK/arm32_test" "$N" 2>"$WORK/err.txt" > "$WORK/out.txt" || true
cat "$WORK/out.txt"
cat "$WORK/err.txt" >&2
# (one line, all of it the one expected: grep takes each line for a pattern)
[ "$(wc -l < "$WORK/out.txt")" -eq 1 ] && grep -qxF "$(cat "$WORK/out.txt")" tools/arm7/arm32_expected.txt \
   || { echo "arm32_test: the recompiler does not make what it made (tools/arm7/arm32_expected.txt)"; exit 1; }
echo "arm32_test: as before"
