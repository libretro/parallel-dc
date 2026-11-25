#!/bin/sh
# The texture converters (core/rend/texconv.c) on another processor, under
# qemu: NEON's (AArch64) and the plain C's (RISC-V 64) against the same
# results as texconv_test.sh's (texconv_expected.txt).
#
#   sh tools/rend/texconv_cross.sh aarch64-linux-gnu qemu-aarch64-static
#   sh tools/rend/texconv_cross.sh riscv64-linux-gnu qemu-riscv64-static
set -e
cd "$(dirname "$0")/../.."
P=$1
RUN=$2
case $P in
aarch64*) DEFS="-DTARGET_LINUX_ARMv8 -DHOST_CPU=0x20000006" ;;
riscv64*) DEFS="-DTARGET_LINUX_RISCV64 -DHOST_CPU=0x20000009" ;;
*) echo "texconv_cross: which processor is $P?" >&2; exit 1 ;;
esac
WORK=${TMPDIR:-/tmp}/texconv_cross.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
I="-I./core/libretro -I./core -I./core/deps -I./core/libretro-common/include -I./core/deps/khronos $DEFS -D__LIBRETRO__ -DNDEBUG -DPVR_REGS_FOR_RENDERER -O2"
"$P-gcc" $I -c core/rend/texconv.c -o "$WORK/texconv.o"
# (only the converters: what else the test can do, it is not asked to)
"$P-gcc" $I -w -static -o "$WORK/texconv_test" tools/rend/texconv_test.c "$WORK/texconv.o" \
   -Wl,--unresolved-symbols=ignore-all
(echo "# random"; "$RUN" "$WORK/texconv_test"; echo "# ramp"; "$RUN" "$WORK/texconv_test" ramp) > "$WORK/out.txt"
if cmp -s "$WORK/out.txt" tools/rend/texconv_expected.txt; then
   echo "texture converters ($P): $(grep -vc '^#' "$WORK/out.txt") results, all as upstream's"
else
   echo "FAIL: texture converters ($P) differ from upstream's:" >&2
   diff "$WORK/out.txt" tools/rend/texconv_expected.txt | head -20 >&2
   exit 1
fi
