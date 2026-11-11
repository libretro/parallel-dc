#!/bin/sh
# core/rend/dx11/dx11_dxbc.inc: the bytecode of every shader the Direct3D 11
# renderers' start makes (the vertex shaders, the shaders of a kind of their
# own, the general pixel shaders, the write-out pass), for the renderers to
# start with no compiler and draw every polygon with the general shaders
# without one (dx11_shaders.h). Made from the core's own run: the core,
# built for Windows from this tree, run under wine with Microsoft's
# d3dcompiler_47.dll on the per-triangle and the per-pixel renderer, from
# nothing kept; the shaders of those families in the files it leaves, by
# the hash of what each was compiled from (tools/rend/dx11_dxbc_inc.c).
#
#   D3DCOMPILER=path/to/d3dcompiler_47.dll sh tools/rend/kernels_dx11.sh [core.dll]
#
# Whether the file is still the sources' is tools/rend/d3d_draw_path.sh's:
# a Direct3D 11 run that compiles anything at the start fails it.
# Needs mingw-w64, wine, Xvfb, python3 and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
[ -n "${D3DCOMPILER:-}" ] && [ -f "$D3DCOMPILER" ] \
   || { echo "$0: D3DCOMPILER=/path/to/d3dcompiler_47.dll is needed" >&2; exit 1; }
CORE=$(readlink -f "${1:-parallel_dc_libretro.dll}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/kernels_dx11.$$
mkdir -p "$WORK"
DISPLAY_NO=${KERNELS_DX11_DISPLAY:-:96}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
cp "$D3DCOMPILER" "$WORK/d3dcompiler_47.dll"
${CC_WIN:-x86_64-w64-mingw32-gcc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c tools/rend/alloc_count.c -ld3d11 -ld3d12 -ldxguid
${CC:-cc} -O1 -Wall -Icore/rend -Icore/libretro-common/include -o "$WORK/dxbc_inc" \
   tools/rend/dx11_dxbc_inc.c core/rend/d3d_shader_cache.c
python3 tools/threads/live_disc.py "$WORK/plain.gdi" > /dev/null

for m in tri pix; do
   case $m in
      tri) sort="reicast_alpha_sorting=per-triangle (normal)" ;;
      pix) sort="reicast_alpha_sorting=per-pixel (accurate)" ;;
   esac
   mkdir -p "$WORK/$m"
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan \
      WINEDLLOVERRIDES="winedbg.exe=d;d3dcompiler_47=n" D3DH_VERBOSE=1 D3DH_UNLOAD=1 D3DH_API=11 \
      D3DH_FRONTEND=sdr D3DH_DIR="Z:$WORK/$m" \
      timeout 600 wine "$WORK/d3d_hdr.exe" "Z:$CORE" "Z:$WORK/plain.gdi" "Z:$WORK/$m.raw" 5 "$sort" \
      > "$WORK/$m.log" 2>&1 || { tail -5 "$WORK/$m.log"; echo "$0: the $m run failed" >&2; exit 1; }
   grep -o "DX11: d3d11[a-z_]*shaders.bin: .*" "$WORK/$m.log" || true
   if [ $m = pix ] && grep -q "per-pixel renderer initialization failed" "$WORK/$m.log"; then
      echo "$0: the per-pixel renderer did not start" >&2
      exit 1
   fi
done
# (the families but a polygon's own, PS: dx11_shaders.h's and dx11_oitshaders.h's)
files=$(find "$WORK/tri" "$WORK/pix" -name 'd3d11*shaders.bin' | sed 's/$/:0,2,3,4,5,6,7/')
[ $(echo "$files" | grep -c oit) -ge 1 ] && [ $(echo "$files" | grep -vc oit) -ge 1 ] \
   || { echo "$0: the runs left no shader files" >&2; exit 1; }
"$WORK/dxbc_inc" core/rend/dx11/dx11_dxbc.inc $files
# (which compiler made it, as the other files say)
SUM=$(sha1sum < "$D3DCOMPILER" | cut -c1-40)
sed -i "s|^ \\* edited\\. \\*/\$| * edited.\\n * d3dcompiler_47 $SUM\\n */|" core/rend/dx11/dx11_dxbc.inc
