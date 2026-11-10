#!/bin/sh
# Allocations during play (AGENTS.md, "No allocations during play") with
# OpenGL: the core under gl_frames.c (with tools/rend/alloc_count.c, built
# with -DALLOC_COUNT: GLF_ALLOCS, GLF_ALLOC_SITES), the test discs, each
# OpenGL renderer (per-triangle, per-strip, per-pixel), threaded rendering
# on and off, an OpenGL ES 3 context too, from frame 20 on. Each place in
# the core that allocated - the heap, or OpenGL names, storage, programs
# and shaders - is named by its function (tools/vulkan/vk_alloc_sites.py)
# and must be on tools/gl/gl_allocs.allowed, which only ever shrinks.
#
#   sh tools/gl/gl_allocs.sh [core.so]
#
# KEEP=1: the logs are kept, in $TMPDIR/gl_allocs.<pid>. Needs python3,
# addr2line and Mesa's surfaceless EGL (llvmpipe).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/gl_allocs.$$
mkdir -p "$WORK"
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -DALLOC_COUNT -rdynamic -o "$WORK/gl_frames" tools/gl/gl_frames.c tools/rend/alloc_count.c \
   -lEGL -lGL -ldl
for d in plain atlas cull dma blend; do
   mkdir -p "$WORK/$d"
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi" --$d; fi > /dev/null
done
TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
fails=0

# run <name> <disc> <options...>: 80 frames, counted from the 20th
run() {
   name=$1; disc=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   if ! env GLF_DIR="$WORK/dir" GLF_ALLOCS=20 GLF_ALLOC_SITES=1 \
         timeout 900 "$WORK/gl_frames" "$CORE" "$WORK/$disc/$disc.gdi" "$WORK/$name.raw" 80 "$@" \
         > "$WORK/$name.log" 2>&1; then
      echo "  FAIL: $name: the run failed:"; tail -5 "$WORK/$name.log"
      fails=$((fails + 1))
      return 0
   fi
   echo "$name:$(sed -n 's/.*during play (frames 20 to 79)://p' "$WORK/$name.log")"
}

for d in plain atlas cull dma blend; do
   run $d-tri $d "$TRI"
   run $d-tri-nothread $d "$TRI" reicast_threaded_rendering=disabled
   run $d-strip $d "$STRIP"
   run $d-pix $d "$PIX"
   GLF_GLES=1 run $d-tri-gles $d "$TRI"
done
python3 tools/vulkan/vk_alloc_sites.py "$CORE" tools/gl/gl_allocs.allowed "$WORK"/*.log || fails=$((fails + 1))
[ $fails -eq 0 ] || { echo "gl_allocs: $fails failed"; exit 1; }
echo "gl_allocs: no allocation site that is not on the list"
