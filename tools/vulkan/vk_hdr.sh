#!/bin/sh
# The HDR output (Color Format) with Vulkan, on a frontend that runs the
# core's frames as RetroArch does and reads back the picture handed over
# (vk_hdr.c), under the validation layer:
#
#  - the 24-bit picture is the same with Color Format 24-bit as with an
#    HDR Color Format the frontend refuses, to the byte;
#  - HDR10 and scRGB, for each renderer - per-strip, per-triangle,
#    per-pixel, with and without SSAA, and compute (which hands over its
#    picture the same way with SSAA as without) - and for a frame written
#    to the framebuffer by the CPU, are the 24-bit picture of the same frame,
#    encoded: below paper white the reference knee is the identity, and
#    vk_hdr --compare encodes the 24-bit picture on the CPU and holds the
#    HDR one to it;
#  - with the chip's write-out on, the picture written out, encoded;
#  - with it off, what was resolved - SSAA's samples on every renderer,
#    the X scaler's pair of pixels on the compute renderer - is encoded
#    from their exact average, not the byte the picture rounded it to,
#    and is not dithered (vk_hdr --exact; on the disc with a slanted
#    shaded triangle, and that with the X scaler, which the old code
#    fails);
#  - with the HDR post effects (bloom in each of its shapes, halation,
#    chromatic aberration, Compact Intermediates), on each renderer, with
#    SSAA too, the scRGB picture is what the CPU makes of the 24-bit one
#    through them (vk_hdr --post, tools/hdr/hdr_post_ref.c), and not what
#    it makes with other settings; HDR10 through them draws;
#  - a filmic curve is not (the comparison sees a wrong encode);
#  - every Display Transform draws, and ACES 2.0 (Full) long enough for
#    its cube to be made and used.
#
#   sh tools/vulkan/vk_hdr.sh [core.so]
#
# Needs the core built with Vulkan, a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json) and python3 for
# the test disc (tools/threads/live_disc.py).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=${TMPDIR:-/tmp}/vk_hdr.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm
python3 tools/threads/live_disc.py "$WORK/test.gdi"
# (each in a directory of its own: the track files are named alike)
mkdir -p "$WORK/slant" "$WORK/hscale"
python3 tools/threads/live_disc.py "$WORK/slant/slant.gdi" --slant
python3 tools/threads/live_disc.py "$WORK/hscale/hscale.gdi" --hscale
DISC=test.gdi
# (from frame 70 the disc's picture has its textured background, mid-tones
# and saturated colours in it; for its first 64 frames it renders through
# the tile accelerator three frames in four, and writes the fourth to the
# framebuffer itself)
FRAMES=70

run() {
   # @1 the frontend, @2 the picture, @3... options
   fe=$1; out=$2; shift 2
   VKH_DIR="$WORK" VKH_FRONTEND=$fe "$WORK/vk_hdr" "$CORE" "$WORK/$DISC" "$WORK/$out" $FRAMES "$@" \
      2>"$WORK/log.txt" || { cat "$WORK/log.txt"; echo "vk_hdr.sh: $out failed"; exit 1; }
}

run sdr sdr.raw
run sdr refused.raw reicast_color_format=30bit_hdr
"$WORK/vk_hdr" --same "$WORK/sdr.raw" "$WORK/refused.raw"

# a frame of each kind: rendered, and written to the framebuffer by the CPU
for FRAMES in 41 42 43 44; do
   run sdr "f$FRAMES.raw"
   run hdr10 "f$FRAMES-hdr10.raw" reicast_color_format=30bit_hdr
   "$WORK/vk_hdr" --compare "$WORK/f$FRAMES.raw" "$WORK/f$FRAMES-hdr10.raw"
done
FRAMES=70

