#!/bin/sh
# core/rend/dx9/dx9_dxbc.inc: the Direct3D 9 renderer's shaders as
# bytecode, so that it starts with no shader compiler (dx9_shaders.cpp
# looks there first, by the hash of what each is compiled from):
# dx9_shaders.cpp's sources compiled with Microsoft's d3dcompiler_47.dll
# under wine (tools/rend/dx9_dxbc_make.cpp).
#
#   D3DCOMPILER=path/to/d3dcompiler_47.dll sh tools/rend/kernels_dx9.sh
#
# Whether the file is still the sources' is tools/rend/d3d_same.sh's: a
# Direct3D 9 run that compiles a shader fails it.
set -e
cd "$(dirname "$0")/../.."
[ -n "${D3DCOMPILER:-}" ] && [ -f "$D3DCOMPILER" ] \
   || { echo "$0: D3DCOMPILER=/path/to/d3dcompiler_47.dll is needed" >&2; exit 1; }
WORK=$(readlink -f "${TMPDIR:-/tmp}")/kernels_dx9.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CXX_WIN:-x86_64-w64-mingw32-g++} -O1 -std=c++17 -D__LIBRETRO__ -Icore -Icore/deps -Icore/libretro-common/include \
   -static -o "$WORK/make.exe" tools/rend/dx9_dxbc_make.cpp core/rend/d3d_shader_cache.c
cp "$D3DCOMPILER" "$WORK/d3dcompiler_47.dll"
env -u TMPDIR WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" \
   wine "$WORK/make.exe" "Z:$WORK/d3dcompiler_47.dll" "Z:$WORK/dx9_dxbc.inc"
cp "$WORK/dx9_dxbc.inc" core/rend/dx9/dx9_dxbc.inc
# (which compiler made it, as the other files say)
SUM=$(sha1sum < "$D3DCOMPILER" | cut -c1-40)
sed -i "s|^ \\* edited\\. \\*/\$| * edited.\\n * d3dcompiler_47 $SUM\\n */|" core/rend/dx9/dx9_dxbc.inc
