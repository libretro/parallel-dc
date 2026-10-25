#!/bin/sh
# The HDR output's encode with Direct3D 11 and 12 - the core's encoders
# (rend/hdr/hdr_encode_d3d11.c, hdr_encode_d3d12.c) and the DXBC committed
# for them - built with mingw-w64 and run under wine (wined3d and vkd3d,
# on a Vulkan driver: lavapipe will do), over every case hdr_hlsl.sh
# draws, and held to the Vulkan renderer's GLSL (hdr_hlsl --against);
# and Direct3D 12's with SSAA's samples to their exact average (12ssaa);
# and both with the post effects to what the CPU makes of the picture
# (post11, post12: tools/hdr/hdr_post_ref.c).
#
#   sh tools/hdr/hdr_d3d.sh
#
# Needs mingw-w64, wine, Xvfb (wine's Direct3D wants a display),
# glslangValidator and a Vulkan driver.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/hdr_d3d.$$
mkdir -p "$WORK"
CC_WIN=${CC_WIN:-x86_64-w64-mingw32-gcc}
DISPLAY_NO=${HDR_D3D_DISPLAY:-:93}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; rm -rf "$WORK"' EXIT
LRC=core/libretro-common
$CC_WIN -O2 -Wall -I$LRC/include -Icore/deps/khronos -o "$WORK/hdr_d3d.exe" tools/hdr/hdr_d3d.c \
   core/rend/hdr/hdr_encode_d3d11.c core/rend/hdr/hdr_encode_d3d12.c core/rend/hdr/hdr_output.c \
   core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c core/rend/hdr/hdr_display.c \
   core/rend/hdr/hdr_libretro.c tools/hdr/hdr_post_ref.c \
   $LRC/rthreads/rthreads.c $LRC/rthreads/retro_eventcount.c \
   -ld3d11 -ld3d12 -ldxguid -lm
for api in 11 12; do
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan \
      wine "$WORK/hdr_d3d.exe" $api "Z:$WORK/d3d$api.bin"
   HDR_HLSL_AGAINST="$WORK/d3d$api.bin" sh tools/vulkan/hdr_hlsl.sh
done
# Direct3D 12's encode with SSAA's samples: their exact average
env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all wine "$WORK/hdr_d3d.exe" 12ssaa
# and with their channel sums, as the per-pixel renderer stores them
env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all wine "$WORK/hdr_d3d.exe" 12sums
# the post effects, against the CPU's
for api in 11 12; do
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan \
      wine "$WORK/hdr_d3d.exe" post$api
done
echo "hdr_d3d.sh: passed"
