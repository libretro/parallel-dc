#!/bin/sh
# The Direct3D 11 renderers' shaders made ahead and kept on disk, on a
# device (tools/rend/dx11_shaders_ahead_test.cpp): built with mingw-w64
# (CXX_WIN, default x86_64-w64-mingw32-g++) from the renderers' own
# rend/dx11/dx11_shaders.cpp and rend/dx11/oit/dx11_oitshaders.cpp, run
# under wine (WINE, default wine64, or /usr/lib/wine/wine64 where only that
# is there; "" on Windows) and xvfb-run where this is not Windows.
#
#   sh tools/rend/dx11_shaders_ahead_test.sh     PASS when it passes
#
# D3DCOMPILER=/path/to/a/compiler.dll takes another compiler than the
# system's d3dcompiler_47.dll. A renderer whose shaders the compiler does
# not compile is not tested, and said so: wine's own compiles the
# per-triangle renderer's and not the per-pixel one's. Made in $WORK
# (default: a new directory in $TMPDIR).
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/dx11_shaders_ahead_test.$$
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
LRC=core/libretro-common
INC="-Icore -Icore/deps -I$LRC/include -Icore/libretro"
DEFS="-D__LIBRETRO__ -DHAVE_THREADS -DHAVE_D3D11 -DNDEBUG"
for f in core/rend/d3d_warm.c core/rend/d3d_shader_cache.c $LRC/queues/retro_spsc.c $LRC/rthreads/rthreads.c; do
   $CC_WIN -O1 $DEFS $INC -c -o "$WORK/$(basename "$f" .c).o" "$f"
done
$CXX_WIN -std=c++11 -O1 -fpermissive -fno-operator-names $DEFS $INC -o "$WORK/dx11_shaders_ahead_test.exe" \
   tools/rend/dx11_shaders_ahead_test.cpp core/rend/dx11/dx11_shaders.cpp core/rend/dx11/oit/dx11_oitshaders.cpp \
   "$WORK"/*.o -ld3d11 -ldxguid -static
ARGS="Z:$(echo "$WORK/data/" | tr / '\\')"
if [ -n "$D3DCOMPILER" ]; then
   cp "$D3DCOMPILER" "$WORK/compiler.dll"
   ARGS="$ARGS compiler.dll"
fi
cd "$WORK"
if [ -n "$WINE" ]; then
   # (wine wants the TMPDIR it was set up with)
   # (no debugger to wait for, should it crash)
   env -u TMPDIR WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" xvfb-run -a "$WINE" ./dx11_shaders_ahead_test.exe $ARGS | tr -d '\r' | tee log.txt
else
   ./dx11_shaders_ahead_test.exe $ARGS | tee log.txt
fi
grep -q "dx11_shaders_ahead_test: PASS" log.txt
