#!/bin/sh
# The modem's UPnP (core/network/miniupnp.cpp, on libretro-common's
# net_upnp) against a gateway on this host (upnp_gateway.py): found by an
# SSDP search, its external address asked for, a TCP and a UDP port mapped
# to this host and both deleted when let go, each as the gateway logs it.
# Then with no gateway: given up on in its time, nothing mapped. Under the
# address and undefined-behaviour sanitizers, leaks too.
#
#   sh tools/threads/upnp_test.sh
set -e
cd "$(dirname "$0")/../.."
CXX=${CXX:-c++}
CC=${CC:-cc}
WORK=${TMPDIR:-/tmp}/upnp_test.$$
mkdir -p "$WORK"
GW=
trap '[ -n "$GW" ] && kill $GW 2> /dev/null; rm -rf "$WORK"' EXIT
L=core/libretro-common
INC="-Icore -Icore/libretro -Icore/deps -I$L/include"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
export ASAN_OPTIONS=detect_leaks=1
objs=
for f in $L/net/net_upnp.c $L/net/net_http.c $L/net/net_socket.c $L/net/net_compat.c \
      $L/formats/xml/rxml.c $L/lists/string_list.c $L/encodings/encoding_base64.c \
      $L/compat/compat_strl.c $L/compat/compat_strcasestr.c $L/string/stdstring.c \
      $L/encodings/encoding_utf.c $L/time/rtime.c $L/features/features_cpu.c; do
   o="$WORK/$(basename "$f" .c).o"
   $CC -g -O1 $SAN $INC -c -o "$o" "$f"
   objs="$objs $o"
done
$CXX -std=c++11 -fpermissive -g -O1 $SAN $INC -DENABLE_MODEM -o "$WORK/upnp_test" \
   tools/threads/upnp_test.cpp core/network/miniupnp.cpp $objs -lpthread

fail() { echo "upnp_test: $1" >&2; exit 1; }

"$WORK/upnp_test" none || fail "with no gateway"

python3 tools/threads/upnp_gateway.py "$WORK/gw.log" "$WORK/ready" &
GW=$!
i=0
while [ ! -f "$WORK/ready" ]; do
   i=$((i + 1))
   [ $i -lt 100 ] || fail "the gateway did not start"
   sleep 0.1
done
"$WORK/upnp_test" found || { cat "$WORK/gw.log" >&2; fail "with a gateway"; }

# What the gateway was asked, in order, the arguments that matter
sed -n 's/^\([A-Za-z]*\).*NewExternalPort=\([0-9]*\) NewProtocol=\([A-Z]*\).*/\1 \2 \3/p;
        s/^GetExternalIPAddress$/GetExternalIPAddress/p' "$WORK/gw.log" > "$WORK/asked"
printf '%s\n' GetExternalIPAddress "AddPortMapping 5000 TCP" "AddPortMapping 5001 UDP" \
   "DeletePortMapping 5000 TCP" "DeletePortMapping 5001 UDP" > "$WORK/want"
if ! cmp -s "$WORK/asked" "$WORK/want"; then
   cat "$WORK/gw.log" >&2
   diff "$WORK/want" "$WORK/asked" >&2 || true
   fail "the gateway was not asked as it should be"
fi
# (the ports mapped to this host, as the gateway sees it, both ways the same)
grep '^AddPortMapping' "$WORK/gw.log" | grep -v 'NewInternalPort=500[01] NewInternalClient=127.0.0.1' \
   && fail "a port mapped to another host or port"
echo "upnp_test: PASS (gateway found, its address asked, TCP and UDP mapped and deleted; none found without)"
