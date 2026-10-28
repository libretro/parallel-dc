#!/bin/sh
# The Direct3D 12 renderer's pipelines made ahead, on a device
# (tools/rend/dx12_pipelines_ahead_test.cpp): built with mingw-w64
# (CXX_WIN, default x86_64-w64-mingw32-g++) from the renderer's own
# rend/dx12/dx12_pso.cpp and its shaders, run under wine (WINE, default
# wine64, or /usr/lib/wine/wine64 where only that is there; "" on Windows)
# and xvfb-run where this is not Windows, with vkd3d-proton's d3d12.dll and
# d3d12core.dll from $D3D12_DLLS where that is given (wine's own vkd3d
# cannot make the renderer's root signature on every Vulkan driver: it
# wants more descriptor sets than lavapipe's 8).
#
#   sh tools/rend/dx12_pipelines_ahead_test.sh     PASS when it passes
#
# Made in $WORK (default: a new directory in $TMPDIR).
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/dx12_pipelines_ahead_test.$$
   trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK/data"
CXX_WIN=${CXX_WIN:-x86_64-w64-mingw32-g++}
CC_WIN=${CC_WIN:-$(echo "$CXX_WIN" | sed 's/g++$/gcc/')}
if [ -z "${WINE+x}" ]; then
   if command -v wine64 > /dev/null; then WINE=wine64
   elif [ -x /usr/lib/wine/wine64 ]; then WINE=/usr/lib/wine/wine64
   else WINE=wine
   fi
fi
if [ -n "$D3D12_DLLS" ]; then
   cp "$D3D12_DLLS/d3d12.dll" "$D3D12_DLLS/d3d12core.dll" "$WORK/"
   export WINEDLLOVERRIDES="d3d12,d3d12core=n"
fi
LRC=core/libretro-common
INC="-Icore -Icore/deps -I$LRC/include -Icore/libretro"
DEFS="-D__LIBRETRO__ -DHAVE_THREADS -DHAVE_D3D12 -DNDEBUG"
for f in core/rend/d3d_warm.c $LRC/queues/retro_spsc.c $LRC/rthreads/rthreads.c; do
   $CC_WIN -O1 $DEFS $INC -c -o "$WORK/$(basename "$f" .c).o" "$f"
done
$CXX_WIN -std=c++11 -O1 -fpermissive -fno-operator-names $DEFS $INC -o "$WORK/dx12_pipelines_ahead_test.exe" \
   tools/rend/dx12_pipelines_ahead_test.cpp core/rend/dx12/dx12_pso.cpp core/rend/dx12/dx12_shaders.cpp \
   "$WORK"/*.o -ld3d12 -ldxguid -static
ARGS="Z:$(echo "$WORK/data/" | tr / '\\')"
cd "$WORK"
if [ -n "$WINE" ]; then
   # (wine wants the TMPDIR it was set up with)
   # (no debugger to wait for, should it crash)
   env -u TMPDIR WINEDEBUG=-all WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}winedbg.exe=d" xvfb-run -a "$WINE" ./dx12_pipelines_ahead_test.exe $ARGS | tr -d '\r' | tee log.txt
else
   ./dx12_pipelines_ahead_test.exe $ARGS | tee log.txt
fi
grep -q "dx12_pipelines_ahead_test: PASS" log.txt
