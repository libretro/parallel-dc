#!/bin/sh
# The Direct3D 12 renderer's raster shaders as DXIL where the device takes
# shader model 6.0, as DXBC where it does not (dx12/dx12_shaders.h,
# dx12_shaders_choose()): the core's Windows build under wine (vkd3d on
# lavapipe) on a frontend that runs the core's frames as RetroArch does
# (tools/hdr/d3d_hdr.c), the test discs, each translucent sort. Run as the
# device answers, the shaders are DXIL if, and only if, it says 6.0 (wine's
# vkd3d says 5.1 and takes no DXIL: a core that chose DXIL regardless draws
# nothing); run with the device saying 5.1 (D3DH_SM51), DXBC; and the
# pictures of the two the same.
#
#   sh tools/rend/d3d_dxil.sh [core.dll]
#
# KEEP=1: the pictures and logs are kept, in $TMPDIR/d3d_dxil.<pid>.
#
# Needs wine, Xvfb, mingw-w64 and a Vulkan driver (lavapipe:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json).
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.dll}")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/d3d_dxil.$$
mkdir -p "$WORK"
DISPLAY_NO=${D3D_DXIL_DISPLAY:-:96}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
${CC_WIN:-x86_64-w64-mingw32-gcc} -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c tools/rend/alloc_count.c -ld3d11 -ld3d12 -ldxguid
for disc in plain cull; do
   mkdir -p "$WORK/$disc"
   case $disc in
      plain) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" ;;
      *) python3 tools/threads/live_disc.py "$WORK/$disc/test.gdi" --$disc ;;
   esac
done
fails=0

# run <disc> <name> <options...>: 100 frames, from nothing kept by an earlier run
run() {
   run_disc=$1; run_name=$2; shift 2
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan VKD3D_CONFIG=virtual_heaps \
      D3DH_VERBOSE=1 D3DH_API=12 D3DH_FRONTEND=sdr D3DH_DIR="Z:$WORK/dir" WINEDLLOVERRIDES="winedbg.exe=d" \
      timeout 600 wine "$WORK/d3d_hdr.exe" "Z:$CORE" "Z:$WORK/$run_disc/test.gdi" "Z:$WORK/$run_name.raw" 100 "$@" \
      > "$WORK/$run_name.log" 2>&1 || { echo "  FAIL: the $run_name run failed:"; tail -5 "$WORK/$run_name.log"; return 1; }
}

for disc in plain cull; do
   for r in per-triangle per-strip per-pixel; do
      case $r in
         per-triangle) set -- "reicast_alpha_sorting=per-triangle (normal)" ;;
         per-strip) set -- "reicast_alpha_sorting=per-strip (fast, least accurate)" ;;
         per-pixel) set -- "reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      name="$disc-$r"
      run $disc "$name" "$@" || { fails=$((fails + 1)); continue; }
      D3DH_SM51=1 run $disc "$name-sm51" "$@" || { fails=$((fails + 1)); continue; }
      model=$(sed -n 's/^d3d_hdr: highest shader model \([0-9a-f]*\).*/\1/p' "$WORK/$name.log" | head -1)
      want="DXBC"
      [ -n "$model" ] && [ $((0x$model)) -ge $((0x60)) ] && want="DXIL"
      chose=$(sed -n 's/.*DX12: the shaders as \([A-Z]*\).*/\1/p' "$WORK/$name.log" | head -1)
      chose51=$(sed -n 's/.*DX12: the shaders as \([A-Z]*\).*/\1/p' "$WORK/$name-sm51.log" | head -1)
      echo "$name: the device says shader model ${model:-?}: $chose; said 5.1: $chose51"
      if [ "$chose" != "$want" ] || [ "$chose51" != "DXBC" ]; then
         echo "  FAIL: the shaders are not the ones the device's shader model calls for"
         fails=$((fails + 1))
      fi
      if grep -q "could not be made" "$WORK/$name.log" "$WORK/$name-sm51.log"; then
         echo "  FAIL: a pipeline could not be made with the shaders chosen"
         fails=$((fails + 1))
      fi
      if [ ! -s "$WORK/$name.raw" ] || ! cmp -s "$WORK/$name.raw" "$WORK/$name-sm51.raw"; then
         echo "  FAIL: the picture is not the DXBC one"
         fails=$((fails + 1))
      fi
   done
done
[ $fails -eq 0 ] || { echo "d3d_dxil: $fails failed"; exit 1; }
echo "d3d_dxil: all passed"
