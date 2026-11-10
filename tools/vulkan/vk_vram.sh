#!/bin/sh
# Renders to textures draw into the screen's attachments - the depth with
# the per-triangle and per-strip renderers, the colour and depth the
# per-pixel one keeps between its passes - which are made large enough for
# a render of a screen (TextureDrawer::ShareExtent()), and none of their
# own is made: no more video memory than the screen's, at the console's
# resolution and above. And a render draws its own pixels, not the whole
# texture, a power of two, that it is in: past them the texture is black,
# as when the render pass cleared it all (Texture::ClearOutside()).
#
# The test disc built with -DLIVE_ODD (tools/threads/live_disc.py --odd),
# with a render to a texture of a size no power of two, 120 by 50 and 96
# by 40 in turn and then 96 by 40, drawn as its whole texture: red, then
# black past it, the strip the larger one drew too (tools/rend/odd_check.py)
# - each raster renderer, at the console's resolution, twice it, and with
# the renders to textures at twice it; under the validation layer when it
# is installed. The core says how the renders were drawn when it goes:
# all with the screen's attachments, and some textures cleared outside
# the render. The image memory at the most is printed (VKH_IMAGE_BYTES);
# given a second core, its pictures are the same, and it is printed too.
#
#   sh tools/vulkan/vk_vram.sh [core.so [other.so]]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/vk_vram.<pid>. Needs
# python3 and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
OTHER=${2:+$(readlink -f "$2")}
WORK=$(readlink -f "${TMPDIR:-/tmp}")/vk_vram.$$
mkdir -p "$WORK"
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/rend/alloc_count.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
python3 tools/threads/live_disc.py "$WORK/odd.gdi" --odd > /dev/null
if [ -z "${VK_INSTANCE_LAYERS+set}" ] && ls /usr/share/vulkan/explicit_layer.d/*khronos_validation* >/dev/null 2>&1; then
   export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
fails=0

# frames <core> <name> <options...>: 160 frames, the core's notices kept
frames() {
   run_core=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   env VKH_DIR="$WORK/dir" VKH_FRONTEND=sdr VKH_VERBOSE=1 VKH_UNLOAD=1 VKH_IMAGE_BYTES=1 \
      timeout 900 "$WORK/vk_hdr" "$run_core" "$WORK/odd.gdi" "$WORK/$run_name.raw" 160 "$@" > "$WORK/$run_name.log" 2>&1
}

# run <name> <options...>
run() {
   name=$1; shift
   if ! frames "$CORE" "$name" "$@"; then
      echo "  FAIL: $name: the run failed:"; tail -5 "$WORK/$name.log"
      fails=$((fails + 1))
      return 0
   fi
   said=$(sed -n "s/.*Renders to textures: \([0-9]*\) drawn with the screen's [a-z]*, \([0-9]*\) with [a-z ]*own; \([0-9]*\) cleared outside.*/\1 \2 \3/p" "$WORK/$name.log" | tail -1)
   peak=$(sed -n 's/.*image memory peak \([0-9]*\) KiB.*/\1/p' "$WORK/$name.log")
   echo "$name: renders drawn with the screen's attachments, with their own, textures cleared outside: ${said:-not said}; image memory at the most ${peak:-?} KiB"
   shared=${said%% *}; cleared=${said##* }; own=${said#* }; own=${own%% *}
   if [ -z "$said" ] || [ "$shared" -eq 0 ] || [ "$own" -ne 0 ] || [ "$cleared" -eq 0 ]; then
      echo "  FAIL: $name: not every render drew with the screen's attachments, or no texture was cleared outside"
      fails=$((fails + 1))
   fi
   if [ -n "${VK_INSTANCE_LAYERS:-}" ] && ! grep -q "validation clean" "$WORK/$name.log"; then
      echo "  FAIL: $name: the validation layer said something:"; grep -m5 -i "VUID\|validation" "$WORK/$name.log"
      fails=$((fails + 1))
   fi
   python3 tools/rend/odd_check.py "$WORK/$name.raw" || { echo "  ($name)"; fails=$((fails + 1)); }
   if [ -n "$OTHER" ]; then
      if ! frames "$OTHER" "other-$name" "$@"; then
         echo "  FAIL: $name: the other core's run failed"; fails=$((fails + 1))
      elif ! cmp -s "$WORK/$name.raw" "$WORK/other-$name.raw"; then
         echo "  FAIL: $name: not the other core's picture"; fails=$((fails + 1))
      else
         echo "   the other core's: $(sed -n 's/.*image memory peak \([0-9]*\) KiB.*/\1/p' "$WORK/other-$name.log") KiB, the same picture"
      fi
   fi
}

for r in tri strip pix; do
   case $r in tri) MODE=$TRI;; strip) MODE=$STRIP;; pix) MODE=$PIX;; esac
   run $r "$MODE"
   run $r-2x "$MODE" reicast_internal_resolution=1280x960
   run $r-rtt2x "$MODE" reicast_render_to_texture_upscaling=2x
done
[ $fails -eq 0 ] || { echo "vk_vram: $fails failed"; exit 1; }
echo "vk_vram: renders to textures drew with the screen's attachments, and black past them"
