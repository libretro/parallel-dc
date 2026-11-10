#!/bin/sh
# Nothing made on the draw path, Vulkan (AGENTS.md): the core's frames run
# as RetroArch runs them (vk_hdr.c, VKH_COUNT_PIPELINES: every
# vkCreate*Pipelines counted by the thread that calls it), cold - no
# pipeline cache file, and the first frame straight after the context, as
# a frontend with no loading of its own gives it - and none may be made in
# retro_run, or by the thread that hands the frames over after its first.
#
#  - every renderer (per-triangle, per-strip, per-pixel, each with SSAA,
#    compute) on every test disc (plain, the texture atlas, culling, DMA
#    over renders to textures), threaded rendering on, and per-triangle
#    with it off;
#  - on the culling disc, the device without VK_EXT_extended_dynamic_state,
#    without _state3, and without both (VKH_HIDE_EXT): a general pipeline
#    for every fixed state a draw can reach is made at the start;
#  - HDR10 output with the post effects (made with the context);
#  - a change of renderer while the game runs (VKH_OPTS_AT: per-triangle,
#    per-pixel, compute, SSAA taken up), frames paced as a display has
#    them (VKH_FRAME_MS): the new one's pipelines made on the worker while
#    the frames show the kept picture, the game going on after (the core
#    says how many frames that took), and the last picture the one the new
#    options draw from the start.
#
# Allowed, and only shrinking (docs/ShaderStutterPlan.md): the device
# without either extension, where the states a draw can reach are too many
# to make at the start until the blend is folded - its counts are shown,
# not judged.
#
#   sh tools/vulkan/vk_draw_path.sh [core.so]
#
# Needs the core built with Vulkan, a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json) and python3 for
# the test discs (tools/threads/live_disc.py). KEEP=1: the logs are kept,
# in $TMPDIR/vk_draw_path.<pid>.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/vk_draw_path.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
# (each disc in its own directory: their tracks have the same names)
for d in plain atlas cull dma; do
   mkdir -p "$WORK/$d"
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d/$d.gdi" --$d; fi > /dev/null
done
EDS=VK_EXT_extended_dynamic_state
EDS3=VK_EXT_extended_dynamic_state3
TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
fails=0
# (a lane's own: set before a run, put back after it - an assignment in
# front of a function call stays set in some shells)
frames=60; opts_at=; frame_ms=

# run <name> <disc> <hidden extensions or -> <frontend> <judged: 1 or 0> <options...>
run() {
   name=$1; disc=$2; hide=$3; fe=$4; judged=$5; shift 5
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   if ! env $( [ "$hide" = - ] || echo VKH_HIDE_EXT=$hide ) ${opts_at:+"VKH_OPTS_AT=$opts_at"} \
         ${frame_ms:+VKH_FRAME_MS=$frame_ms} VKH_VERBOSE=1 VKH_DIR="$WORK/dir" VKH_FRONTEND=$fe \
         VKH_COUNT_PIPELINES=1 VKH_PIPELINE_CALLERS=1 VKH_UNLOAD=1 \
         timeout 900 "$WORK/vk_hdr" "$CORE" "$WORK/$disc/$disc.gdi" "$WORK/$name.raw" $frames "$@" \
         > "$WORK/$name.log" 2>&1; then
      echo "$name"; echo "  FAIL: the run failed:"; grep -v 'pipeline asked for' "$WORK/$name.log" | tail -5
      fails=$((fails + 1)); return 0
   fi
   play=$(sed -n "s/.* \([0-9]*\) in retro_run.*/\1/p" "$WORK/$name.log")
   present=$(sed -n 's/.*hands the frames over after its first frame: \([0-9]*\).*/\1/p' "$WORK/$name.log")
   echo "$name: ${play:-?} made in retro_run, ${present:-?} by the presenting thread after its first frame"
   if [ -z "$play" ] || [ -z "$present" ]; then
      echo "  FAIL: no counts"; fails=$((fails + 1)); return 0
   fi
   if [ "$play" != 0 ] || [ "$present" != 0 ]; then
      grep 'pipeline asked for' "$WORK/$name.log" | grep -v '(context' | head -5 | sed 's/^vk_hdr: /  /'
      if [ $judged = 1 ]; then
         echo "  FAIL: made on the draw path"; fails=$((fails + 1))
      else
         echo "  (allowed: docs/ShaderStutterPlan.md)"
      fi
   fi
}

