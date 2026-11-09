#!/bin/sh
# Nothing compiled on the draw path, OpenGL (AGENTS.md): the core's frames
# on a frontend that draws with a real OpenGL and no screen (gl_frames.c,
# Mesa's surfaceless EGL), cold - nothing kept from an earlier run - and
# no glCompileShader, glLinkProgram or glProgramBinary called inside
# retro_run (GLF_COUNT_PROGRAMS): a key first drawn is drawn with the
# general program.
#
#  - OpenGL ES 2 and OpenGL 2 (GLF_VERSION: the context says it is one of
#    them, and the core takes their paths, a general program whose key is
#    four nibbles in floats), OpenGL ES 3 and OpenGL 3+;
#  - each test disc (plain, the texture atlas, culling, DMA over renders to
#    textures), per triangle and per strip, threaded rendering on and off;
#    per pixel (OpenGL 4) on OpenGL;
#  - and, with GL_PROGRAM_TEST_GENERAL built in (GENERAL=core.so), each
#    picture the one the core draws with the general programs alone.
#
# The driver is one that compiles in the background (GLF_SLOW_COMPILE: a
# program's first completion status is "not done"), where a key's own
# program used to be asked for in the frame that first drew it.
#
#   sh tools/gl/gl_draw_path.sh [core.so]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/gl_draw_path.<pid>.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
[ -z "${GENERAL:-}" ] || GENERAL=$(readlink -f "$GENERAL")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/gl_draw_path.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
# (-rdynamic: GLF_VERSION's glGetString and glGetIntegerv are the core's)
${CC:-cc} -O1 -g -Wall -rdynamic -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
# (a directory a disc: their tracks' names are the same)
for d in plain atlas cull dma; do
   mkdir -p "$WORK/$d"
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi" --$d; fi > /dev/null
done
TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
fails=0

# run <name> <disc> <gles2|gl2|gles3|gl3> <options...>: 60 frames, from nothing kept
run() {
   name=$1; disc=$2; v=$3; shift 3
   case $v in
      gles2) env="GLF_GLES=1 GLF_VERSION=gles2" ;;
      gl2) env="GLF_VERSION=gl2" ;;
      gles3) env="GLF_GLES=1" ;;
      *) env="" ;;
   esac
   for core in "$CORE" ${GENERAL:+"$GENERAL"}; do
      out=$name; [ "$core" = "$CORE" ] || out=$name.general
      rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
      if ! env $env GLF_SLOW_COMPILE=1 GLF_COUNT_PROGRAMS=1 GLF_VERBOSE=1 GLF_DIR="$WORK/dir" \
            timeout 600 "$WORK/gl_frames" "$core" "$WORK/$disc/$disc.gdi" "$WORK/$out.raw" 60 "$@" \
            > "$WORK/$out.log" 2>&1; then
         echo "$name"; echo "  FAIL: the run failed:"; tail -5 "$WORK/$out.log"
         fails=$((fails + 1)); return 0
      fi
   done
   counts=$(sed -n 's/^programs: //p' "$WORK/$name.log")
   echo "$name: ${counts:-no counts}"
   case "$counts" in
      "0 compiled, 0 linked, 0 loaded in retro_run") ;;
      *) echo "  FAIL: compiled on the draw path"; fails=$((fails + 1)) ;;
   esac
   case $v in
      gles2) want="OpenGL version: OpenGL ES 2" ;;
      gl2) want="OpenGL version: 2\." ;;
      gles3) want="OpenGL version: OpenGL ES 3" ;;
      *) want="OpenGL version: [34]\." ;;
   esac
   if ! grep -q "$want" "$WORK/$name.log"; then
      echo "  FAIL: the context was not taken for $v"; fails=$((fails + 1))
   fi
   if [ -n "${GENERAL:-}" ] && ! cmp -s "$WORK/$name.raw" "$WORK/$name.general.raw"; then
      echo "  FAIL: not the picture of the general programs"; fails=$((fails + 1))
   fi
}

for v in gles2 gl2 gles3 gl3; do
   for d in plain atlas cull dma; do
      run $v-$d-tri $d $v "$TRI"
      run $v-$d-strip $d $v "$STRIP"
   done
   run $v-cull-tri-nothread cull $v "$TRI" reicast_threaded_rendering=disabled
done
for d in plain cull; do
   run gl3-$d-pix $d gl3 "$PIX"
done

if [ $fails != 0 ]; then
   echo "gl_draw_path: $fails failed"
   exit 1
fi
echo "gl_draw_path: all passed"
