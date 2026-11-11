#!/bin/sh
# Custom textures' pictures (core/rend/custom_image.h, read with
# libretro-common's rpng and rjpeg, dumped with rpng): PNGs of each kind
# (grey, grey and alpha, palettes, 1, 4, 8 and 16 bits, every row filter,
# interlaced) and JPEGs (4:2:0, 4:2:2, 4:4:4, grey, progressive, restart
# intervals) read to the very pixels stb_image read them to when it was
# the core's (tools/rend/custom_image_expected.txt: stb_image's but at
# 4:2:2's last two columns, the second of which it took mostly from its left
# neighbour, rjpeg's the way round libjpeg takes it); each file cut short
# at every length and spoilt read without a fault; textures of each kind
# dumped and read back the same, the PNG read by Python's zlib as well.
# Under the address and undefined-behaviour sanitizers, leaks too.
#
#   sh tools/rend/custom_image_test.sh
#
# CC (default cc) builds it. With WINDOWS=1, CC a mingw-w64 one (default
# x86_64-w64-mingw32-gcc), built for Windows with no sanitizer and run
# with $WINE (default wine; "" on Windows). With RUN set, built with no
# sanitizer and run with it: another machine's, CC="aarch64-linux-gnu-gcc
# -static" RUN=qemu-aarch64-static.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/custom_image_test.$$
mkdir -p "$WORK/dump"
trap 'rm -rf "$WORK"' EXIT
L=core/libretro-common
OURS="tools/rend/custom_image_test.c core/rend/custom_image.c"
THEIRS="$L/formats/png/rpng.c $L/formats/png/rpng_encode.c $L/file/rpng_file.c
   $L/formats/jpeg/rjpeg.c $L/encodings/encoding_xxh32.c $L/encodings/encoding_crc32.c $L/encodings/encoding_deflate.c
   $L/streams/trans_stream.c $L/streams/trans_stream_deflate.c $L/streams/trans_stream_pipe.c
   $L/streams/interface_stream.c $L/streams/memory_stream.c $L/streams/file_stream.c
   $L/vfs/vfs_implementation.c $L/file/file_path.c $L/file/file_path_io.c
   $L/compat/compat_strl.c $L/compat/fopen_utf8.c $L/encodings/encoding_utf.c
   $L/string/stdstring.c $L/string/rstrtod.c $L/time/rtime.c $L/features/features_cpu.c"
STRICT="-std=c89 -pedantic -Wno-long-long -Wall -Wextra -Werror -Wdeclaration-after-statement"
INC="-Icore/rend -I$L/include"
FIXTURES=$(ls tools/rend/custom_image/*)

if [ "$WINDOWS" = 1 ]; then
   CC=${CC:-x86_64-w64-mingw32-gcc}
   RUN=${WINE-wine}
   EXE="$WORK/t.exe"
   SAN=
elif [ -n "$RUN" ]; then
   CC=${CC:-cc}
   EXE="$WORK/t"
   SAN=
else
   CC=${CC:-cc}
   EXE="$WORK/t"
   SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
   export ASAN_OPTIONS=detect_leaks=1
fi
objs=
for f in $THEIRS; do
   o="$WORK/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
   $CC -g -O1 $SAN $INC -c -o "$o" "$f"
   objs="$objs $o"
done
for f in $OURS; do
   o="$WORK/$(basename "$f" .c).o"
   $CC $STRICT -g -O1 $SAN $INC -c -o "$o" "$f"
   objs="$objs $o"
done
$CC $SAN -o "$EXE" $objs -lm

fail() { echo "custom_image_test: $1" >&2; exit 1; }

# (a Windows path for wine; the names alone are compared)
$RUN "$EXE" decode $FIXTURES | tr -d '\r' | sort > "$WORK/got.txt" || fail "decode failed"
if ! cmp -s "$WORK/got.txt" tools/rend/custom_image_expected.txt; then
   diff tools/rend/custom_image_expected.txt "$WORK/got.txt" >&2 || true
   fail "pictures not read as stb_image read them"
fi
echo "custom_image_test: $(wc -l < "$WORK/got.txt") pictures read as stb_image read them"
$RUN "$EXE" cut $FIXTURES > "$WORK/cut.txt" || fail "a file cut short or spoilt faulted"
echo "custom_image_test: each cut short at every length and spoilt, no fault"
$RUN "$EXE" dump "$WORK/dump" | tr -d '\r' || fail "dumped textures not read back the same"

# The dumped files read by another PNG reader
if command -v python3 > /dev/null; then
   python3 tools/rend/custom_image_check.py "$WORK/dump"/*.png || fail "dumped PNGs not as Python reads them"
fi
echo "custom_image_test: PASS"
