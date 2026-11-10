#!/bin/sh
# A frame's vertices, indices and modifier volumes' triangles parsed
# straight into memory the card reads (core/rend/dx12/parse_arena_d3d12.h),
# not copied into the upload ring after, with Direct3D 12: the core's
# Windows build under wine (vkd3d on lavapipe, whose adapter's memory is
# the processor's: CacheCoherentUMA), on a frontend that runs the core's
# frames as RetroArch does (tools/hdr/d3d_hdr.c). Every render the CPU
# parses is drawn from where it was parsed, and every frame is the one of
# the lists copied (D3DH_EVERY), as an adapter of its own has it (D3DH_NOT_UMA) - the
# test discs' plain one, heavy one (a volume far off the screen among it),
# DMA one and the one with modifier volumes, with each renderer, threaded
# rendering on and off.
#
#   sh tools/rend/d3d_parse_arena.sh [core.dll]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/d3d_parse_arena.<pid>.
#
# Needs wine, Xvfb, mingw-w64 and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.dll}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/d3d_parse_arena.$$
mkdir -p "$WORK"
DISPLAY_NO=${D3D_PARSE_ARENA_DISPLAY:-:94}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC_WIN:-x86_64-w64-mingw32-gcc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c tools/rend/alloc_count.c -ld3d11 -ld3d12 -ldxguid
# (a directory a disc: their tracks' names are the same)
for disc in plain heavy dma cull; do
   mkdir -p "$WORK/$disc"
   case $disc in
      plain) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" ;;
      *) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" --$disc ;;
   esac
done
fails=0

# run <disc> <name> <options...>: 100 frames, from nothing kept by an
# earlier run (the discs write to the memory card)
run() {
   run_disc=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan VKD3D_CONFIG=virtual_heaps \
      D3DH_EVERY=1 D3DH_VERBOSE=1 D3DH_API=12 D3DH_FRONTEND=sdr D3DH_DIR="Z:$WORK/dir" WINEDLLOVERRIDES="winedbg.exe=d" \
      timeout 600 wine "$WORK/d3d_hdr.exe" "Z:$CORE" "Z:$WORK/$run_disc/test.gdi" "Z:$WORK/$run_name.raw" 100 "$@" \
      > "$WORK/$run_name.log" 2>&1 || { echo "  FAIL: the $run_name run failed:"; tail -5 "$WORK/$run_name.log"; return 1; }
}

for disc in plain heavy dma cull; do
   for r in per-triangle per-pixel; do
      case $r in
         per-triangle) set -- "reicast_alpha_sorting=per-triangle (normal)" ;;
         per-pixel) set -- "reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      for threaded in enabled disabled; do
         name="$disc-$r-$threaded"
         echo "$name"
         run $disc "$name" "$@" reicast_threaded_rendering=$threaded || { fails=$((fails + 1)); continue; }
         D3DH_NOT_UMA=1 run $disc "$name-copied" "$@" reicast_threaded_rendering=$threaded \
            || { fails=$((fails + 1)); continue; }
         if ! grep -q "renders parsed where the card reads them" "$WORK/$name.log" \
               || grep -q "a render copied to the card" "$WORK/$name.log"; then
            echo "  FAIL: not every render parsed where the card reads it"
            fails=$((fails + 1))
         fi
         if grep -q "renders parsed where the card reads them" "$WORK/$name-copied.log"; then
            echo "  FAIL: with an adapter of its own, the parse is still put there"
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

if [ $fails != 0 ]; then
   echo "d3d_parse_arena: $fails failed"
   exit 1
fi
echo "d3d_parse_arena: all passed"
