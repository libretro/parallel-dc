#!/bin/sh
# A processor the core has no recompiler for, RISC-V 64 (a generic one to
# the core: HOST_CPU_GENERIC): built for it with the cross compiler, as
# `make ARCH=riscv64` has it - the interpreter, the C library's rounding,
# every symbol there at the link - and the disc run on it under qemu
# (headless.sh).
#
#   sh tools/threads/generic_cpu.sh
#
# CROSS  the cross compilers' prefix (default riscv64-linux-gnu)
# QEMU   what runs its programs (default qemu-riscv64-static)
#
# (The host's own programs the test runs say they cannot preload its
# libGLESv2.so.2, which is RISC-V's: they do not need it.)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CROSS=${CROSS:-riscv64-linux-gnu}
QEMU=${QEMU:-qemu-riscv64-static}
SYSROOT=/usr/$CROSS
WORK=${TMPDIR:-/tmp}/generic_cpu.$$
mkdir -p "$WORK/gl"
cleanup() { git worktree remove --force "$WORK/src" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

# (built away from the tree's own build)
git worktree add --detach "$WORK/src" >/dev/null
MAKEARGS="-C $WORK/src -j$(nproc) platform=unix ARCH=riscv64 CC=$CROSS-gcc"
# An OpenGL that does nothing to link with, of the functions the core calls
make $MAKEARGS CXX="$CROSS-g++ -L$WORK/gl" -k >/dev/null 2>&1 || true
find "$WORK/src/core" -name '*.o' -exec "$CROSS-nm" --undefined-only {} + 2>/dev/null \
   | awk '{print $NF}' | grep '^gl' | sort -u | python3 tools/threads/headless_gl.py > "$WORK/gl/nogl.c"
"$CROSS-gcc" -O1 -shared -fPIC -o "$WORK/gl/libGL.so" "$WORK/gl/nogl.c"
make $MAKEARGS CXX="$CROSS-g++ -L$WORK/gl"

LD_LIBRARY_PATH=$WORK/gl CC=$CROSS-gcc NM=$CROSS-nm RUN="$QEMU -L $SYSROOT" WORK=$WORK/run \
   sh tools/threads/headless.sh "$WORK/src/parallel_dc_libretro.so"
