#!/bin/sh
# Two Linux builds of the core drawing the same with Vulkan: the last
# picture each hands over (tools/vulkan/vk_hdr.c), byte for byte, on the
# test discs (tools/threads/live_disc.py: plain, the texture atlas,
# culling, DMA over renders to textures, a Gouraud-shaded triangle, seldom
# blends and depth modes), each renderer - per triangle, per strip, per
# pixel, per pixel with SSAA 4x, compute - and per triangle with threaded
# rendering off and with TA processing on the GPU. For a change that
# should draw nothing differently (the renderer made C, say), against the
# build before it, as tools/rend/d3d_same.sh is for Direct3D.
#
#   sh tools/vulkan/vk_same.sh before.so after.so
#
# KEEP=1: the logs and pictures are kept, in $TMPDIR/vk_same.<pid>. A run
# whose last picture has a few colours at most fails, as one that drew
# nothing. Needs a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json) and python3.
set -e
cd "$(dirname "$0")/../.."
[ $# -eq 2 ] || { echo "usage: $0 before.so after.so" >&2; exit 2; }
A=$(readlink -f "$1"); B=$(readlink -f "$2")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/vk_same.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "${VK_DRIVER_FILES:-}${VK_ICD_FILENAMES:-}" ] && export VK_DRIVER_FILES=$LVP
${CC:-cc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/rend/alloc_count.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
# (each disc in its own directory: their tracks have the same names)
for d in plain atlas cull dma slant blend; do
   mkdir -p "$WORK/$d"
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi" --$d; fi > /dev/null
done
FRAMES=${FRAMES:-100}
TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
fails=0
for d in plain atlas cull dma slant blend; do
   for m in tri strip pix pix-ssaa compute tri-nothread tri-gpu; do
      case $m in
      tri) set -- "$TRI";;
      strip) set -- "$STRIP";;
      pix) set -- "$PIX";;
      pix-ssaa) set -- "$PIX" reicast_ssaa=4x;;
      compute) set -- "$PIX" reicast_rendering_method=compute;;
      tri-nothread) set -- "$TRI" reicast_threaded_rendering=disabled;;
      tri-gpu) set -- "$TRI" reicast_ta_processing=gpu;;
      esac
      name=vk-$d-$m
      for c in A B; do
         core=$A; [ $c = B ] && core=$B
         # (from nothing kept by an earlier run: the disc writes to the memory card)
         rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
         VKH_DIR="$WORK/dir" timeout 600 "$WORK/vk_hdr" "$core" "$WORK/$d/$d.gdi" "$WORK/$name.$c.raw" $FRAMES "$@" \
            > "$WORK/$name.$c.log" 2>&1 \
            || { echo "  FAIL: $name ($c) did not run:"; tail -3 "$WORK/$name.$c.log"; fails=$((fails + 1)); }
      done
      if [ -s "$WORK/$name.A.raw" ] && cmp -s "$WORK/$name.A.raw" "$WORK/$name.B.raw"; then
         if ! python3 tools/rend/raw_colours.py "$WORK/$name.B.raw" 16 > /dev/null; then
            echo "  FAIL: $name: drew nothing (its picture a few colours at most)"; fails=$((fails + 1))
            continue
         fi
         echo "same  $name"
      else
         echo "  FAIL: $name: not the same picture"; fails=$((fails + 1))
      fi
   done
done
[ $fails -eq 0 ] || { echo "vk_same: $fails failed"; exit 1; }
echo "vk_same: all the same"