for d in plain atlas cull dma; do
   run $d-tri $d - sdr 1 "$TRI"
   run $d-tri-nothread $d - sdr 1 "$TRI" reicast_threaded_rendering=disabled
   run $d-strip $d - sdr 1 "$STRIP"
   run $d-pix $d - sdr 1 "$PIX"
   run $d-tri-ssaa $d - sdr 1 "$TRI" reicast_ssaa=4x
   run $d-pix-ssaa $d - sdr 1 "$PIX" reicast_ssaa=4x
   run $d-compute $d - sdr 1 "$PIX" reicast_rendering_method=compute
done
for hide in $EDS $EDS3 $EDS,$EDS3; do
   judged=1
   [ $hide != $EDS,$EDS3 ] || judged=0
   short=$(echo $hide | sed "s/$EDS3/eds3/; s/$EDS/eds/")
   run cull-tri-no-$short cull $hide sdr $judged "$TRI"
   run cull-pix-no-$short cull $hide sdr $judged "$PIX"
   run cull-tri-ssaa-no-$short cull $hide sdr $judged "$TRI" reicast_ssaa=4x
done
# switch <name> <option set at frame 20> <the option before>: 180 frames,
# against the new options from the start for as many frames as the
# machine ran - 180 less those that showed the kept picture, the machine
# not running - so both end at the same frame of the program (whose
# picture changes as it goes: its palette banks at its frames 90 and 100)
switch() {
   sw=$1; at=$2; from=$3
   frames=180; opts_at="20:$at"; frame_ms=16
   run $sw cull - sdr 1 "$from"
   opts_at=; frame_ms=
   held=$(sed -n 's/.*: \([0-9]*\) frames showed the kept picture.*/\1/p' "$WORK/$sw.log")
   echo "  ${held:-0} frames showed the kept picture"
   if [ "${held:-0}" -ge 150 ]; then
      echo "  FAIL: the new renderer's pipelines were never made"; fails=$((fails + 1))
   fi
   frames=$((180 - ${held:-0}))
   if [ "${at%%=*}" = "${from%%=*}" ]; then
      run $sw.new cull - sdr 1 "$at" > /dev/null
   else
      run $sw.new cull - sdr 1 "$from" "$at" > /dev/null
   fi
   frames=60
   if ! cmp -s "$WORK/$sw.raw" "$WORK/$sw.new.raw"; then
      echo "  FAIL: not the picture the new options draw"; fails=$((fails + 1))
   fi
}
switch cull-tri-to-pix "$PIX" "$TRI"
switch cull-pix-to-tri "$TRI" "$PIX"
switch cull-tri-to-compute reicast_rendering_method=compute "$TRI"
switch cull-pix-to-compute reicast_rendering_method=compute "$PIX"
switch cull-tri-to-ssaa reicast_ssaa=4x "$TRI"
switch cull-pix-to-ssaa reicast_ssaa=4x "$PIX"
run cull-tri-hdr cull - hdr10 1 "$TRI" reicast_color_format=30bit_hdr reicast_hdr_bloom=standard reicast_hdr_chromatic=subtle
run cull-pix-hdr cull - hdr10 1 "$PIX" reicast_color_format=30bit_hdr reicast_hdr_bloom=standard reicast_hdr_post_compact=enabled

if [ $fails != 0 ]; then
   echo "vk_draw_path: $fails failed"
   exit 1
fi
echo "vk_draw_path: all passed"