for r in "per-strip (fast, least accurate)|rasterizer|disabled|strip" \
         "per-triangle (normal)|rasterizer|disabled|triangle" \
         "per-pixel (accurate)|rasterizer|disabled|pixel" \
         "per-pixel (accurate)|compute|disabled|compute" \
         "per-triangle (normal)|rasterizer|4x|triangle-ssaa" \
         "per-pixel (accurate)|rasterizer|4x|pixel-ssaa"; do
   sort=${r%%|*}; rest=${r#*|}; method=${rest%%|*}; rest=${rest#*|}; ssaa=${rest%%|*}; name=${rest#*|}
   set -- "reicast_alpha_sorting=$sort" "reicast_rendering_method=$method" "reicast_ssaa=$ssaa"
   run sdr "$name.raw" "$@"
   run hdr10 "$name-hdr10.raw" reicast_color_format=30bit_hdr "$@"
   "$WORK/vk_hdr" --compare "$WORK/$name.raw" "$WORK/$name-hdr10.raw"
   run scrgb "$name-scrgb.raw" reicast_color_format=64bit_scrgb "$@"
   "$WORK/vk_hdr" --compare "$WORK/$name.raw" "$WORK/$name-scrgb.raw"
done

# With the chip's write-out (reicast_pvr2_scaler: the Y filter, the
# 16-bit packing and its dither, after the frame's render pass), the
# picture encoded is the one written out: the HDR subpass leaves it to
# the encode after the write-out on the rasterizers, and the compute
# renderer's own pass is before its encode
for r in "per-strip (fast, least accurate)|rasterizer|disabled|wo-strip" \
         "per-pixel (accurate)|rasterizer|disabled|wo-pixel" \
         "per-triangle (normal)|rasterizer|4x|wo-triangle-ssaa" \
         "per-pixel (accurate)|compute|disabled|wo-compute"; do
   sort=${r%%|*}; rest=${r#*|}; method=${rest%%|*}; rest=${rest#*|}; ssaa=${rest%%|*}; name=${rest#*|}
   set -- reicast_pvr2_scaler=enabled "reicast_alpha_sorting=$sort" "reicast_rendering_method=$method" "reicast_ssaa=$ssaa"
   run sdr "$name.raw" "$@"
   run hdr10 "$name-hdr10.raw" reicast_color_format=30bit_hdr "$@"
   "$WORK/vk_hdr" --compare "$WORK/$name.raw" "$WORK/$name-hdr10.raw"
done
run scrgb wo-scrgb.raw reicast_color_format=64bit_scrgb reicast_pvr2_scaler=enabled \
   "reicast_alpha_sorting=per-strip (fast, least accurate)" reicast_rendering_method=rasterizer reicast_ssaa=disabled
"$WORK/vk_hdr" --compare "$WORK/wo-strip.raw" "$WORK/wo-scrgb.raw"
# (...and the picture before its write-out is not that: the comparison sees it)
if "$WORK/vk_hdr" --compare "$WORK/wo-strip.raw" "$WORK/strip-hdr10.raw" > /dev/null 2>&1; then
   echo "vk_hdr.sh: the picture before its write-out passed for the one written out"
   exit 1
fi

# The HDR post effects: the scRGB picture is what the CPU makes of the
# 24-bit picture of the same frame through them, on each renderer
P=reicast_hdr_bloom=standard
for name in strip triangle pixel compute triangle-ssaa pixel-ssaa; do
   case $name in
   strip) set -- "reicast_alpha_sorting=per-strip (fast, least accurate)" reicast_rendering_method=rasterizer reicast_ssaa=disabled ;;
   triangle) set -- "reicast_alpha_sorting=per-triangle (normal)" reicast_rendering_method=rasterizer reicast_ssaa=disabled ;;
   pixel) set -- "reicast_alpha_sorting=per-pixel (accurate)" reicast_rendering_method=rasterizer reicast_ssaa=disabled ;;
   compute) set -- "reicast_alpha_sorting=per-pixel (accurate)" reicast_rendering_method=compute reicast_ssaa=disabled ;;
   triangle-ssaa) set -- "reicast_alpha_sorting=per-triangle (normal)" reicast_rendering_method=rasterizer reicast_ssaa=4x ;;
   pixel-ssaa) set -- "reicast_alpha_sorting=per-pixel (accurate)" reicast_rendering_method=rasterizer reicast_ssaa=4x ;;
   esac
   run scrgb "$name-post.raw" reicast_color_format=64bit_scrgb $P "$@"
   "$WORK/vk_hdr" --post "$WORK/$name.raw" "$WORK/$name-post.raw" $P
