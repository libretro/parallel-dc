#!/bin/sh
# The DIMM board of a NAOMI GD-ROM system (core/hw/naomi/naomi_dimm.c)
# without the rest of the machine: what it leaves in the NAOMI's memory at
# start-up and in answer to each request, for the plain board (firmware
# 1.02) and the network board (firmware 3.17).
#
#   sh tools/dimm/run.sh
#
# Built as C89, plain and under ASan + UBSan.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${TMPDIR:-/tmp}/dimm_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=c89 -pedantic -Wall -Wextra -Werror -O2 -g"
SRC="$ROOT/tools/dimm/dimm_test.c $ROOT/core/hw/naomi/naomi_dimm.c"

for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
   ${CC:-cc} $FLAGS $san -I "$ROOT/core/hw/naomi" -o "$WORK/dimm_test$EXE" $SRC
   "$WORK/dimm_test$EXE"
done
