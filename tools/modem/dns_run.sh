#!/bin/sh
# The modem's DNS queries and answers (core/hw/modem/dns.c) by themselves.
#
#   sh tools/modem/dns_run.sh [extra C flags]
#
# dns_test.c is built with dns.c, with the C flags the core is built with;
# see the test for what it checks. Extra flags, such as
# -fsanitize=address,undefined, go to the compiler and the link.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/dns_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

CCMD=$(make -n -B core/hw/modem/dns.o 2>/dev/null | grep 'dns\.c' | tail -n 1)
[ -n "$CCMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
CCMD=${CCMD% -o *}
CFLAGS_DNS=$(echo "$CCMD" | sed 's| [^ ]*dns\.c||; s/ -MMD -MP//')
CC=${CFLAGS_DNS%% *}

for SRC in core/hw/modem/dns.c core/libretro-common/net/net_socket.c \
      core/libretro-common/net/net_compat.c core/libretro-common/features/features_cpu.c \
      tools/modem/dns_test.c; do
   eval "$CFLAGS_DNS $EXTRA \"$SRC\" -o \"$WORK/$(basename "$SRC" .c).o\""
done
"$CC" $EXTRA -o "$WORK/dns_test" "$WORK"/*.o
"$WORK/dns_test"
