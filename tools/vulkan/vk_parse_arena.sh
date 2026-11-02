#!/bin/sh
# A frame's vertices and indices parsed straight into memory the graphics
# card reads (core/rend/parse_arena.h, vulkan/parse_arena_vk.h), not copied
# there after: with Vulkan, on a frontend that runs the core's frames as
# RetroArch does (vk_hdr.c), under the validation layer when it is
# installed. On a card whose memory the CPU reads back fast (lavapipe's)
# every render the CPU parses is drawn from where it was parsed, none
# copied; told that memory is slow to read (VKH_UNCACHED) the core copies
# them, as ever - and the pictures of the two are the same, byte for byte:
# the test discs' plain one, heavy one (more strips than the copy's first
# guess holds: tools/threads/live_disc.py --heavy) and DMA one, with each
# renderer, threaded rendering on and off, and with a temporal upscaler
# on the frontend (its motion vectors drawn from the same geometry).
#
#   sh tools/vulkan/vk_parse_arena.sh [core.so]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/vk_parse_arena.<pid>.
#
# Needs the core built with Vulkan, a Vulkan driver whose memory is the
# CPU's (lavapipe: VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json)
# and python3.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=${TMPDIR:-/tmp}/vk_parse_arena.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
# (a directory a disc: their tracks' names are the same)
for disc in plain heavy dma; do
   mkdir -p "$WORK/$disc"
   case $disc in
      plain) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" ;;
      *) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" --$disc ;;
   esac
done
if [ -z "${VK_INSTANCE_LAYERS+set}" ] && ls /usr/share/vulkan/explicit_layer.d/*khronos_validation* >/dev/null 2>&1; then
   export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
fails=0

# run <disc> <name> <options...>: 100 frames, from nothing kept by an
# earlier run (the discs write to the memory card)
run() {
   run_disc=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   VKH_VERBOSE=1 VKH_DIR="$WORK/dir" VKH_FRONTEND=sdr "$WORK/vk_hdr" "$CORE" "$WORK/$run_disc/test.gdi" \
      "$WORK/$run_name.raw" 100 "$@" > "$WORK/$run_name.log" 2>&1 || {
      { echo "  FAIL: the $run_name run failed:"; tail -5 "$WORK/$run_name.log"; } >&2; return 1; }
   if grep -q "VUID-\|Validation Error" "$WORK/$run_name.log"; then
      { echo "  FAIL: $run_name: the validation layer:"; grep -m3 "VUID-" "$WORK/$run_name.log"; } >&2
      return 1
   fi
}

for disc in plain heavy dma; do
   for r in per-triangle per-pixel per-triangle-upscaler per-pixel-upscaler; do
      unset VKH_UPSCALER
      case $r in
         per-triangle*) set -- "reicast_alpha_sorting=per-triangle (normal)" ;;
         per-pixel*) set -- "reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      case $r in *-upscaler) export VKH_UPSCALER=1440x1080:960x720 ;; esac
      for threaded in enabled disabled; do
         name="$disc-$r-$threaded"
         echo "$name"
         run $disc "$name" "$@" reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
         VKH_UNCACHED=1 run $disc "$name-copied" "$@" reicast_threaded_rendering=$threaded \
            || { fails=$((fails + 1)); continue; }
         if ! grep -q "renders parsed where the card reads them" "$WORK/$name.log" \
               || grep -q "a render copied to the card" "$WORK/$name.log"; then
            echo "  FAIL: not every render parsed where the card reads it"
            fails=$((fails + 1))
         fi
         if grep -q "renders parsed where the card reads them" "$WORK/$name-copied.log"; then
            echo "  FAIL: with memory slow to read back, the parse is still put there"
            fails=$((fails + 1))
         fi
         if ! cmp -s "$WORK/$name.raw" "$WORK/$name-copied.raw"; then
            echo "  FAIL: the picture is not the one of the lists copied"
            fails=$((fails + 1))
         fi
      done
   done
done

# A change of size mid-run (the renderer made again where it is: Resize()),
# the core's context let go at the end: the arena kept, not made again and
# lost - the buffers left after are as many as with the lists copied
# (VKH_BUFFERS) - and the pictures the same.
unset VKH_UPSCALER
for r in per-triangle per-pixel; do
   case $r in
      per-triangle) set -- "reicast_alpha_sorting=per-triangle (normal)" ;;
      per-pixel) set -- "reicast_alpha_sorting=per-pixel (accurate)" ;;
   esac
   name="resize-$r"
   echo "$name"
   export VKH_OPTS_AT=40:reicast_internal_resolution=1x VKH_BUFFERS=1 VKH_UNLOAD=1
   run plain "$name" "$@" reicast_internal_resolution=2x || { fails=$((fails + 1)); unset VKH_OPTS_AT VKH_BUFFERS VKH_UNLOAD; continue; }
   VKH_UNCACHED=1 run plain "$name-copied" "$@" reicast_internal_resolution=2x || fails=$((fails + 1))
   unset VKH_OPTS_AT VKH_BUFFERS VKH_UNLOAD
   left=$(sed -n 's/^buffers left //p' "$WORK/$name.log")
   left_copied=$(sed -n 's/^buffers left //p' "$WORK/$name-copied.log")
   if [ -z "$left" ] || [ "$left" != "$left_copied" ]; then
      echo "  FAIL: ${left:-no count of} buffers left after the change of size, ${left_copied:-none counted} with the lists copied"
      fails=$((fails + 1))
   fi
   if ! grep -q "renders parsed where the card reads them, [0-9]* copied" "$WORK/$name.log" \
         || grep -q "a render copied to the card" "$WORK/$name.log"; then
      echo "  FAIL: not every render parsed where the card reads it"
      fails=$((fails + 1))
   fi
   if ! cmp -s "$WORK/$name.raw" "$WORK/$name-copied.raw"; then
      echo "  FAIL: the picture is not the one of the lists copied"
      fails=$((fails + 1))
   fi
done

if [ $fails != 0 ]; then
   echo "vk_parse_arena: $fails failed"
   exit 1
fi
echo "vk_parse_arena: all passed"
