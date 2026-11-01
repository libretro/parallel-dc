#!/bin/sh
# The core let go whole: the core built with AddressSanitizer (make
# DEBUG_ASAN=1) runs the test discs' frames with OpenGL (gl_frames.c, Mesa's
# surfaceless EGL) and the recompiler, is unloaded (retro_unload_game,
# retro_deinit) and the process ends as any does, its statics destroyed,
# and LeakSanitizer finds nothing left over. (A frontend that unloads a
# core with dlclose() destroys them too; tools/vulkan/vk_sanitize.sh's
# frontend ends with _Exit(), before them.) The recompiled blocks, which
# hold the blocks that jump to them, were lost there: two that jump to each
# other held each other.
#
#   sh tools/threads/unload_leaks.sh [core.so]
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-parallel_dc_libretro.so}")
WORK=${TMPDIR:-/tmp}/unload_leaks.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -o "$WORK/gl_frames" tools/gl/gl_frames.c -lEGL -lGL -ldl
RUNTIME=$(${CC:-cc} -print-file-name=libasan.so)
fails=0
for disc in plain clip; do
   if [ $disc = plain ]; then python3 tools/threads/live_disc.py "$WORK/$disc.gdi"
   else python3 tools/threads/live_disc.py "$WORK/$disc.gdi" --$disc; fi > /dev/null
   for sort in per-triangle per-pixel; do
      case $sort in
         per-triangle) o="reicast_alpha_sorting=per-triangle (normal)" ;;
         per-pixel) o="reicast_alpha_sorting=per-pixel (accurate)" ;;
      esac
      name=$disc-$sort
      echo "$name"
      rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
      # (the recompiler's memory faults are its own: ASan's handler is kept out of them)
      if ! ASAN_OPTIONS="detect_leaks=1:handle_segv=0:log_path=$WORK/$name.san" LD_PRELOAD="$RUNTIME" \
            GLF_DIR="$WORK/dir" "$WORK/gl_frames" "$CORE" "$WORK/$disc.gdi" "$WORK/$name.raw" 200 "$o" \
            > "$WORK/$name.log" 2>&1; then
         echo "  FAIL: the run failed:"; tail -5 "$WORK/$name.log"
         cat "$WORK/$name.san".* 2>/dev/null | grep -E "SUMMARY|ERROR" | head -3
         fails=$((fails + 1)); continue
      fi
      if cat "$WORK/$name.san".* > /dev/null 2>&1; then
         echo "  FAIL: LeakSanitizer:"
         cat "$WORK/$name.san".* | grep -E "SUMMARY|ERROR" | head -3
         fails=$((fails + 1))
      fi
   done
done
if [ $fails != 0 ]; then
   echo "unload_leaks: $fails failed"
   exit 1
fi
echo "unload_leaks: all let go"
