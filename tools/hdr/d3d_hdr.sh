#!/bin/sh
# The HDR output (Color Format) with Direct3D 11, and 12 with D3D12_DLLS
# given: the core's Windows build
# run under wine (wined3d on a Vulkan driver: lavapipe will do) by a
# frontend that reads back what it is handed (d3d_hdr.c), judged by
# vk_hdr's --same and --compare as tools/vulkan/vk_hdr.sh judges Vulkan's:
#
#  - with an HDR Color Format the frontend refuses, the 24-bit picture,
#    to the byte;
#  - HDR10 and scRGB, per-strip, per-pixel and compute: the 24-bit picture
#    of the same frame, encoded;
#  - a filmic curve is not (the comparison sees a wrong encode).
#
#   sh tools/hdr/d3d_hdr.sh parallel_dc_libretro.dll
#
# Needs mingw-w64, wine, Xvfb, python3 (tools/threads/live_disc.py) and a
# Vulkan driver. Direct3D 12 is run with vkd3d-proton's d3d12.dll and
# d3d12core.dll from $D3D12_DLLS, as tools/rend/tile_test.sh does: wine's
# own vkd3d cannot make the renderer's root signature on a driver with
# eight descriptor sets (lavapipe's). tools/hdr/hdr_d3d.sh tries the
# Direct3D 12 HDR encoder on wine's own.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.dll}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/d3d_hdr.$$
mkdir -p "$WORK/dc"
DISPLAY_NO=${D3D_HDR_DISPLAY:-:92}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; rm -rf "$WORK"' EXIT
${CC_WIN:-x86_64-w64-mingw32-gcc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c -ld3d11 -ld3d12 -ldxguid
${CC:-cc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c -ldl -lm
python3 tools/threads/live_disc.py "$WORK/test.gdi"
# (as vk_hdr.sh: from frame 70, the textured background, mid-tones and saturated colours)
FRAMES=70

API=11
OVERRIDES=
run() {
   # @1 the frontend, @2 the picture, @3... options
   fe=$1; out=$2; shift 2
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan \
      ${OVERRIDES:+WINEDLLOVERRIDES=$OVERRIDES} D3DH_API=$API D3DH_FRONTEND=$fe D3DH_DIR="Z:$WORK" \
      timeout 600 wine "$WORK/d3d_hdr.exe" "Z:$CORE" "Z:$WORK/test.gdi" "Z:$WORK/$out" $FRAMES "$@" \
      > "$WORK/log.txt" 2>&1 || { cat "$WORK/log.txt"; echo "d3d_hdr.sh: $out failed"; exit 1; }
   grep "^d3d_hdr:" "$WORK/log.txt"
}

judge() {
run sdr sdr.raw
run sdr refused.raw reicast_color_format=30bit_hdr
"$WORK/vk_hdr" --same "$WORK/sdr.raw" "$WORK/refused.raw"

for r in "per-strip (fast, least accurate)|rasterizer|strip" \
         "per-pixel (accurate)|rasterizer|pixel" \
         "per-pixel (accurate)|compute|compute"; do
   sort=${r%%|*}; rest=${r#*|}; method=${rest%%|*}; name=${rest#*|}
   set -- "reicast_alpha_sorting=$sort" "reicast_rendering_method=$method"
   run sdr "$name.raw" "$@"
   run hdr10 "$name-hdr10.raw" reicast_color_format=30bit_hdr "$@"
   "$WORK/vk_hdr" --compare "$WORK/$name.raw" "$WORK/$name-hdr10.raw"
   run scrgb "$name-scrgb.raw" reicast_color_format=64bit_scrgb "$@"
   "$WORK/vk_hdr" --compare "$WORK/$name.raw" "$WORK/$name-scrgb.raw"
done

# (the comparison has to see a wrong encode, or it proves nothing)
run hdr10 aces.raw reicast_color_format=30bit_hdr reicast_hdr_display_transform=aces
if "$WORK/vk_hdr" --compare "$WORK/sdr.raw" "$WORK/aces.raw" > /dev/null 2>&1; then
   echo "d3d_hdr.sh: ACES (Narkowicz) passed for the reference knee"
   exit 1
fi
}

judge
if [ -n "$D3D12_DLLS" ]; then
   cp "$D3D12_DLLS/d3d12.dll" "$D3D12_DLLS/d3d12core.dll" "$WORK/"
   API=12
   OVERRIDES="d3d12,d3d12core=n"
   judge
fi
echo "d3d_hdr.sh: passed"
