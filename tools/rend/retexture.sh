#!/bin/sh
# A texture made again in another shape, mid-game: the test disc's
# 1024 by 1024 mipmapped texture (tools/threads/live_disc.py --retex),
# rewritten every 16 frames, with Texture Upscaling turned on, or off,
# from the frontend's menu between two rewrites - a texture with its levels
# one time, one level twice the size the next, which an OpenGL texture
# whose storage is fixed (glTexStorage2D), or a Vulkan image, cannot be
# made into. With OpenGL and OpenGL ES on Mesa's surfaceless EGL
# (tools/gl/gl_frames.c) and with Vulkan (tools/vulkan/vk_hdr.c, under the
# validation layer when it is installed), per triangle and per pixel: no
# OpenGL error in any frame (GLF_ERRORS), nothing from the validation
# layer, and the texture green at the end, as the last rewrite made it.
#
#   VK_DRIVER_FILES=.../lvp_icd.json sh tools/rend/retexture.sh [core.so]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/retexture.<pid>.
#
# Needs the core built with OpenGL and Vulkan, Mesa's EGL
# (libegl1-mesa-dev, libgl1-mesa-dri), a Vulkan driver (lavapipe) and
# python3.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/retexture.$$
mkdir -p "$WORK"
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -rdynamic -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/rend/alloc_count.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
if [ -z "${VK_INSTANCE_LAYERS+set}" ] && ls /usr/share/vulkan/explicit_layer.d/*khronos_validation* >/dev/null 2>&1; then
   export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi
python3 tools/threads/live_disc.py "$WORK/retex.gdi" --retex > /dev/null
fails=0
for api in gl gles vk; do
   for sort in tri pix; do
      [ $api.$sort != gles.pix ] || continue
      case $sort in
         tri) opt="reicast_alpha_sorting=per-triangle (normal)" ;;
         *) opt="reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      for turn in on off; do
         name=$api-$sort-$turn
         if [ $turn = on ]; then
            start=reicast_texupscale=disabled
            change=100:reicast_texupscale=2x
         else
            start=reicast_texupscale=2x
            change=100:reicast_texupscale=disabled
         fi
         mkdir -p "$WORK/$name"
         if [ $api = vk ]; then
            VKH_OPTS_AT=$change VKH_DIR="$WORK/$name" VKH_FRONTEND=sdr \
               timeout 900 "$WORK/vk_hdr" "$CORE" "$WORK/retex.gdi" "$WORK/$name.raw" 300 \
               "$opt" "$start" reicast_texupscale_max_filtered_texture_size=1024 > "$WORK/$name.log" 2>&1
         else
            gl=
            [ $api = gl ] || gl=1
            env ${gl:+GLF_GLES=1} GLF_ERRORS=1 GLF_OPTION=$change GLF_DIR="$WORK/$name" \
               timeout 900 "$WORK/gl_frames" "$CORE" "$WORK/retex.gdi" "$WORK/$name.raw" 300 \
               "$opt" "$start" reicast_texupscale_max_filtered_texture_size=1024 > "$WORK/$name.log" 2>&1
         fi || {
            echo "  FAIL: $name: the run failed:"; tail -5 "$WORK/$name.log"
            fails=$((fails + 1))
            continue
         }
         if grep -q "VUID-\|Validation Error" "$WORK/$name.log"; then
            echo "  FAIL: $name: the validation layer:"; grep -m3 "VUID-" "$WORK/$name.log"
            fails=$((fails + 1))
            continue
         fi
         # (the texture's middle, 640 by 480 being the 320 by 240 screen twice over)
         if python3 - "$WORK/$name.raw" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack('<II', d[8:16])
o = 16 + (436 * w + 612) * 4
r, g, b = d[o], d[o + 1], d[o + 2]
if (w, h) != (640, 480) or r > 8 or g < 247 or b > 8:
    print('  the texture is %02x%02x%02x, not green (%ux%u)' % (r, g, b, w, h))
    sys.exit(1)
PY
         then
            echo "$name: ok"
         else
            echo "  ($name)"; fails=$((fails + 1))
         fi
      done
   done
done
[ $fails -eq 0 ] || { echo "retexture: $fails failed"; exit 1; }
echo "retexture: all passed"
