#!/bin/sh
# The parse's speed: the test disc built with -DLIVE_HEAVY (live_disc.py
# --heavy: about 14,000 vertices a frame) run on tools/threads/headless.c,
# every frame from the 100th parsed again and timed (ta_parse_bench.cpp).
# The core's own objects are linked in, so the core has to be built for
# this machine first (make platform=unix).
#
#   sh tools/pvr/ta_parse_bench.sh [FRAMES]
#
# PARSE_BENCH_REPEAT parses a frame (32), PARSE_BENCH_FROM the frame to
# start at (100). PARSE_BENCH_SUMS=1: a checksum of what each frame's
# parse made - its indices, vertices and polygons - a line, for two builds'
# to be compared. PARSE_BENCH_DISC: the disc's live_disc.py option
# (--heavy). KEEP=1: the program is kept, in $TMPDIR/ta_parse_bench.<pid>.
set -e
cd "$(dirname "$0")/../.."
FRAMES=${1:-160}
WORK=${TMPDIR:-/tmp}/ta_parse_bench.$$
mkdir -p "$WORK"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$WORK"' EXIT
[ -f core/hw/pvr/ta_vtx.o ] || { echo "build the core first: core/hw/pvr/ta_vtx.o is not there" >&2; exit 1; }

CORE_FLAGS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp core/hw/pvr/ta_vtx.o 2>/dev/null | grep -m1 'core/hw/pvr/ta_vtx.cpp' \
   | sed -e 's/^g++ //' -e 's/ -c / /' -e 's/ -MMD -MP .*$//' -e 's/-DGIT_VERSION=[^ ]* [^ ]*//')
OBJS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp 2>/dev/null | tail -1 | tr ' ' '\n' | grep '\.o$' | grep -v 'glslang.js.o')
${CC:-cc} -O1 -c -o "$WORK/headless.o" tools/threads/headless.c
${CXX:-g++} $CORE_FLAGS -w -c -o "$WORK/bench.o" tools/pvr/ta_parse_bench.cpp
${CXX:-g++} -fopenmp -rdynamic -o "$WORK/ta_parse_bench" "$WORK/headless.o" "$WORK/bench.o" $OBJS \
   -Wl,--wrap=dlopen -Wl,--wrap=_exit -Wl,--wrap=_Z13ta_parse_vdrcP10TA_context -lGL -lrt -lpthread -lm -ldl
# an OpenGL that does nothing, for the core's renderer (headless.sh's)
nm -D --undefined-only "$WORK/ta_parse_bench" | awk '{print $NF}' | sed 's/@.*//' | grep '^gl' \
   | python3 tools/threads/headless_gl.py > "$WORK/nogl.c" || true
${CC:-cc} -O1 -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o "$WORK/libGLESv2.so.2" "$WORK/nogl.c"
python3 tools/threads/live_disc.py "$WORK/test.gdi" ${PARSE_BENCH_DISC---heavy}
mkdir -p "$WORK/dir"
LD_PRELOAD="$WORK/libGLESv2.so.2" HEADLESS_DIR="$WORK/dir" "$WORK/ta_parse_bench" self "$WORK/test.gdi" "$FRAMES" \
   reicast_threaded_rendering=disabled > "$WORK/out" 2> "$WORK/err" || true
[ -z "${PARSE_BENCH_SUMS:-}" ] || grep '^sum' "$WORK/out"
grep '^ta_parse_bench' "$WORK/out" || { echo "ta_parse_bench: no result"; tail -5 "$WORK/err"; exit 1; }
