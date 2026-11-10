#!/bin/sh
# Allocations during play (AGENTS.md, "No allocations during play"): the
# core under a Vulkan frontend as RetroArch runs it (vk_hdr.c with
# tools/rend/alloc_count.c, built with -DALLOC_COUNT: VKH_ALLOCS,
# VKH_ALLOC_SITES), the test discs, every
# Vulkan renderer (per-triangle, threaded rendering on and off, per-strip,
# per-pixel, compute), from frame 20 of 100 on; the options that change
# what is drawn (texture upscaling, renders to a texture finer, frame
# skipping, the internal resolution, SSAA and the rest); the HDR output with
# its post effects; and with run-ahead
# (VKH_AHEAD: a state saved and loaded every frame), the plain disc and the
# one with a render to a texture in its state. Each place in the core that
# allocated - the heap, or Vulkan memory, buffers, images and pools - is
# named by its function (vk_alloc_sites.py) and must be on
# tools/vulkan/vk_allocs.allowed. That list only ever shrinks: a commit
# that removes a site takes it off.
#
#   sh tools/vulkan/vk_allocs.sh [core.so]
#
# KEEP=1: the logs are kept, in $TMPDIR/vk_allocs.<pid>. Needs python3,
# addr2line and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/vk_allocs.$$
mkdir -p "$WORK"
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -DALLOC_COUNT -rdynamic -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/rend/alloc_count.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
# (each disc in its own directory: their tracks have the same names)
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
   if ! env VKH_DIR="$WORK/dir" VKH_FRONTEND=${FE:-sdr} VKH_ALLOCS=20 VKH_ALLOC_SITES=1 \
         ${PIN:-} timeout 900 "$WORK/vk_hdr" "$CORE" "$WORK/$disc/$disc.gdi" "$WORK/$name.raw" 100 "$@" \
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
   run $d-compute $d "$PIX" reicast_rendering_method=compute
done
# the options that change what is drawn, or at what size: the plain disc,
# per-triangle (texture upscaling at 6x and renders to a texture at 3x and
# 8x are left out: their images are of sizes no image of the pool's has).
# Under the validation layer a frame skipped is the framebuffer, handed
# over at its own size and read back as that size.
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
run plain-res2 plain "$TRI" reicast_internal_resolution=2x
run plain-ssaa2 plain "$TRI" reicast_ssaa=2x
run plain-aniso plain "$TRI" reicast_anisotropic_filtering=16
run plain-pvr2 plain "$TRI" reicast_pvr2_scaler=enabled
run plain-vert plain "$TRI" reicast_screen_rotation=vertical
run plain-vmu plain "$TRI" reicast_vmu1_screen_display=enabled
run plain-cross plain "$TRI" reicast_lightgun1_crosshair=White
run plain-tagpu plain "$TRI" reicast_ta_processing=gpu
run plain-texdbg plain "$TRI" reicast_texture_debug=enabled
# the HDR output to an HDR10 and an scRGB frontend, with the post effects
# (bloom, halation, chromatic aberration): the plain disc's frames are the
# framebuffer it writes and its renders, of two sizes, in turn
FE=hdr10
run plain-hdr10 plain "$TRI" reicast_color_format=30bit_hdr
run plain-hdr10-post plain "$TRI" reicast_color_format=30bit_hdr reicast_hdr_bloom=standard \
   reicast_hdr_halation=subtle reicast_hdr_chromatic=subtle
run plain-hdr10-post-compute plain "$PIX" reicast_rendering_method=compute reicast_color_format=30bit_hdr \
   reicast_hdr_bloom=intense
FE=scrgb
run plain-scrgb-post plain "$PIX" reicast_color_format=64bit_scrgb reicast_hdr_bloom=standard
FE=
# with run-ahead (a state saved and loaded every frame): the plain disc, and
# the one whose state has a render to a texture on the card in it
for d in plain keep; do
   VKH_AHEAD=10 run $d-tri-ahead $d "$TRI"
   VKH_AHEAD=10 run $d-pix-ahead $d "$PIX"
   VKH_AHEAD=10 run $d-compute-ahead $d "$PIX" reicast_rendering_method=compute
done
python3 tools/vulkan/vk_alloc_sites.py "$CORE" tools/vulkan/vk_allocs.allowed "$WORK"/*.log || {
   fails=$((fails + 1))
   # (and which of the pool's shapes ran out, where one did)
   grep -h "image left" "$WORK"/*.log | sort | uniq -c || true
}
[ $fails -eq 0 ] || { echo "vk_allocs: $fails failed"; exit 1; }
echo "vk_allocs: no allocation site that is not on the list"
