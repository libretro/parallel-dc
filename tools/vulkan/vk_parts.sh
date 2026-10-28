#!/bin/sh
# Textures written a few bytes at a time, as a game writes into an atlas
# (the test disc built with -DLIVE_ATLAS: tools/threads/live_disc.py
# --atlas), with Vulkan, on a frontend that runs the core's frames as
# RetroArch does (vk_hdr.c), under the validation layer. The texture
# cache makes only the parts of them that changed again; for each
# renderer, the picture is the same with the textures made on the GPU and
# on the CPU, with threaded rendering on and off - and, given a second
# core (one built before a change to the texture cache), the same as that
# core's in each.
#
#   sh tools/vulkan/vk_parts.sh [core.so [other.so]]
#
# VKPARTS_FRAMES: how many frames are run before the picture is taken
# (160 by default; the disc writes into its textures from frame 64 on, and
# much of each at frames 95, 127, 159...). VKPARTS_RENDERERS: the lanes,
# of per-triangle, per-strip, per-pixel and compute (all but compute by
# default: lavapipe takes minutes over the compute renderer's frames).
# KEEP=1: the pictures and logs are kept, in $TMPDIR/vk_parts.<pid>.
#
# Needs the core built with Vulkan, a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json) and python3 for
# the test disc.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
OTHER=${2:+$(readlink -f "$2")}
WORK=${TMPDIR:-/tmp}/vk_parts.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
python3 tools/threads/live_disc.py "$WORK/test.gdi" --atlas
FRAMES=${VKPARTS_FRAMES:-160}
RENDERERS=${VKPARTS_RENDERERS:-"per-triangle per-strip per-pixel"}
fails=0

# run <core> <name> <options...>: from nothing kept by an earlier run (the
# disc writes to the memory card)
run() {
   run_core=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   VKH_DIR="$WORK/dir" VKH_FRONTEND=sdr "$WORK/vk_hdr" "$run_core" "$WORK/test.gdi" "$WORK/$run_name.raw" $FRAMES \
      reicast_internal_resolution=2x "$@" > "$WORK/$run_name.log" 2>&1 || {
      { echo "  FAIL: the $run_name run failed:"; tail -5 "$WORK/$run_name.log"; } >&2; return 1; }
}

for r in $RENDERERS; do
   case $r in
      per-triangle) set -- "reicast_alpha_sorting=per-triangle (normal)" ;;
      per-strip) set -- "reicast_alpha_sorting=per-strip (fast, least accurate)" ;;
      per-pixel) set -- "reicast_alpha_sorting=per-pixel (accurate)" ;;
      compute) set -- "reicast_alpha_sorting=per-pixel (accurate)" "reicast_rendering_method=compute" ;;
      *) echo "vk_parts: no renderer $r" >&2; exit 2 ;;
   esac
   echo "$r"
   before=$fails
   first=
   for gpu in enabled disabled; do
      for threaded in enabled disabled; do
         name="$r-$gpu-$threaded"
         run "$CORE" "$name" "$@" reicast_gpu_texture_decode=$gpu reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
         if [ -z "$first" ]; then
            first=$name
         elif ! cmp -s "$WORK/$first.raw" "$WORK/$name.raw"; then
            echo "  FAIL: the picture with GPU decode $gpu, threaded rendering $threaded is not the one with $first's"
            fails=$((fails + 1))
         fi
         if [ -n "$OTHER" ]; then
            run "$OTHER" "other-$name" "$@" reicast_gpu_texture_decode=$gpu reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
            if ! cmp -s "$WORK/$name.raw" "$WORK/other-$name.raw"; then
               echo "  FAIL: the picture with GPU decode $gpu, threaded rendering $threaded is not the other core's"
               fails=$((fails + 1))
            fi
         fi
      done
   done
   if [ $fails = $before ]; then
      echo "  GPU decode on and off, threaded rendering on and off: the same picture${OTHER:+, the other core's}"
   fi
done

if [ $fails != 0 ]; then
   echo "vk_parts: $fails failed"
   exit 1
fi
echo "vk_parts: all passed"
