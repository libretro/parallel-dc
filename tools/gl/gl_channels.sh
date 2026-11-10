#!/bin/sh
# Textures of one channel with OpenGL: the fog table and an 8-bit paletted
# texture's indices, GL_ALPHA on OpenGL ES 2 and OpenGL 2, GL_R8 on OpenGL
# ES 3 (where the texture pool, which has no GL_ALPHA, makes them) and
# OpenGL 3+. The plain test disc (tools/threads/live_disc.py) on Mesa's
# surfaceless EGL (gl_frames.c; GLF_VERSION for the older APIs), per
# triangle and per strip, has to show its polygon in fog half white and
# half green and its paletted texture blue (tools/rend/channel_check.py).
#
#   sh tools/gl/gl_channels.sh [core.so]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/gl_channels.<pid>.
# SANITIZE=address|undefined: the core is one built with DEBUG_ASAN=1 or
# DEBUG_UBSAN=1, and runs with its sanitizer's runtime; what it reports
# fails the run.
#
# Needs the core built with OpenGL, Mesa's EGL (libegl1-mesa-dev,
# libgl1-mesa-dri) and python3.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/gl_channels.$$
mkdir -p "$WORK"
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
# (-rdynamic: GLF_VERSION's glGetString and glGetIntegerv are the core's)
${CC:-cc} -O1 -g -Wall -rdynamic -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
python3 tools/threads/live_disc.py "$WORK/plain.gdi" > /dev/null
fails=0
preload=
case "${SANITIZE:-}" in
'') ;;
address)
   preload=LD_PRELOAD=$(${CC:-cc} -print-file-name=libasan.so)
   # (the recompiler's memory faults are its own: ASan's handler is kept out of them)
   export ASAN_OPTIONS="halt_on_error=1:detect_leaks=0:handle_segv=0:log_path=$WORK/san" ;;
undefined)
   preload=LD_PRELOAD=$(${CC:-cc} -print-file-name=libubsan.so)
   export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$WORK/san" ;;
*)
   echo "gl_channels.sh: SANITIZE is address or undefined" >&2; exit 2 ;;
esac

for api in gles2 gl2 gles3 gl; do
   case $api in
      gles2) env="GLF_GLES=1 GLF_VERSION=gles2" ;;
      gl2) env="GLF_VERSION=gl2" ;;
      gles3) env="GLF_GLES=1" ;;
      *) env= ;;
   esac
   for sort in tri strip; do
      case $sort in
         tri) opt="reicast_alpha_sorting=per-triangle (normal)" ;;
         *) opt="reicast_alpha_sorting=per-strip (fast, least accurate)" ;;
      esac
      name=$api-$sort
      mkdir -p "$WORK/$name"
      if ! env $preload $env GLF_DIR="$WORK/$name" timeout 900 "$WORK/gl_frames" "$CORE" "$WORK/plain.gdi" \
            "$WORK/$name.raw" 80 "$opt" > "$WORK/$name.log" 2>&1; then
         echo "  FAIL: $name: the run failed:"; tail -5 "$WORK/$name.log"
         fails=$((fails + 1))
         continue
      fi
      if [ -n "$preload" ] && ls "$WORK"/san.* > /dev/null 2>&1; then
         echo "  FAIL: $name: the $SANITIZE sanitizer"; head -20 "$WORK"/san.*
         rm -f "$WORK"/san.*
         fails=$((fails + 1))
         continue
      fi
      if python3 tools/rend/channel_check.py "$WORK/$name.raw"; then
         echo "$name: ok"
      else
         echo "  ($name)"; fails=$((fails + 1))
      fi
   done
done
[ $fails -eq 0 ] || { echo "gl_channels: $fails failed"; exit 1; }
echo "gl_channels: all passed"
