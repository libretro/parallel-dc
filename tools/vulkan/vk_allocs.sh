#!/bin/sh
# Allocations during play (AGENTS.md, "No allocations during play"): the
# core under a Vulkan frontend as RetroArch runs it (vk_hdr.c with
# tools/rend/alloc_count.c, built with -DALLOC_COUNT: VKH_ALLOCS,
# VKH_ALLOC_SITES), the test discs, every
# Vulkan renderer (per-triangle, threaded rendering on and off, per-strip,
# per-pixel, compute), from frame 20 of 100 on; and with run-ahead
# (VKH_AHEAD: a state saved and loaded every frame), the plain disc and the
# one with a render to a texture in its state. Each place in the core that
# allocated - the heap, or Vulkan memory, buffers, images and pools - is
# named by its function (vk_alloc_sites.py) and must be on
# tools/vulkan/vk_allocs.allowed. That list only ever shrinks: a commit
# that removes a site takes it off.
#
#   sh tools/vulkan/vk_allocs.sh [core.so]
#
# KEEP=1: the logs are kept, in $TMPDIR/vk_allocs.<pid>. Needs python3,
# addr2line and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/vk_allocs.$$
mkdir -p "$WORK"
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -DALLOC_COUNT -rdynamic -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/rend/alloc_count.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
# (each disc in its own directory: their tracks have the same names)
for d in plain atlas cull dma blend keep; do
   mkdir -p "$WORK/$d"
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi" --$d; fi > /dev/null
done
TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
fails=0

# run <name> <disc> <options...>: 100 frames, counted from the 20th
run() {
   name=$1; disc=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   if ! env VKH_DIR="$WORK/dir" VKH_FRONTEND=sdr VKH_ALLOCS=20 VKH_ALLOC_SITES=1 \
         timeout 900 "$WORK/vk_hdr" "$CORE" "$WORK/$disc/$disc.gdi" "$WORK/$name.raw" 100 "$@" \
         > "$WORK/$name.log" 2>&1; then
      echo "  FAIL: $name: the run failed:"; tail -5 "$WORK/$name.log"
      fails=$((fails + 1))
      return 0
   fi
   echo "$name:$(sed -n 's/.*during play (frames 20 to 99)://p' "$WORK/$name.log")"
}

for d in plain atlas cull dma blend; do
   run $d-tri $d "$TRI"
   run $d-tri-nothread $d "$TRI" reicast_threaded_rendering=disabled
   run $d-strip $d "$STRIP"
   run $d-pix $d "$PIX"
   run $d-compute $d "$PIX" reicast_rendering_method=compute
done
# with run-ahead (a state saved and loaded every frame): the plain disc, and
# the one whose state has a render to a texture on the card in it
for d in plain keep; do
   VKH_AHEAD=10 run $d-tri-ahead $d "$TRI"
   VKH_AHEAD=10 run $d-pix-ahead $d "$PIX"
   VKH_AHEAD=10 run $d-compute-ahead $d "$PIX" reicast_rendering_method=compute
done
python3 tools/vulkan/vk_alloc_sites.py "$CORE" tools/vulkan/vk_allocs.allowed "$WORK"/*.log || fails=$((fails + 1))
[ $fails -eq 0 ] || { echo "vk_allocs: $fails failed"; exit 1; }
echo "vk_allocs: no allocation site that is not on the list"
