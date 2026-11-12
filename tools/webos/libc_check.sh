#!/bin/sh
# Every C file the webOS build compiles, checked against the SDK's libc.
#
#   . <sdk>/environment-setup && sh tools/webos/libc_check.sh
#
# The SDK's glibc is 2.12, older than _DEFAULT_SOURCE, so a name the
# desktop glibc declares can be missing there: net_http.c's strncasecmp()
# broke the webOS build that way. Nothing links; this is a declaration
# check with the Makefile's own webOS flags.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

CC=${CC:-arm-webos-linux-gnueabi-gcc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/webos_libc.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

make -n platform=webos CC=WEBOS_CC CXX=WEBOS_CXX 2>/dev/null \
   | grep '^WEBOS_CC .* -c .*\.c ' \
   | sed -e "s#^WEBOS_CC #$CC #" -e 's/ -o [^ ]*\.o/ /' -e 's/ -MMD -MP//' \
         -e 's/ -c / -fsyntax-only -Werror=implicit-function-declaration -Werror=implicit-int /' \
   > "$WORK/cmds"

n=0
while IFS= read -r l; do
   n=$((n + 1))
   printf '%s\n' "$l" > "$WORK/$n.sh"
done < "$WORK/cmds"
if [ "$n" -lt 100 ]; then
   echo "only $n C files in the webOS build; the Makefile's output changed shape"
   exit 1
fi

ls "$WORK"/*.sh | xargs -P "$(getconf _NPROCESSORS_ONLN)" -I{} \
   sh -c 'sh {} > {}.log 2>&1 || echo {} >> '"$WORK"'/failed'
if [ -s "$WORK/failed" ]; then
   for f in $(cat "$WORK/failed"); do
      grep -oE ' [^ ]+\.c ' "$f" | tail -1
      grep ': error: ' "$f.log" | sed 's/^/      /' | head -8
   done
   exit 1
fi
echo "ok    $n C files against the webOS SDK"
