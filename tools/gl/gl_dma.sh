#!/bin/sh
# Video memory written by DMA over what was rendered to a texture there
# and is still on the graphics card, and over a texture the game draws with
# (the test disc built with -DLIVE_DMA: tools/threads/live_disc.py --dma),
# with OpenGL - the per-triangle renderer and the per-pixel one (OpenGL 4)
# - on a frontend with a real OpenGL and no screen (gl_frames.c: Mesa's
# surfaceless EGL, llvmpipe), as tools/vulkan/vk_dma.sh has it with
# Vulkan. A render the
# DMA covers whole is not fetched off the card first (rtt_watch_overwrite());
# one it covers in part is, and keeps what the DMA did not write; a texture
# under the DMA is made again. The top right of the screen has to show cyan
# over yellow, cyan, and green - for each renderer, threaded rendering on
# and off, the renders to textures at the console's size and finer - and,
# given a second core, the same picture as that core's in each.
#
#   sh tools/gl/gl_dma.sh [core.so [other.so]]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/gl_dma.<pid>.
#
# Needs the core built with OpenGL, Mesa's EGL (libegl1-mesa-dev,
# libgl1-mesa-dri) and python3.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
OTHER=${2:+$(readlink -f "$2")}
WORK=${TMPDIR:-/tmp}/gl_dma.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
python3 tools/threads/live_disc.py "$WORK/test.gdi" --dma
fails=0

# run <core> <name> <options...>: 100 frames, from nothing kept by an
# earlier run (the disc writes to the memory card)
run() {
   run_core=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   GLF_DIR="$WORK/dir" "$WORK/gl_frames" "$run_core" "$WORK/test.gdi" "$WORK/$run_name.raw" 100 \
      "$@" > "$WORK/$run_name.log" 2>&1 || {
      { echo "  FAIL: the $run_name run failed:"; tail -5 "$WORK/$run_name.log"; } >&2; return 1; }
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
   echo "gl_dma: $fails failed"
   exit 1
fi
echo "gl_dma: all passed"
