#!/bin/sh
# The Vulkan renderers compiled for 32-bit Windows (mingw-w64's i686), as
# the buildbot builds them: there a non-dispatchable Vulkan handle is a
# 64-bit integer, not a pointer, and one handed to or from a pointer (NULL,
# a void *) is an error here, not only on that build. The Makefile's
# flags for 64-bit Windows, its host 32-bit x86; syntax only.
#
#   sh tools/vulkan/vk_i686.sh
set -e
cd "$(dirname "$0")/../.."
CC_I686=${CC_I686:-i686-w64-mingw32-gcc}
FLAGS=$(make -s -n -B platform=win64 CC=XCC core/rend/vulkan/vk_renderer.o 2>/dev/null | sed -n 's/^XCC //p' | head -1 \
   | sed 's/ -c .*//; s/ -o [^ ]*//; s/-DHOST_CPU=[^ ]*//g; s/-DGIT_VERSION=[^ ]* [^ ]*//')
[ -n "$FLAGS" ] || { echo "vk_i686: no flags from the Makefile"; exit 1; }
fails=0
for f in core/rend/vulkan/*.c core/rend/vulkan/oit/*.c; do
   [ -f "$f" ] || continue
   $CC_I686 -fsyntax-only $FLAGS -DHOST_CPU=0x20000001 '-DGIT_VERSION="i686"' -Werror=int-conversion \
      -Werror=incompatible-pointer-types -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast "$f" \
      || { echo "  FAIL: $f"; fails=$((fails + 1)); }
done
[ $fails -eq 0 ] || { echo "vk_i686: $fails failed"; exit 1; }
echo "vk_i686: every Vulkan renderer file compiles for 32-bit Windows"
