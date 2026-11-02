#!/bin/sh
# Video memory written by DMA over what was rendered to a texture there
# and is still on the graphics card, and over a texture the game draws with
# (the test disc built with -DLIVE_DMA: tools/threads/live_disc.py --dma),
# with Vulkan, on a frontend that runs the core's frames as RetroArch does
# (vk_hdr.c), under the validation layer when it is installed. A render the
# DMA covers whole is not fetched off the card first (rtt_watch_overwrite());
# one it covers in part is, and keeps what the DMA did not write; a texture
# under the DMA is made again. The top right of the screen has to show cyan
# over yellow, cyan, and green - for each renderer, threaded rendering on
# and off, the renders to textures at the console's size and finer - and,
# given a second core, the same picture as that core's in each. A render
# drawn as a texture of another shape is made into it on the card, not
# read back every frame: no more than 40 reads off the card in the run
# (VKH_READS), 35 of them the program's own of the render it reads every
# frame; read back, the view's are 33 more.
#
#   sh tools/vulkan/vk_dma.sh [core.so [other.so]]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/vk_dma.<pid>.
#
# Needs the core built with Vulkan, a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json) and python3.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
OTHER=${2:+$(readlink -f "$2")}
WORK=${TMPDIR:-/tmp}/vk_dma.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
python3 tools/threads/live_disc.py "$WORK/test.gdi" --dma
if [ -z "${VK_INSTANCE_LAYERS+set}" ] && ls /usr/share/vulkan/explicit_layer.d/*khronos_validation* >/dev/null 2>&1; then
   export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
fails=0

# run <core> <name> <options...>: 100 frames, from nothing kept by an
# earlier run (the disc writes to the memory card)
run() {
   run_core=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   VKH_READS=1 VKH_DIR="$WORK/dir" VKH_FRONTEND=sdr "$WORK/vk_hdr" "$run_core" "$WORK/test.gdi" "$WORK/$run_name.raw" 100 \
      "$@" > "$WORK/$run_name.log" 2>&1 || {
      { echo "  FAIL: the $run_name run failed:"; tail -5 "$WORK/$run_name.log"; } >&2; return 1; }
   if grep -q "VUID-\|Validation Error" "$WORK/$run_name.log"; then
      { echo "  FAIL: $run_name: the validation layer:"; grep -m3 "VUID-" "$WORK/$run_name.log"; } >&2
      return 1
   fi
}


for r in per-triangle per-pixel; do
   case $r in
      per-triangle) set -- "reicast_alpha_sorting=per-triangle (normal)" ;;
      per-pixel) set -- "reicast_alpha_sorting=per-pixel (accurate)" ;;
   esac
   for scale in 1 2; do
      for threaded in enabled disabled; do
         name="$r-${scale}x-$threaded"
         echo "$name"
         run "$CORE" "$name" "$@" reicast_render_to_texture_upscaling=${scale}x reicast_threaded_rendering=$threaded \
            || { fails=$((fails + 1)); continue; }
         python3 tools/rend/dma_check.py "$WORK/$name.raw" || fails=$((fails + 1))
         reads=$(sed -n 's/^reads //p' "$WORK/$name.log")
         if [ "${reads:-999}" -gt 40 ]; then
            echo "  FAIL: ${reads:-no count of} reads off the card, more than 40"
            fails=$((fails + 1))
         fi
         if [ -n "$OTHER" ]; then
            run "$OTHER" "other-$name" "$@" reicast_render_to_texture_upscaling=${scale}x reicast_threaded_rendering=$threaded \
               || { fails=$((fails + 1)); continue; }
            if ! cmp -s "$WORK/$name.raw" "$WORK/other-$name.raw"; then
               echo "  FAIL: the picture is not the other core's"
               fails=$((fails + 1))
            fi
         fi
      done
   done
done

if [ $fails != 0 ]; then
   echo "vk_dma: $fails failed"
   exit 1
fi
echo "vk_dma: all passed"
