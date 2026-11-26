#!/bin/sh
# The core is C, and is linked as C: no C++ runtime among the libraries it
# needs, and no symbol of one left for the loader to find.
#
#   sh tools/c89/no_cxx_runtime.sh CORE
#
# CORE is the built core, an ELF shared object or a Windows DLL. A core
# linked by a C++ driver (g++, clang++) needs libstdc++ or libc++ even
# with no C++ in it, which a system without one does not load.
#
# READELF, NM and OBJDUMP are the tools for the core's processor
# (default readelf, nm, objdump).
set -e
CORE=${1:?core}
[ -f "$CORE" ] || { echo "no_cxx_runtime: $CORE: no such file" >&2; exit 2; }
fails=0
if "${OBJDUMP:-objdump}" -f "$CORE" 2>/dev/null | grep -q 'file format pei-'; then
   needs=$("${OBJDUMP:-objdump}" -p "$CORE" | sed -n 's/^[[:space:]]*DLL Name: //p')
else
   needs=$("${READELF:-readelf}" -d "$CORE" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
   # (__cxa_finalize and __cxa_atexit are the C library's)
   syms=$("${NM:-nm}" -D --undefined-only "$CORE" | awk '{print $NF}' \
      | grep -E '^(_Z|__cxa_|__gxx_personality)|@(GLIBCXX|CXXABI)' | grep -v '@GLIBC_' || true)
   if [ -n "$syms" ]; then
      echo "$syms" | head -n 5 | sed 's/^/  /'
      echo "no_cxx_runtime: FAIL: $CORE wants C++ runtime symbols"
      fails=1
   fi
fi
cxx=$(echo "$needs" | grep -iE '^lib(std)?c\+\+' || true)
if [ -n "$cxx" ]; then
   echo "  $cxx"
   echo "no_cxx_runtime: FAIL: $CORE needs a C++ runtime"
   fails=1
fi
[ $fails = 0 ] || exit 1
echo "no_cxx_runtime: $CORE needs no C++ runtime ($(echo $needs))"
