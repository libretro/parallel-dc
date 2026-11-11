#!/bin/sh
# The core's C, as MSVC's C89 takes it (AGENTS.md: MSVC C89, gradually):
# every .c file of the core's own that the build compiles (not deps/ or
# libretro-common/, which are others'), with the build's own flags, compiled
# again as -std=c89 -pedantic, declarations after statements, functions
# called undeclared (C89 takes them as int f(), as MSVC does) and every
# warning an error. The headers they include are held to it with them -
# types.h, which every file includes, first among them. Third-party
# headers (deps/, libretro-common/) are system headers here.
#
#   sh tools/c89/c89_check.sh [make arguments...]
#
# CC (default cc) is the compiler; the make arguments, the build's (e.g.
# platform=win CC=x86_64-w64-mingw32-gcc for the Windows files).
set -e
cd "$(dirname "$0")/../.."
CC=${CC:-cc}
flags=$(make -s -f Makefile -f - "$@" c89-flags <<'MK'
c89-flags: ; @echo $(INCFLAGS) $(CFLAGS) $(MFLAGS) ; echo $(filter core/% ./core/%,$(SOURCES_C))
MK
)
cflags=$(echo "$flags" | sed -n 1p | sed 's/ -std=[^ ]*//g; s/ -c / /g' \
   | sed 's#-I\(\./\)\{0,1\}core/deps#-isystem ./core/deps#g; s#-I\(\./\)\{0,1\}core/libretro-common#-isystem ./core/libretro-common#g')
files=$(echo "$flags" | sed -n 2p | tr ' ' '\n' | sed 's#^\./##' | grep -v '^core/deps/\|^core/libretro-common/' | sort -u)
fails=0
n=0
for f in $files; do
   n=$((n + 1))
   if ! out=$($CC $cflags -std=c89 -pedantic -Wno-long-long -Wno-variadic-macros -Werror \
         -Wdeclaration-after-statement -Werror=implicit-function-declaration -fsyntax-only "$f" 2>&1); then
      echo "$out" | grep -m 5 "error" | sed "s#^#  #"
      echo "c89_check: FAIL: $f"
      fails=$((fails + 1))
   fi
done
[ $fails -eq 0 ] || { echo "c89_check: $fails of $n files not C89"; exit 1; }
echo "c89_check: $n files, all C89"
