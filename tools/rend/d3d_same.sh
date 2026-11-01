#!/bin/sh
# Two Windows builds of the core drawing the same: under wine, with Direct3D
# 9 every frame each hands over (tools/rend/d3d9_frames.c: checksums, frame
# for frame), and with Direct3D 11 and 12 the last picture
# (tools/hdr/d3d_hdr.c) - the test discs (tools/threads/live_disc.py:
# plain, the texture atlas, culling, DMA over renders to textures), each
# translucent sort Direct3D has (per triangle and per strip; per pixel with
# 11 and 12), threaded rendering on and off. For a change that should draw
# nothing differently, against the build before it.
#
#   sh tools/rend/d3d_same.sh before.dll after.dll
#
# KEEP=1: the logs and pictures are kept, in $TMPDIR/d3d_same.<pid>.
# Direct3D 9's shaders need a compiler for shader model 3, which wine's own
# d3dcompiler is not yet: D3DCOMPILER=path/to/d3dcompiler_47.dll (Microsoft's)
# is put beside the frontend and used; without it, Direct3D 9 is left out
# where its shaders cannot be made, and said so.
# Needs mingw-w64, wine, Xvfb, python3 and a Vulkan driver (lavapipe will
# do), as tools/hdr/d3d_hdr.sh.
set -e
cd "$(dirname "$0")/../.."
[ $# -eq 2 ] || { echo "usage: $0 before.dll after.dll" >&2; exit 2; }
A=$(readlink -f "$1"); B=$(readlink -f "$2")
WORK=$(readlink -f "${TMPDIR:-/tmp}")/d3d_same.$$
mkdir -p "$WORK"
DISPLAY_NO=${D3D_SAME_DISPLAY:-:93}
Xvfb $DISPLAY_NO -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
if [ -n "${KEEP:-}" ]; then trap 'kill $XVFB 2>/dev/null' EXIT; else trap 'kill $XVFB 2>/dev/null; rm -rf "$WORK"' EXIT; fi
CC_WIN=${CC_WIN:-x86_64-w64-mingw32-gcc}
$CC_WIN -std=c89 -O1 -Wall -I./core/libretro-common/include -o "$WORK/d3d9_frames.exe" tools/rend/d3d9_frames.c -ld3d9
OVERRIDES="winedbg.exe=d"
if [ -n "${D3DCOMPILER:-}" ]; then
   cp "$D3DCOMPILER" "$WORK/d3dcompiler_47.dll"
   OVERRIDES="$OVERRIDES;d3dcompiler_47=n"
fi
$CC_WIN -O1 -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/d3d_hdr.exe" tools/hdr/d3d_hdr.c -ld3d11 -ld3d12 -ldxguid
for d in plain atlas cull dma; do
   if [ $d = plain ]; then python3 tools/threads/live_disc.py "$WORK/$d.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$d.gdi" --$d; fi > /dev/null
done
FRAMES=100
fails=0
skipped=0

# wine_run <log> <exe> <args...>: from nothing kept by an earlier run (the disc writes to the memory card)
wine_run() {
   log=$1; shift
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   env -u TMPDIR DISPLAY=$DISPLAY_NO WINEDEBUG=-all WINE_D3D_CONFIG=renderer=vulkan VKD3D_CONFIG=virtual_heaps \
      WINEDLLOVERRIDES="$OVERRIDES" D3D9F_DIR="Z:$WORK/dir" D3DH_DIR="Z:$WORK/dir" \
      timeout 600 wine "$@" > "$log" 2> "$log.err"
}

TRI="reicast_alpha_sorting=per-triangle (normal)"
STRIP="reicast_alpha_sorting=per-strip (fast, least accurate)"
PIX="reicast_alpha_sorting=per-pixel (accurate)"
for d in plain atlas cull dma; do
   for m in tri strip tri-nothread; do
      case $m in tri) set -- "$TRI";; strip) set -- "$STRIP";; tri-nothread) set -- "$TRI" reicast_threaded_rendering=disabled;; esac
      name=d3d9-$d-$m
      [ $skipped -eq 0 ] || continue
      for c in A B; do
         core=$A; [ $c = B ] && core=$B
         wine_run "$WORK/$name.$c.log" "$WORK/d3d9_frames.exe" "Z:$core" "Z:$WORK/$d.gdi" "Z:$WORK/$name.$c.raw" $FRAMES "$@" \
            || true
      done
      if grep -q "DX9: no shaders" "$WORK/$name.A.log.err" "$WORK/$name.B.log.err"; then
         echo "  Direct3D 9 left out: no compiler makes its shaders here (D3DCOMPILER)"
         skipped=1
         continue
      fi
      n=$(grep -c "^frame [0-9]* [0-9]*x" "$WORK/$name.A.log" || true)
      if [ "$n" -gt 0 ] && cmp -s "$WORK/$name.A.log" "$WORK/$name.B.log"; then
         echo "same  $name ($n frames)"
      else
         echo "  FAIL: $name: not the same frames ($n read back)"; fails=$((fails + 1))
      fi
   done
   for api in 11 12; do
      for m in tri strip pix; do
         case $m in tri) set -- "$TRI";; strip) set -- "$STRIP";; pix) set -- "$PIX";; esac
         name=d3d$api-$d-$m
         for c in A B; do
            core=$A; [ $c = B ] && core=$B
            D3DH_API=$api wine_run "$WORK/$name.$c.log" "$WORK/d3d_hdr.exe" "Z:$core" "Z:$WORK/$d.gdi" "Z:$WORK/$name.$c.raw" $FRAMES "$@" \
               || { echo "  FAIL: $name ($c) did not run:"; tail -3 "$WORK/$name.$c.log.err"; fails=$((fails + 1)); }
         done
         if [ -s "$WORK/$name.A.raw" ] && cmp -s "$WORK/$name.A.raw" "$WORK/$name.B.raw"; then
            echo "same  $name"
         else
            echo "  FAIL: $name: not the same picture"; fails=$((fails + 1))
         fi
      done
   done
done
[ $fails -eq 0 ] || { echo "d3d_same: $fails failed"; exit 1; }
if [ $skipped -eq 0 ]; then echo "d3d_same: all the same"; else echo "d3d_same: all the same (Direct3D 9 left out)"; fi
