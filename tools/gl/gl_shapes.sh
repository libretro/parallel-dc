#!/bin/sh
# The OpenGL renderers compiled as the builds that reach OpenGL through
# libretro-common's glsm (HAVE_GLSYM_PRIVATE) have them, on Linux, with no
# SDK of theirs: glsm's functions are its own (glFenceSync returns a void *)
# and it has only those it wraps (no glFlush, say).
#
#   clang  as macOS and iOS build it: a void * is not made a GLsync, or
#          anything else, without a cast, -fpermissive or not
#   libnx  as the Switch builds it: GL from switch/nx_glsym.h, so a GL
#          function glsm does not wrap is not declared
#
#   sh tools/gl/gl_shapes.sh
#
# CLANGXX and CXX: the compilers (clang++ and g++ by default).
cd "$(dirname "$0")/../.."
CLANGXX=${CLANGXX:-clang++}
CXX=${CXX:-g++}
FLAGS="-I./core/libretro -I./core -I./core/deps -I./core/libretro-common/include
   -I./core/deps/vixl -I./core/deps/stb -I./core/deps/picotcp/include
   -I./core/deps/picotcp/modules -I./core/deps/khronos -D__LIBRETRO__
   -DHAVE_GLSYM_PRIVATE -DHAVE_THREADS -DHAVE_TEXUPSCALE -DHAVE_OPENGL
   -DTARGET_NO_OPENMP -DHAVE_CHD -std=c++11 -fno-rtti -fpermissive
   -fno-operator-names -fsyntax-only"
# the renderers' sources, and what else draws with OpenGL (libretro.cpp
# wants the platform's own headers, and is built by the other lanes)
FILES=$( (ls core/rend/gles/*.cpp core/rend/gl4/*.cpp
          grep -rlE '#include.*(gles\.h|gl4\.h|glcache\.h)' core --include=*.cpp) \
   | grep -v -E '^core/(deps|libretro-common)/|^core/libretro/libretro\.cpp$' | sort -u)
fails=0

for f in $FILES; do
   # (the per-pixel renderer is desktop OpenGL only: not on the Switch)
   if ! $CLANGXX $FLAGS -DHAVE_GL4 "$f" > /tmp/gl_shapes.$$ 2>&1; then
      echo "FAIL clang: $f"; grep -E 'error' /tmp/gl_shapes.$$ | head -5
      fails=$((fails + 1))
   fi
   case $f in core/rend/gl4/*|core/rend/gles/ta_decoder_gl.cpp) continue ;; esac
   if ! $CXX $FLAGS -DHAVE_LIBNX -DTARGET_LIBNX "$f" > /tmp/gl_shapes.$$ 2>&1; then
      echo "FAIL libnx: $f"; grep -E 'error' /tmp/gl_shapes.$$ | head -5
      fails=$((fails + 1))
   fi
done
rm -f /tmp/gl_shapes.$$

if [ $fails != 0 ]; then
   echo "gl_shapes: $fails failed"
   exit 1
fi
echo "gl_shapes: $(echo "$FILES" | wc -l) files, as macOS and the Switch build them: all passed"
