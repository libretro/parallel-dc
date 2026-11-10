#!/bin/sh
# No allocation during play (AGENTS.md), with Direct3D 11 and 12: the
# core's Windows build under wine, on a frontend that runs the core's frames
# as RetroArch does (tools/hdr/d3d_hdr.c, D3DH_ALLOCS: the core's heap - what
# it imports for it pointed at a counter - and the Direct3D objects it makes,
# from frame 20 of 100 on: tools/rend/alloc_count.c). The test discs (plain,
# the texture atlas, culling, DMA over renders to textures, blends and depth
# modes a game seldom draws with), the per-triangle renderer (per triangle,
# threaded rendering on and off, and per strip) and the per-pixel one, and
# with run-ahead (D3DH_AHEAD: a state saved and loaded every frame) the
# plain disc and the one with a render to a texture in its state;
# every site named (tools/vulkan/vk_alloc_sites.py, by the build's symbol
# table) has to be on tools/rend/d3d_allocs.allowed, and none may be in the
# fault handler.
#
#   sh tools/rend/d3d_allocs.sh [core.dll]
#
# KEEP=1: the logs are kept, in $TMPDIR/d3d_allocs.<pid>.
# D3DCOMPILER=path/to/d3dcompiler_47.dll: Microsoft's compiler used, not
# wine's, which does not compile the per-pixel renderer's shaders (as
# tools/rend/d3d_draw_path.sh).
# Needs mingw-w64, wine, Xvfb, python3 and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.dll}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/d3d_allocs.$$
mkdir -p "$WORK"
DISPLAY_NO=${D3D_ALLOCS_DISPLAY:-:93}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
OVERRIDES="winedbg.exe=d"
if [ -n "${D3DCOMPILER:-}" ]; then
   cp "$D3DCOMPILER" "$WORK/d3dcompiler_47.dll"
   OVERRIDES="$OVERRIDES;d3dcompiler_47=n"
fi
${CC_WIN:-x86_64-w64-mingw32-gcc} -O1 -Wall -DALLOC_COUNT -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c tools/rend/alloc_count.c -ld3d11 -ld3d12 -ldxguid
# (each disc in its own directory: their tracks have the same names)
for d in plain atlas cull dma blend keep; do
   mkdir -p "$WORK/$d"
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi" --$d; fi > /dev/null
done
fails=0

# run <name> <disc> <options...>: 100 frames, from nothing kept, with Direct3D $api
run() {
   name=$1; disc=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   if ! env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan VKD3D_CONFIG=virtual_heaps \
         WINEDLLOVERRIDES="$OVERRIDES" D3DH_ALLOCS=20 D3DH_ALLOC_SITES=1 D3DH_API=$api D3DH_FRONTEND=sdr \
         D3DH_DIR="Z:$WORK/dir" timeout 600 wine "$WORK/d3d_hdr.exe" "Z:$CORE" "Z:$WORK/$disc/$disc.gdi" \
         "Z:$WORK/$name.raw" 100 "$@" > "$WORK/$name.log" 2>&1; then
      echo "  FAIL: $name: the run failed:"; tail -5 "$WORK/$name.log"
      fails=$((fails + 1))
      return 0
   fi
   echo "$name:$(sed -n 's/.*during play (frames 20 to 99)://p' "$WORK/$name.log")"
}

TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
for api in 11 12; do
   for d in plain atlas cull dma blend; do
      run d3d$api-$d-tri $d "$TRI"
      run d3d$api-$d-tri-nothread $d "$TRI" reicast_threaded_rendering=disabled
      run d3d$api-$d-strip $d "$STRIP"
      run d3d$api-$d-pix $d "$PIX"
   done
   # with run-ahead (a state saved and loaded every frame): the plain disc,
   # and the one whose state has a render to a texture on the card in it
   for d in plain keep; do
      D3DH_AHEAD=10 run d3d$api-$d-tri-ahead $d "$TRI"
      D3DH_AHEAD=10 run d3d$api-$d-pix-ahead $d "$PIX"
   done
done
python3 tools/vulkan/vk_alloc_sites.py "$CORE" tools/rend/d3d_allocs.allowed "$WORK"/*.log || fails=$((fails + 1))
[ $fails -eq 0 ] || { echo "d3d_allocs: $fails failed"; exit 1; }
echo "d3d_allocs: no allocation site that is not on the list"
