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
# ARCH   make's ARCH (default riscv64): with another, such as arm, the core
#        for that processor, with its recompilers
# CROSS_FLAGS  what the compilers are to be given besides (for 32-bit ARM,
#        -marm -mfpu=neon-vfpv4, as the core's ARM builds have)
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
ARCH=${ARCH:-riscv64}
mkdir -p "$WORK/gl"
cleanup() { git worktree remove --force "$WORK/src" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT
# (the compilers, with the flags they are to have, under their own names)
if [ -n "$CROSS_FLAGS" ]; then
   mkdir -p "$WORK/bin"
   for TOOL in gcc g++; do
      printf '#!/bin/sh\nexec %s %s "$@"\n' "$(command -v "$CROSS-$TOOL")" "$CROSS_FLAGS" > "$WORK/bin/$CROSS-$TOOL"
      chmod +x "$WORK/bin/$CROSS-$TOOL"
   done
   PATH=$WORK/bin:$PATH
   export PATH
fi

# OpenGL's headers are the host's (the cross compiler looks in /usr/include
# after its own: libgl1-mesa-dev), the same for every processor
[ -f /usr/include/GL/gl.h ] && [ -f /usr/include/KHR/khrplatform.h ] \
   || { echo "generic_cpu: OpenGL's headers are not installed (libgl1-mesa-dev)" >&2; exit 1; }
# (built away from the tree's own build)
git worktree add --detach "$WORK/src" >/dev/null
MAKEARGS="-C $WORK/src -j$(nproc) platform=unix ARCH=$ARCH CC=$CROSS-gcc"
# An OpenGL that does nothing to link with, of the functions the core calls
make $MAKEARGS CXX="$CROSS-g++ -L$WORK/gl" -k >/dev/null 2>&1 || true
find "$WORK/src/core" -name '*.o' -exec "$CROSS-nm" --undefined-only {} + 2>/dev/null \
   | awk '{print $NF}' | grep '^gl' | sort -u | python3 tools/threads/headless_gl.py > "$WORK/gl/nogl.c"
"$CROSS-gcc" -O1 -shared -fPIC -o "$WORK/gl/libGL.so" "$WORK/gl/nogl.c"
make $MAKEARGS CXX="$CROSS-g++ -L$WORK/gl"

LD_LIBRARY_PATH=$WORK/gl CC=$CROSS-gcc NM=$CROSS-nm RUN="$QEMU -L $SYSROOT" WORK=$WORK/run \
   sh tools/threads/headless.sh "$WORK/src/parallel_dc_libretro.so"
