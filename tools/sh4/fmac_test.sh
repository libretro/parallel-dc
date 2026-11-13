#!/bin/sh
# FMAC's sum, a * b + c rounded once (core/hw/sh4/sh4_fmac.h), against the
# C library's fmaf(): see fmac_test.c. With the flags the core's floating
# point is built with (no fused multiply-add made by the compiler, the SSE
# unit on 32-bit x86); for this host, and for ARM64 under qemu where the
# cross compiler is there (fmaf() is the processor's own instruction there).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
WORK=${TMPDIR:-/tmp}/fmac_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=c99 -O2 -ffp-contract=off -fno-fast-math -frounding-math -Icore -Icore/libretro-common/include"
${CC:-cc} $FLAGS -o "$WORK/fmac_test" tools/sh4/fmac_test.c -lm
"$WORK/fmac_test"
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && command -v qemu-aarch64-static >/dev/null 2>&1; then
   aarch64-linux-gnu-gcc $FLAGS -static -o "$WORK/fmac_test_a64" tools/sh4/fmac_test.c -lm
   qemu-aarch64-static "$WORK/fmac_test_a64"
fi
