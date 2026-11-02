#!/bin/sh
# A polygon's blend in the Direct3D 12 renderer, the factors that read
# only the source applied by its pixel shader, against the PowerVR's for
# every pair of instructions (tools/rend/dx12_blend_fold_test.cpp): built
# with mingw-w64 (CXX_WIN, default x86_64-w64-mingw32-g++) and run under
# wine (WINE, default wine64, or wine; "" on Windows). No device.
#
#   sh tools/rend/dx12_blend_fold_test.sh     PASS when it passes
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/dx12_blend_fold_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
CXX_WIN=${CXX_WIN:-x86_64-w64-mingw32-g++}
if [ -z "${WINE+x}" ]; then
   if command -v wine64 > /dev/null; then WINE=wine64; else WINE=wine; fi
fi
LRC=core/libretro-common
$CXX_WIN -std=c++11 -O1 -fno-operator-names -D__LIBRETRO__ -DHAVE_D3D12 -DNDEBUG -Icore -Icore/deps -I$LRC/include \
   -o "$WORK/dx12_blend_fold_test.exe" tools/rend/dx12_blend_fold_test.cpp -static
if [ -n "$WINE" ]; then
   env -u TMPDIR WINEDEBUG=-all "$WINE" "$WORK/dx12_blend_fold_test.exe" | tr -d '\r' | tee "$WORK/log.txt"
else
   "$WORK/dx12_blend_fold_test.exe" | tee "$WORK/log.txt"
fi
grep -q "dx12_blend_fold_test: PASS" "$WORK/log.txt"
