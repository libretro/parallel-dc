#!/bin/sh
# The core built with a sanitizer (make DEBUG_ASAN=1 or DEBUG_UBSAN=1) runs
# the test disc's frames (tools/threads/live_disc.py) on Vulkan, by the
# frontend vk_hdr.c is, on each renderer, at 24-bit and with the HDR
# output, with the recompiler and threaded rendering as they are by
# default: any report fails the run. What it has caught: the flash ROM's
# CRC and the AICA's filter shifting negative values left, the
# recompiler's register lists ending in a value no register enum has,
# div1's -b overflowing at INT_MIN, the modifier volumes' misaligned
# double load (rend/shadows.h).
#
#   SANITIZE=address|undefined sh tools/vulkan/vk_sanitize.sh [core.so]
#
# Needs a Vulkan driver (lavapipe will do), python3 and the compiler's
# sanitizer runtime (gcc -print-file-name finds it).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
SANITIZE=${SANITIZE:-undefined}
WORK=${TMPDIR:-/tmp}/vk_sanitize.$$
mkdir -p "$WORK/log"
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm
python3 tools/threads/live_disc.py "$WORK/test.gdi"
case "$SANITIZE" in
address)
   RUNTIME=$(${CC:-cc} -print-file-name=libasan.so)
   export ASAN_OPTIONS="halt_on_error=1:detect_leaks=0:log_path=$WORK/log/san" ;;
undefined)
   RUNTIME=$(${CC:-cc} -print-file-name=libubsan.so)
   export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$WORK/log/san" ;;
*)
   echo "vk_sanitize.sh: SANITIZE is address or undefined" >&2; exit 2 ;;
esac
FRAMES=70

run() {
   # @1 the frontend, @2 a name, @3... options
   fe=$1; name=$2; shift 2
   if ! LD_PRELOAD=$RUNTIME VKH_DIR="$WORK" VKH_FRONTEND=$fe \
         "$WORK/vk_hdr" "$CORE" "$WORK/test.gdi" "$WORK/$name.raw" $FRAMES "$@" > "$WORK/out.txt" 2>&1 \
         || [ -n "$(ls "$WORK/log")" ]; then
      cat "$WORK/out.txt" "$WORK"/log/* 2>/dev/null | tail -40
      echo "vk_sanitize.sh: $name: the $SANITIZE sanitizer"
      exit 1
   fi
   echo "vk_sanitize.sh: $name clean"
}

run sdr triangle
run sdr strip "reicast_alpha_sorting=per-strip (fast, least accurate)"
run sdr pixel "reicast_alpha_sorting=per-pixel (accurate)"
run sdr compute "reicast_alpha_sorting=per-pixel (accurate)" reicast_rendering_method=compute
run sdr triangle-ssaa reicast_ssaa=4x
# (the frontend's temporal upscaler: the motion vector pass, the strips matched on this side)
VKH_UPSCALER=1440x1080:960x720 run sdr upscaler
VKH_UPSCALER=1440x1080:960x720 run sdr upscaler-ssaa reicast_ssaa=4x
VKH_UPSCALER=1440x1080:960x720 run sdr upscaler-pixel "reicast_alpha_sorting=per-pixel (accurate)"
VKH_UPSCALER=1440x1080:960x720 run sdr upscaler-compute "reicast_alpha_sorting=per-pixel (accurate)" reicast_rendering_method=compute
run sdr write-out reicast_pvr2_scaler=enabled
run hdr10 hdr10 reicast_color_format=30bit_hdr
run scrgb scrgb reicast_color_format=64bit_scrgb reicast_hdr_display_transform=aces2
# (SSAA's channel sums for the encode: the per-pixel and compute renderers')
run scrgb pixel-ssaa-scrgb reicast_color_format=64bit_scrgb "reicast_alpha_sorting=per-pixel (accurate)" reicast_ssaa=4x
run scrgb compute-ssaa-scrgb reicast_color_format=64bit_scrgb "reicast_alpha_sorting=per-pixel (accurate)" reicast_rendering_method=compute reicast_ssaa=4x
# (the HDR post effects: the bloom's pyramid, the aberration, Compact Intermediates)
run scrgb post-scrgb reicast_color_format=64bit_scrgb reicast_hdr_bloom=standard reicast_hdr_bloom_convolution=enabled \
   reicast_hdr_chromatic=strong reicast_hdr_post_compact=enabled
run hdr10 post-hdr10 reicast_color_format=30bit_hdr reicast_hdr_bloom=intense reicast_hdr_halation=subtle
echo "vk_sanitize.sh: passed ($SANITIZE)"
