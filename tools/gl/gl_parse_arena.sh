#!/bin/sh
# A frame's vertices and indices parsed straight into buffers the card
# reads (core/rend/gles/parse_arena_gl.h), not copied there after, with
# OpenGL on a frontend that draws with a real OpenGL and no screen
# (gl_frames.c, Mesa's surfaceless EGL): every render the CPU parses is
# drawn from where it was parsed, with no OpenGL error (MESA_DEBUG), and
# every frame is the one of the lists copied, as an OpenGL without
# ARB_buffer_storage has it (GLF_NO_STORAGE) - the test discs' plain one,
# heavy one, DMA one and the one with modifier volumes (--cull: their
# triangles drawn from there too), the sorted passes' indices written after
# the frame's, with each renderer, threaded rendering on and off,
# a VMU's screen drawn over them.
# And with OpenGL ES, which has no such buffers, the frames are drawn as
# ever.
#
#   sh tools/gl/gl_parse_arena.sh [core.so]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/gl_parse_arena.<pid>.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=${TMPDIR:-/tmp}/gl_parse_arena.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
# (a directory a disc: their tracks' names are the same)
for disc in plain heavy dma cull; do
   mkdir -p "$WORK/$disc"
   case $disc in
      plain) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" ;;
      *) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" --$disc ;;
   esac
done
fails=0

# run <disc> <name> <options...>: 100 frames, every one's sum printed, from
# nothing kept by an earlier run (the discs write to the memory card)
run() {
   run_disc=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   MESA_DEBUG=1 GLF_VERBOSE=1 GLF_EVERY=1 GLF_DIR="$WORK/dir" "$WORK/gl_frames" "$CORE" "$WORK/$run_disc/test.gdi" \
      "$WORK/$run_name.raw" 100 "$@" > "$WORK/$run_name.log" 2>&1 || {
      { echo "  FAIL: the $run_name run failed:"; tail -5 "$WORK/$run_name.log"; } >&2; return 1; }
   # (the extensions read till there are none: the one past the last asked for)
   if grep "Mesa: error\|Mesa: User error" "$WORK/$run_name.log" | grep -qv "in glGetStringi(index="; then
      { echo "  FAIL: $run_name: an OpenGL error:"; grep -m3 "Mesa: error\|Mesa: User error" "$WORK/$run_name.log"; } >&2
      return 1
   fi
}

for disc in plain heavy dma cull; do
   for r in per-triangle per-pixel; do
      case $r in
         per-triangle) set -- "reicast_alpha_sorting=per-triangle (normal)" ;;
         per-pixel) set -- "reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      # (a VMU's screen over the picture: a quad of the renderer's own, drawn
      # in the frame, through its own buffers and not the frame's)
      set -- "$@" reicast_vmu1_screen_display=enabled
      for threaded in enabled disabled; do
         name="$disc-$r-$threaded"
         echo "$name"
         run $disc "$name" "$@" reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
         GLF_NO_STORAGE=1 run $disc "$name-copied" "$@" reicast_threaded_rendering=$threaded \
            || { fails=$((fails + 1)); continue; }
         if ! grep -q "renders parsed where the card reads them" "$WORK/$name.log" \
               || grep -q "a render copied to the card" "$WORK/$name.log"; then
            echo "  FAIL: not every render parsed where the card reads it"
            fails=$((fails + 1))
         fi
         if grep -q "renders parsed where the card reads them" "$WORK/$name-copied.log"; then
            echo "  FAIL: with no ARB_buffer_storage, the parse is still put there"
            fails=$((fails + 1))
         fi
         if ! cmp -s "$WORK/$name.raw" "$WORK/$name-copied.raw" \
               || [ "$(grep '^frame ' "$WORK/$name.log")" != "$(grep '^frame ' "$WORK/$name-copied.log")" ]; then
            echo "  FAIL: the frames are not those of the lists copied"
            fails=$((fails + 1))
         fi
      done
   done
done

# OpenGL ES: none of it, the frames drawn as ever
echo "gles"
if GLF_GLES=1 run heavy gles "reicast_alpha_sorting=per-triangle (normal)"; then
   if grep -q "renders parsed where the card reads them" "$WORK/gles.log"; then
      echo "  FAIL: OpenGL ES has no such buffers, and the parse was put in some"
      fails=$((fails + 1))
   fi
else
   fails=$((fails + 1))
fi

if [ $fails != 0 ]; then
   echo "gl_parse_arena: $fails failed"
   exit 1
fi
echo "gl_parse_arena: all passed"
