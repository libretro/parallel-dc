#!/bin/sh
# The beam SPG_STATUS reads give, kept from read to read on a line by
# spg_beam(), against the beam found afresh at each (spg_beam_test.cpp):
# the test disc run on tools/threads/headless.c, straight and then with
# every frame run ahead (a state saved and loaded each frame). The core's
# own objects are linked in, so the core has to be built for this machine
# first (make platform=unix).
#
#   sh tools/pvr/spg_beam_test.sh [FRAMES]
#
# KEEP=1: the program is kept, in $TMPDIR/spg_beam_test.<pid>.
# Fails where a read's beam differs, or too few reads were made to tell.
set -e
cd "$(dirname "$0")/../.."
FRAMES=${1:-600}
WORK=${TMPDIR:-/tmp}/spg_beam_test.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
[ -f core/hw/pvr/spg.o ] || { echo "build the core first: core/hw/pvr/spg.o is not there" >&2; exit 1; }

CORE_FLAGS=$(make -n platform=unix -W core/hw/pvr/spg.c core/hw/pvr/spg.o 2>/dev/null | grep -m1 'core/hw/pvr/spg.c' \
   | sed -e 's/^[^ ]* //' -e 's/ -c / /' -e 's/ -MMD -MP .*$//' -e 's/-DGIT_VERSION=[^ ]* [^ ]*//')
OBJS=$(make -n platform=unix -W core/hw/pvr/spg.c 2>/dev/null | tail -1 | tr ' ' '\n' | grep '\.o$' | grep -v 'glslang.js.o')
${CC:-cc} -O1 -c -o "$WORK/headless.o" tools/threads/headless.c
${CXX:-g++} $CORE_FLAGS -w -c -o "$WORK/test.o" tools/pvr/spg_beam_test.cpp
${CXX:-g++} -rdynamic -o "$WORK/spg_beam_test" "$WORK/headless.o" "$WORK/test.o" $OBJS \
   -Wl,--wrap=dlopen -Wl,--wrap=_exit -Wl,--wrap=spg_sync -lGL -lrt -lpthread -lm -ldl
# an OpenGL that does nothing, for the core's renderer (headless.sh's)
nm -D --undefined-only "$WORK/spg_beam_test" | awk '{print $NF}' | sed 's/@.*//' | grep '^gl' \
   | python3 tools/threads/headless_gl.py > "$WORK/nogl.c" || true
${CC:-cc} -O1 -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o "$WORK/libGLESv2.so.2" "$WORK/nogl.c"
python3 tools/threads/live_disc.py "$WORK/test.gdi"

fail=0
for ahead in "" 10; do
   rm -rf "$WORK/dir"; mkdir -p "$WORK/dir"
   HEADLESS_AHEAD=$ahead LD_PRELOAD="$WORK/libGLESv2.so.2" HEADLESS_DIR="$WORK/dir" \
      "$WORK/spg_beam_test" self "$WORK/test.gdi" "$FRAMES" reicast_threaded_rendering=disabled \
      > "$WORK/out" 2> "$WORK/err" || true
   r=$(grep '^spg_beam_test:' "$WORK/out") || { echo "spg_beam_test: no result"; tail -5 "$WORK/err"; exit 1; }
   echo "${ahead:+run ahead from frame $ahead: }$r"
   grep 'spg_beam_test: read' "$WORK/err" || true
   # many reads to a line (a wait on the beam), and every one the same
   echo "$r" | awk '{ if ($6 != 0 || $2 < 100000 || $2 < 4 * $4) exit 1 }' || fail=1
done
[ $fail = 0 ] || { echo "spg_beam_test: FAIL: a read's beam kept from the last differs from the beam found afresh, or too few reads"; exit 1; }
echo "spg_beam_test: pass"
