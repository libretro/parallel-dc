#!/bin/sh
# Allocations during play (AGENTS.md, "No allocations during play") with
# OpenGL: the core under gl_frames.c (with tools/rend/alloc_count.c, built
# with -DALLOC_COUNT: GLF_ALLOCS, GLF_ALLOC_SITES), the test discs, each
# OpenGL renderer (per-triangle, per-strip, per-pixel), threaded rendering
# on and off, an OpenGL ES 3 context too, from frame 20 of 100 on; the
# options that change what is drawn (texture upscaling, renders to a
# texture finer, frame skipping and the rest); and with
# run-ahead (GLF_AHEAD: a state saved and loaded every frame), the plain
# disc and the one with a render to a texture in its state. Each place in
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
   if ! env GLF_DIR="$WORK/dir" GLF_ALLOCS=20 GLF_ALLOC_SITES=1 \
         ${PIN:-} timeout 900 "$WORK/gl_frames" "$CORE" "$WORK/$disc/$disc.gdi" "$WORK/$name.raw" 100 "$@" \
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
   GLF_GLES=1 run $d-tri-gles $d "$TRI"
done
# the options that change what is drawn, or at what size: the plain disc,
# per-triangle (texture upscaling at 6x is left out: its textures are of
# sizes no texture of the pool's has)
# (on one processor: xBRZ's work on this thread, with no OpenMP team)
PIN="taskset -c 0"
run plain-texup2 plain "$TRI" reicast_texupscale=2x
PIN=
run plain-texup4 plain "$TRI" reicast_texupscale=4x
run plain-rtt2 plain "$TRI" reicast_render_to_texture_upscaling=2x
run plain-rtt4 plain "$PIX" reicast_render_to_texture_upscaling=4x
run plain-skip2 plain "$TRI" reicast_frame_skipping=2
run plain-gtdoff plain "$TRI" reicast_gpu_texture_decode=disabled
run plain-gtdoff-pix plain "$PIX" reicast_gpu_texture_decode=disabled
GLF_GLES=1 run plain-gtdoff-gles plain "$TRI" reicast_gpu_texture_decode=disabled
run plain-res2 plain "$TRI" reicast_internal_resolution=2x
run plain-aniso plain "$TRI" reicast_anisotropic_filtering=16
run plain-pvr2 plain "$TRI" reicast_pvr2_scaler=enabled
run plain-vert plain "$TRI" reicast_screen_rotation=vertical
run plain-vmu plain "$TRI" reicast_vmu1_screen_display=enabled
run plain-cross plain "$TRI" reicast_lightgun1_crosshair=White
GLF_GLES=1 run plain-texup2-gles plain "$TRI" reicast_texupscale=2x
GLF_GLES=1 run plain-rtt2-gles plain "$TRI" reicast_render_to_texture_upscaling=2x
# with run-ahead (a state saved and loaded every frame): the plain disc, and
# the one whose state has a render to a texture on the card in it
for d in plain keep; do
   GLF_AHEAD=10 run $d-tri-ahead $d "$TRI"
   GLF_AHEAD=10 run $d-pix-ahead $d "$PIX"
   GLF_AHEAD=10 GLF_GLES=1 run $d-tri-gles-ahead $d "$TRI"
done
python3 tools/vulkan/vk_alloc_sites.py "$CORE" tools/gl/gl_allocs.allowed "$WORK"/*.log || {
   fails=$((fails + 1))
   # (and which of the pool's shapes ran out, where one did)
   grep -h "image left" "$WORK"/*.log | sort | uniq -c || true
}
[ $fails -eq 0 ] || { echo "gl_allocs: $fails failed"; exit 1; }
echo "gl_allocs: no allocation site that is not on the list"
