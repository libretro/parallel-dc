#!/bin/sh
# Polygons drawn outside a clipping rectangle (a tile clip of the
# "inside" kind: rend/tileclip.h), with OpenGL - each sorting and so each
# renderer (per-pixel: OpenGL 4), on OpenGL and on OpenGL ES 3 - on Mesa's
# surfaceless EGL (gl_frames.c), as tools/gl/gl_dma.sh has it. The test
# disc built with -DLIVE_CLIP (tools/threads/live_disc.py --clip) draws
# three rectangles of one kind, the second's clipping rectangle another:
# each has to show a hole where its rectangle is (tools/rend/clip_check.py).
#
#   sh tools/gl/gl_clip.sh [core.so]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/gl_clip.<pid>.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=${TMPDIR:-/tmp}/gl_clip.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
python3 tools/threads/live_disc.py "$WORK/test.gdi" --clip
fails=0

for api in gl gles; do
   for sort in per-triangle per-strip per-pixel; do
      case $sort in
         per-triangle) o="reicast_alpha_sorting=per-triangle (normal)" ;;
         per-strip) o="reicast_alpha_sorting=per-strip (fast, least accurate)" ;;
         per-pixel) o="reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      name=$api-$sort
      echo "$name"
      rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
      # (GLF_GLES set at all asks for OpenGL ES)
      gles=; [ $api = gles ] && gles=GLF_GLES=1
      if ! env $gles GLF_DIR="$WORK/dir" "$WORK/gl_frames" "$CORE" "$WORK/test.gdi" "$WORK/$name.raw" 70 "$o" \
            > "$WORK/$name.log" 2>&1; then
         echo "  FAIL: the $name run failed:"; tail -5 "$WORK/$name.log"
         fails=$((fails + 1)); continue
      fi
      python3 tools/rend/clip_check.py "$WORK/$name.raw" || fails=$((fails + 1))
   done
done

if [ $fails != 0 ]; then
   echo "gl_clip: $fails failed"
   exit 1
fi
echo "gl_clip: all passed"