done
# ...each of their settings, on the per-triangle renderer
for fx in "reicast_hdr_bloom=intense reicast_hdr_bloom_range=widest reicast_hdr_halation=strong" \
          "reicast_hdr_bloom=subtle reicast_hdr_bloom_convolution=enabled reicast_hdr_bloom_select=knee" \
          "reicast_hdr_chromatic=heavy" \
          "reicast_hdr_bloom=standard reicast_hdr_chromatic=subtle reicast_hdr_post_compact=enabled"; do
   run scrgb fx.raw reicast_color_format=64bit_scrgb $fx
   "$WORK/vk_hdr" --post "$WORK/sdr.raw" "$WORK/fx.raw" $fx
done
# (...and the comparison sees other settings than the picture's: the
# packed intermediates' picture, the last, is not the half floats')
if "$WORK/vk_hdr" --post "$WORK/sdr.raw" "$WORK/fx.raw" reicast_hdr_bloom=standard reicast_hdr_chromatic=subtle > /dev/null 2>&1; then
   echo "vk_hdr.sh: Compact Intermediates drew as half floats"
   exit 1
fi
run scrgb fx.raw reicast_color_format=64bit_scrgb reicast_hdr_bloom=standard
for fx in "reicast_hdr_bloom=standard reicast_hdr_halation=subtle" "reicast_hdr_bloom=subtle" \
          "reicast_hdr_bloom=standard reicast_hdr_bloom_convolution=enabled" \
          "reicast_hdr_bloom=standard reicast_hdr_bloom_range=wide"; do
   if "$WORK/vk_hdr" --post "$WORK/sdr.raw" "$WORK/fx.raw" $fx > /dev/null 2>&1; then
      echo "vk_hdr.sh: a bloom of other settings passed for $fx"
      exit 1
   fi
done
# HDR10 through them
run hdr10 fx.raw reicast_color_format=30bit_hdr reicast_hdr_bloom=intense reicast_hdr_chromatic=strong

# What was resolved, encoded from its exact average: SSAA's samples, on
# each renderer, the slanted triangle's edges between them...
DISC=slant/slant.gdi
for r in "per-strip (fast, least accurate)|rasterizer|x-strip" \
         "per-triangle (normal)|rasterizer|x-triangle" \
         "per-pixel (accurate)|rasterizer|x-pixel" \
         "per-pixel (accurate)|compute|x-compute"; do
   sort=${r%%|*}; rest=${r#*|}; method=${rest%%|*}; name=${rest#*|}
   set -- "reicast_alpha_sorting=$sort" "reicast_rendering_method=$method" reicast_ssaa=4x
   run sdr "$name.raw" "$@"
   run scrgb "$name-scrgb.raw" reicast_color_format=64bit_scrgb "$@"
   "$WORK/vk_hdr" --exact "$WORK/$name.raw" "$WORK/$name-scrgb.raw"
done
# ...and the compute renderer's X scaler's pairs of pixels, at one sample
DISC=hscale/hscale.gdi
set -- reicast_pvr2_scaler=enabled "reicast_alpha_sorting=per-pixel (accurate)" reicast_rendering_method=compute
run sdr x-hscale.raw "$@"
run scrgb x-hscale-scrgb.raw reicast_color_format=64bit_scrgb "$@"
"$WORK/vk_hdr" --exact "$WORK/x-hscale.raw" "$WORK/x-hscale-scrgb.raw"
DISC=test.gdi

# (the comparison has to see a wrong encode, or it proves nothing)
run hdr10 aces.raw reicast_color_format=30bit_hdr reicast_hdr_display_transform=aces
if "$WORK/vk_hdr" --compare "$WORK/sdr.raw" "$WORK/aces.raw" > /dev/null 2>&1; then
   echo "vk_hdr.sh: ACES (Narkowicz) passed for the reference knee"
   exit 1
fi

FRAMES=8
for dt in knee_exp rplain rext jodie devlin acesfit aces2 hejl filmicalu filmiclog hable \
          hable2017 lottes gt unreal neutral agx drago expo tumblin ward schlick; do
   run hdr10 dt.raw reicast_color_format=30bit_hdr reicast_hdr_display_transform=$dt \
      reicast_hdr_expansion=hue reicast_hdr_highlight_hue=enabled
done
# ACES 2.0 (Full): the cube is baked off the frame by its worker, then drawn with
FRAMES=300
run scrgb dt.raw reicast_color_format=64bit_scrgb reicast_hdr_display_transform=aces2full
echo "vk_hdr.sh: passed"
