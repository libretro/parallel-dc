#!/bin/sh
# Video memory written by DMA over what was rendered to a texture there and
# is still on the graphics card, and over a texture the game draws with (the
# test disc built with -DLIVE_DMA: tools/threads/live_disc.py --dma), with
# Direct3D 11 and 12: the core's Windows build under wine (wined3d and vkd3d
# on a Vulkan driver: lavapipe will do), by the frontend that reads back
# what it is handed (tools/hdr/d3d_hdr.c), as tools/vulkan/vk_dma.sh has it
# with Vulkan. The top right of the screen has to show cyan over yellow,
# cyan, and green (tools/rend/dma_check.py) - per-triangle and per-pixel,
# the renders to textures at the console's size and finer, threaded
# rendering on and off - and, given a second core, the same picture as that
# core's in each.
#
#   sh tools/rend/d3d_dma.sh parallel_dc_libretro.dll [other.dll]
#
# Needs mingw-w64, wine, Xvfb, python3 and a Vulkan driver. Direct3D 12 is
# vkd3d-proton's d3d12.dll and d3d12core.dll from $D3D12_DLLS, as
# tools/rend/tile_test.sh has it; without them it is skipped: wine's own
# vkd3d on lavapipe draws no textured polygon at all.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.dll}")
OTHER=${2:+$(readlink -f "$2")}
WORK=$(readlink -f "${TMPDIR:-/tmp}")/d3d_dma.$$
mkdir -p "$WORK"
DISPLAY_NO=${D3D_DMA_DISPLAY:-:93}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC_WIN:-x86_64-w64-mingw32-gcc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c -ld3d11 -ld3d12 -ldxguid
python3 tools/threads/live_disc.py "$WORK/test.gdi" --dma
fails=0

# run <core> <api> <picture> <options...>: 100 frames, from nothing kept
run() {
   run_core=$1; api=$2; out=$3; shift 3
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan VKD3D_CONFIG=virtual_heaps \
      D3DH_API=$api D3DH_FRONTEND=sdr D3DH_DIR="Z:$WORK/dir" \
      WINEDLLOVERRIDES="winedbg.exe=d${OVERRIDES:+;$OVERRIDES}" \
      timeout 600 wine "$WORK/d3d_hdr.exe" "Z:$run_core" "Z:$WORK/test.gdi" "Z:$WORK/$out" 100 "$@" \
      > "$WORK/$out.log" 2>&1 || { echo "  FAIL: the $out run failed:"; tail -5 "$WORK/$out.log"; return 1; }
}

APIS=11
OVERRIDES=
if [ -n "${D3D12_DLLS:-}" ]; then
   cp "$D3D12_DLLS/d3d12.dll" "$D3D12_DLLS/d3d12core.dll" "$WORK/"
   OVERRIDES="d3d12,d3d12core=n"
   APIS="11 12"
else
   echo "d3d_dma: no D3D12_DLLS, Direct3D 12 skipped"
fi
for api in $APIS; do
   for r in per-triangle per-pixel; do
      case $r in
         per-triangle) sort="reicast_alpha_sorting=per-triangle (normal)" ;;
         per-pixel) sort="reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      for scale in 1 2; do
         for threaded in enabled disabled; do
            name="d3d$api-$r-${scale}x-$threaded"
            echo "$name"
            run "$CORE" $api "$name.raw" "$sort" reicast_render_to_texture_upscaling=${scale}x \
               reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
            python3 tools/rend/dma_check.py "$WORK/$name.raw" || fails=$((fails + 1))
            if [ -n "$OTHER" ]; then
               run "$OTHER" $api "other-$name.raw" "$sort" reicast_render_to_texture_upscaling=${scale}x \
                  reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
               if ! cmp -s "$WORK/$name.raw" "$WORK/other-$name.raw"; then
                  echo "  FAIL: the picture is not the other core's"
                  fails=$((fails + 1))
               fi
            fi
         done
      done
   done
done

if [ $fails != 0 ]; then
   echo "d3d_dma: $fails failed"
   exit 1
fi
echo "d3d_dma: all passed"
