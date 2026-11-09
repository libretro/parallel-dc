#!/bin/sh
# Nothing made on the draw path (AGENTS.md), with Direct3D 11 and 12: the core's
# Windows build under wine, on a frontend that runs the core's frames as
# RetroArch does (tools/hdr/d3d_hdr.c), the test discs (plain, the texture
# atlas, culling, DMA over renders to textures, blends and depth modes a
# game seldom draws with), the per-triangle renderer
# (sorted per triangle and per strip) and the per-pixel one, threaded
# rendering on and off, each from nothing kept by an earlier run (no shader
# file): every shader and every state a frame wanted was made before the
# first, and the renderer says so when it stops - no shader made on the
# draw path ("0 made on the draw path", each shader file's), no depth-
# stencil, blend or sampler state either ("0 depth-stencil, 0 blend and 0
# sampler states"). The polygons whose own shader was not there yet were
# drawn with a general one: that it draws the same is
# DX11_SHADER_TEST_GENERAL's (dx11_shaders.cpp) with tools/rend/d3d_same.sh.
# And with Direct3D 12, the same discs and renderers: every pipeline a
# frame drew with made before the first ("0 of them in a frame"; its
# shaders are the core's bytecode, no compiler wanted).
#
#   sh tools/rend/d3d_draw_path.sh [core.dll]
#
# KEEP=1: the logs are kept, in $TMPDIR/d3d_draw_path.<pid>.
# D3DCOMPILER=path/to/d3dcompiler_47.dll: Microsoft's compiler used, not
# wine's, which does not compile the per-pixel renderer's shaders: without
# it, the per-triangle renderer it falls back to is what is judged.
# Needs mingw-w64, wine, Xvfb, python3 and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.dll}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/d3d_draw_path.$$
mkdir -p "$WORK"
DISPLAY_NO=${D3D_DRAW_PATH_DISPLAY:-:97}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
OVERRIDES="winedbg.exe=d"
if [ -n "${D3DCOMPILER:-}" ]; then
   cp "$D3DCOMPILER" "$WORK/d3dcompiler_47.dll"
   OVERRIDES="$OVERRIDES;d3dcompiler_47=n"
fi
${CC_WIN:-x86_64-w64-mingw32-gcc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c -ld3d11 -ld3d12 -ldxguid
# (each disc in its own directory: their tracks have the same names)
for d in plain atlas cull dma blend; do
   mkdir -p "$WORK/$d"
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi" --$d; fi > /dev/null
done
fails=0

# run <name> <disc> <options...>: 100 frames, from nothing kept, with Direct3D $api
api=11
run() {
   name=$1; disc=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan VKD3D_CONFIG=virtual_heaps \
      WINEDLLOVERRIDES="$OVERRIDES" D3DH_VERBOSE=1 D3DH_UNLOAD=1 D3DH_API=$api D3DH_FRONTEND=sdr D3DH_DIR="Z:$WORK/dir" \
      timeout 600 wine "$WORK/d3d_hdr.exe" "Z:$CORE" "Z:$WORK/$disc/$disc.gdi" "Z:$WORK/$name.raw" 100 "$@" \
      > "$WORK/$name.log" 2>&1 || { echo "  FAIL: the run failed:"; tail -5 "$WORK/$name.log"; return 1; }
}

for d in plain atlas cull dma blend; do
   for m in tri strip pix; do
      case $m in
         tri) sort="reicast_alpha_sorting=per-triangle (normal)" ;;
         strip) sort="reicast_alpha_sorting=per-strip (fast, least accurate)" ;;
         pix) sort="reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      for threaded in enabled disabled; do
         name="$d-$m-$threaded"
         run "$name" $d "$sort" reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
         shaders=$(grep -o "DX11: d3d11[a-z_]*shaders.bin: .*" "$WORK/$name.log" || true)
         states=$(grep -o "DX11: [0-9]* depth-stencil, [0-9]* blend and [0-9]* sampler states made on the draw path" "$WORK/$name.log" || true)
         general=$(echo "$shaders" | sed -n 's/.*, \([0-9]*\) draws with a general shader.*/\1/p' | awk '{ n += $1 } END { print n + 0 }')
         echo "$name: $(echo "$shaders" | grep -o '[0-9]* made on the draw path' | tr '\n' ' ')/ $general draws with a general shader"
         # (wine's own d3dcompiler does not compile the per-pixel renderer's shaders: the per-triangle one draws)
         if [ $m = pix ] && grep -q "per-pixel renderer initialization failed" "$WORK/$name.log"; then
            echo "  the per-pixel renderer not made here: the per-triangle one judged (D3DCOMPILER for it)"
         fi
         if [ -z "$shaders" ] || [ -z "$states" ]; then
            echo "  FAIL: the renderer did not say what it made"
            fails=$((fails + 1))
            continue
         fi
         if echo "$shaders" | grep -v -q ", 0 made on the draw path"; then
            echo "  FAIL: a shader made on the draw path:"
            echo "$shaders" | sed 's/^/    /'
            fails=$((fails + 1))
         fi
         if ! echo "$states" | grep -q "^DX11: 0 depth-stencil, 0 blend and 0 sampler states"; then
            echo "  FAIL: $states"
            fails=$((fails + 1))
         fi
      done
   done
done
api=12
for d in plain atlas cull dma blend; do
   for m in tri strip pix; do
      case $m in
         tri) sort="reicast_alpha_sorting=per-triangle (normal)" ;;
         strip) sort="reicast_alpha_sorting=per-strip (fast, least accurate)" ;;
         pix) sort="reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      for threaded in enabled disabled; do
         name="d3d12-$d-$m-$threaded"
         run "$name" $d "$sort" reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
         made=$(grep -o "DX12: [0-9]* pipelines made, [0-9]* of them in a frame" "$WORK/$name.log" || true)
         echo "$name: ${made#DX12: }"
         if [ -z "$made" ]; then
            echo "  FAIL: the renderer did not say what it made"
            fails=$((fails + 1))
         elif ! echo "$made" | grep -q ", 0 of them in a frame"; then
            echo "  FAIL: a pipeline made on the draw path:"
            grep "made in a frame" "$WORK/$name.log" | head -5 | sed 's/^/    /'
            fails=$((fails + 1))
         fi
      done
   done
done
[ $fails -eq 0 ] || { echo "d3d_draw_path: $fails failed"; exit 1; }
echo "d3d_draw_path: all passed"
