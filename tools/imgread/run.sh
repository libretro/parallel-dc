#!/bin/sh
# The disc layer (core/imgread) against what it read before.
#
#   sh tools/imgread/run.sh
#
# make_images.py writes .cue, .gdi and .cdi images - well made, and with
# what a sheet or a table can get wrong - and CHDs made of some of them by
# chdman. disc_test.c, built with the core's own disc layer under ASan and
# UBSan with leak detection, prints everything the rest of the core can
# see of each, which has to be expected.txt: what the C++ disc layer it
# replaced printed. A difference names the image and what of it changed.
#
# Needs chdman (MAME tools) and python3: fails rather than skips without
# them.
set -e
cd "$(dirname "$0")/../.."
command -v chdman >/dev/null || { echo "chdman not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
CC=${CC:-cc}
DEFS=$(make -n -B core/imgread/chd_image.o | grep -o -- '-D[A-Za-z0-9_]*' \
       | grep -v GIT_VERSION | sort -u | tr '\n' ' ')
L=core/libretro-common
WORK=${TMPDIR:-/tmp}/disc_test.$$
mkdir -p "$WORK/o"
trap 'rm -rf "$WORK"' EXIT
INC="-Icore -Icore/libretro -Icore/deps -I$L/include"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
# The disc layer and the test as MSVC's C89 takes them; the rest as built
for s in tools/imgread/disc_test.c core/imgread/*.c core/deps/chdpsr/cdipsr.c; do
   $CC -std=c89 -pedantic -Wno-long-long -Wno-variadic-macros -Werror \
      -Wdeclaration-after-statement -Werror=implicit-function-declaration \
      -O1 -g $DEFS $SAN $INC -isystem $L/include -c "$s" -o "$WORK/o/$(echo "$s" | tr / _).o"
done
for s in core/deps/coreio/coreio.c core/archive/archive.c \
   $L/formats/chd/rchd.c $L/encodings/encoding_huffman.c $L/encodings/encoding_rzstd.c \
   $L/encodings/encoding_deflate.c $L/encodings/encoding_crc32.c $L/formats/7z/r7z_lzma.c \
   $L/formats/7z/r7z_archive.c $L/encodings/encoding_rbzip2.c $L/formats/7z/r7z_lzma2.c \
   $L/formats/7z/r7z_lzma_stream.c $L/formats/7z/r7z_bcj2.c $L/formats/7z/r7z_filters.c \
   $L/formats/zip/rzip_archive.c $L/formats/rar/rrar_archive.c $L/formats/rar/rrar_ppmd7.c \
   $L/rthreads/rthreads.c $L/rthreads/retro_eventcount.c $L/formats/flac/rflac.c \
   $L/features/features_cpu.c $L/streams/file_stream.c $L/vfs/vfs_implementation.c \
   $L/vfs/vfs_prefetch.c $L/file/file_path.c $L/file/file_path_io.c $L/file/retro_dirent.c \
   $L/compat/compat_strl.c $L/compat/fopen_utf8.c $L/encodings/encoding_utf.c \
   $L/string/stdstring.c $L/string/rstrtod.c $L/time/rtime.c; do
   $CC -O1 -g $DEFS $SAN $INC -c "$s" -o "$WORK/o/$(echo "$s" | tr / _).o"
done
$CC $SAN -o "$WORK/disc_test" "$WORK"/o/*.o -lm -lpthread
(cd "$WORK" && python3 "$OLDPWD/tools/imgread/make_images.py" images > list.txt)
export ASAN_OPTIONS=detect_leaks=1
# (what the CDI table's reader prints of an image it refuses is not the test's)
(cd "$WORK" && ./disc_test $(cat list.txt)) | grep -E '^(==| |type |not opened)' > "$WORK/got.txt"
if ! diff -u tools/imgread/expected.txt "$WORK/got.txt" > "$WORK/diff.txt"; then
   head -60 "$WORK/diff.txt"
   echo "disc_test: FAIL - the images read differently (above: expected, then got)"
   exit 1
fi
echo "disc_test: PASS ($(grep -c '^==' "$WORK/got.txt") images, $(grep -c '^not opened' "$WORK/got.txt") of them not opened)"
