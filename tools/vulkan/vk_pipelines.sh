#!/bin/sh
# The Vulkan renderers' pipelines made off the frontend's thread
# (core/rend/vulkan/pso.h), on a frontend that runs the core's frames as
# RetroArch does (vk_hdr.c, VKH_COUNT_PIPELINES: every vkCreate*Pipelines
# counted by the thread that calls it). For each renderer - per-strip,
# per-triangle, per-pixel, with and without SSAA, and compute:
#
#  - a first run, with no pipeline cache file: no pipeline is made on the
#    frontend's thread while the context is made, and it leaves the cache
#    file, with the keys of the polygons' pipelines made (the raster
#    renderers': the compute renderer draws its polygons with none);
#  - a second run, with the file: none made there either;
#  - a third, with the file's driver data spoiled (as a new driver would
#    find it): the keys are still taken (the core's log says how many it
#    loaded), none made there either;
#  - the three pictures are the same, to the byte.
#
# How many a frame made itself (the pipeline thread had not got to them)
# is shown; with VKP_STRICT=1 the second and third runs must have made
# none, which needs the frontend to wait before its first frame long
# enough for the pipeline thread to make them all (VKP_PAUSE ms, 5000 by
# default; lavapipe makes the compute renderer's kernels in seconds).
# VKP_ONLY=<a lane's name, as printed> runs that one alone.
#
#   sh tools/vulkan/vk_pipelines.sh [core.so]
#
# Needs the core built with Vulkan, a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json) and python3 for
# the test disc (tools/threads/live_disc.py).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=${TMPDIR:-/tmp}/vk_pipelines.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_NO_THREADS -o "$WORK/vk_hdr" tools/vulkan/vk_hdr.c tools/hdr/hdr_post_ref.c \
   core/rend/hdr/hdr_output.c core/rend/hdr/hdr_transform.c core/rend/hdr/hdr_aces2.c \
   core/rend/hdr/hdr_libretro.c core/rend/hdr/hdr_display.c -ldl -lm -lpthread
python3 tools/threads/live_disc.py "$WORK/test.gdi"
FRAMES=${VKP_FRAMES:-70}
PAUSE=${VKP_PAUSE:-5000}
fails=0

# run <dir> <name> <options...>: prints the counts' line
run() {
   dir=$1; name=$2; shift 2
   VKH_VERBOSE=1 VKH_DIR=$dir VKH_FRONTEND=sdr VKH_COUNT_PIPELINES=1 VKH_UNLOAD=1 VKH_LOAD_PAUSE_MS=$PAUSE \
      "$WORK/vk_hdr" "$CORE" "$WORK/test.gdi" "$dir/$name.raw" $FRAMES "$@" > "$dir/$name.log" 2>&1 || {
      { echo "  FAIL: the $name run failed:"; grep -v '^vk_hdr: pipeline' "$dir/$name.log" | tail -5; } >&2; return 1; }
   grep 'vk_hdr: pipelines: [0-9]' "$dir/$name.log" | sed 's/vk_hdr: pipelines: //'
}
# the cache file's keys (-1 if there is no file, or it is not one)
keys() {
   python3 - "$1" <<'EOF'
import struct, sys
try:
    d = open(sys.argv[1], 'rb').read(24)
except OSError:
    print(-1); sys.exit()
if len(d) < 24:
    print(-1); sys.exit()
magic, version, keys, size = struct.unpack('<8sIIQ', d)
print(keys if magic.rstrip(b'\0') == b'FCVKPSO' else -1)
EOF
}

# lane <label> <whether its keys are kept> <options...>
lane() {
   label=$1; keyed=$2; shift 2
   if [ -n "${VKP_ONLY:-}" ] && [ "$VKP_ONLY" != "$label" ]; then return 0; fi
   dir="$WORK/lane"; rm -rf "$dir"; mkdir -p "$dir"
   file="$dir/dc/vulkan_pipeline.cache"
   echo "$label"
   for name in cold warm driver; do
      if [ $name = driver ]; then
         if [ "$(keys "$file")" = -1 ]; then
            echo "  FAIL: no cache file to spoil the driver's data of"; fails=$((fails + 1)); continue
         fi
         # (the driver's data after the keys, spoiled: one byte of its header)
         python3 - "$file" "$(keys "$file")" <<'EOF'
import sys
f = open(sys.argv[1], 'r+b'); at = 24 + int(sys.argv[2]) * 8 + 16
f.seek(at); b = f.read(1); f.seek(at); f.write(bytes([b[0] ^ 0x5a]))
EOF
      fi
      line=$(run "$dir" $name "$@") || { fails=$((fails + 1)); continue; }
      echo "  $name: $line"
      load=$(echo "$line" | sed 's/ made on.*//')
      play=$(echo "$line" | sed 's/.*context was made, \([0-9]*\) in retro_run.*/\1/')
      if [ "$load" != 0 ]; then
         echo "  FAIL: $name made $load on the frontend's thread while the context was made"; fails=$((fails + 1))
      fi
      if [ "${VKP_STRICT:-0}" = 1 ] && [ $name != cold ] && [ "$play" != 0 ]; then
         echo "  FAIL: $name made $play in retro_run"; fails=$((fails + 1))
      fi
      n=$(keys "$file")
      if [ "$n" = -1 ]; then
         echo "  FAIL: $name left no pipeline cache file"; fails=$((fails + 1))
      elif [ $keyed = 1 ] && [ "$n" = 0 ]; then
         echo "  FAIL: $name left no keys in the cache file"; fails=$((fails + 1))
      fi
      loaded=$(sed -n 's/.*Vulkan pipeline cache loaded from .*, \([0-9]*\) pipeline keys.*/\1/p' "$dir/$name.log" | tail -1)
      if [ $name != cold ] && [ $keyed = 1 ] && [ "${loaded:-0}" = 0 ]; then
         echo "  FAIL: $name took no keys from the cache file"; fails=$((fails + 1))
      fi
   done
   for name in warm driver; do
      if ! cmp -s "$dir/cold.raw" "$dir/$name.raw"; then
         echo "  FAIL: the $name picture is not the first run's"; fails=$((fails + 1))
      fi
   done
}

lane "per-triangle" 1 "reicast_alpha_sorting=per-triangle (normal)"
lane "per-strip" 1 "reicast_alpha_sorting=per-strip (fast, least accurate)"
lane "per-pixel" 1 "reicast_alpha_sorting=per-pixel (accurate)"
lane "per-triangle, SSAA 4x" 1 "reicast_alpha_sorting=per-triangle (normal)" "reicast_ssaa=4x"
lane "per-pixel, SSAA 4x" 1 "reicast_alpha_sorting=per-pixel (accurate)" "reicast_ssaa=4x"
lane "compute" 0 "reicast_alpha_sorting=per-pixel (accurate)" "reicast_rendering_method=compute"

if [ $fails != 0 ]; then
   echo "vk_pipelines: $fails failed"
   exit 1
fi
echo "vk_pipelines: all passed"
