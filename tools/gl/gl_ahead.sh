#!/bin/sh
# A state saved while a render to a texture is still on the graphics card,
# with OpenGL and OpenGL ES 3 on Mesa's surfaceless EGL (gl_frames.c), as
# tools/gl/gl_dma.sh has it. The test disc built with -DLIVE_KEEP
# (tools/threads/live_disc.py --keep) renders a picture to a texture once,
# at the very end of a frame, and from the next frame on only draws with
# it. Every frame run ahead (gl_frames GLF_AHEAD: a state saved, a frame
# run, the state loaded) has to show what a plain run shows, the picture
# among it: a state carries video memory as the console has it, renders
# that have not been fetched off the card included (tools/gl/ahead_check.py).
#
#   sh tools/gl/gl_ahead.sh [core.so]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/gl_ahead.<pid>.
# SANITIZE=address|undefined: the core is one built with DEBUG_ASAN=1 or
# DEBUG_UBSAN=1, and runs with its sanitizer's runtime; what it reports
# fails the run.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=${TMPDIR:-/tmp}/gl_ahead.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
python3 tools/threads/live_disc.py "$WORK/test.gdi" --keep
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
   echo "gl_ahead.sh: SANITIZE is address or undefined" >&2; exit 2 ;;
esac

# The picture is rendered in frame 82 and drawn from 83 on; the run ahead
# starts well before.
for api in gl gles; do
   echo "$api"
   # (GLF_GLES set at all asks for OpenGL ES)
   gles=; [ $api = gles ] && gles=GLF_GLES=1
   ok=1
   for run in plain ahead; do
      ahead=; frames=101
      [ $run = ahead ] && { ahead=GLF_AHEAD=5; frames=100; }
      rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
      if ! env $preload $gles $ahead GLF_DIR="$WORK/dir" "$WORK/gl_frames" "$CORE" "$WORK/test.gdi" "$WORK/$api-$run.raw" $frames \
            > "$WORK/$api-$run.log" 2>&1; then
         echo "  FAIL: the $api $run run failed:"; tail -5 "$WORK/$api-$run.log"
         ok=0
      fi
      if [ -n "$preload" ] && ls "$WORK"/san.* > /dev/null 2>&1; then
         echo "  FAIL: the $api $run run: the $SANITIZE sanitizer"; head -20 "$WORK"/san.*
         rm -f "$WORK"/san.*
         ok=0
      fi
   done
   if [ $ok = 1 ]; then
      python3 tools/gl/ahead_check.py "$WORK/$api-plain.raw" "$WORK/$api-ahead.raw" || fails=$((fails + 1))
   else
      fails=$((fails + 1))
   fi
done

if [ $fails != 0 ]; then
   echo "gl_ahead: $fails failed"
   exit 1
fi
echo "gl_ahead: all passed"
