#!/bin/sh
# The sound chip's DSP, compiled against interpreted.
#
#   sh tools/aica/run.sh [cases [flags]]
#
# The DSP runs a 128-step program for every sample. The core has it four
# times: a compiler that turns the program into machine code for each of
# x86-64, x86 and ARM64 (dsp_x64.c, dsp_x86.c, dsp_arm64.c), and an
# interpreter (dsp_interp.c) for everything else.
#
# dsp_test.c is built around each of them in turn, with nothing of the
# rest of the core, and all are given the same made-up programs, 1000
# unless a number is given: 1 to 64 random steps, random coefficients and
# addresses, 64 samples of random input. What each leaves behind - its 16
# outputs after every sample, its registers and its delay memory at the
# end - has to be the same from all, for every program. x86 is built with
# i686-linux-gnu-gcc (or gcc -m32), ARM64 with aarch64-linux-gnu-gcc and
# run under qemu-aarch64-static; where one of them is not there, that
# compiler is left out and said to be.
#
# Every program is also run a second time with a step added at the very
# end of the 128 that is not empty and does nothing. The DSP leaves out
# the empty steps after a program's last; the added step makes it run them
# all. Both runs have to leave the same behind, from each.
#
# One difference between the compilers and the interpreter is known and
# kept out of the programs. A step can take a value that a step two before
# it read from memory. The compilers keep the four places those values
# wait in from one sample to the next; the interpreter empties them at the
# start of every sample. So a program that takes a value without having
# read one two steps before gets an old value from one and zero from the
# other. Which the hardware does is not known here, and no program that
# means anything does it, so the made-up programs only take a value two
# steps after a read.
#
# Needs the core's makefile for its flags. Flags after the number of
# cases, such as -fsanitize=address,undefined, go to the interpreter's and
# the x86-64 compiler's builds; $X86_CFLAGS goes to the x86 build (a
# header path a host's -m32 may want).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CASES=${1:-1000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/aica_dsp.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# the C flags the core compiles dsp.c with, for the make arguments given
flags() {
   CMD=$(make -n -B "$@" core/hw/aica/dsp.o 2>/dev/null | grep 'dsp\.c' | tail -n 1)
   [ -n "$CMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
   CMD=${CMD% -o *}
   echo "$CMD" | sed 's| [^ ]*dsp\.c||; s/ -MMD -MP//'
}

build() {
   # $1: name, $2: C flags (with the compiler), $3: extra flags, $4: link flags
   for SRC in core/hw/aica/dsp.c core/hw/aica/dsp_x64.c core/hw/aica/dsp_x86.c core/hw/aica/dsp_arm64.c \
         core/hw/aica/dsp_interp.c tools/aica/dsp_test.c; do
      # the flags come out of make quoted for a shell
      eval "$2 $3 \"$SRC\" -o \"$WORK/$1-$(basename "$SRC" .c).o\""
   done
   CC=${2%% *}
   $CC $3 $4 -o "$WORK/dsp_test_$1" "$WORK/$1"-*.o
}

same() {
   # $1, $2: outputs to compare, $3: what they are; the case number and the
   # program's length lead each line and are left out of the comparison
   cut -d' ' -f3- "$1" > "$WORK/a.txt"
   cut -d' ' -f3- "$2" > "$WORK/b.txt"
   if ! cmp -s "$WORK/a.txt" "$WORK/b.txt"; then
      echo "FAIL: $3 differ, first in these cases:" >&2
      diff "$1" "$2" | grep '^<' | head -n 5 | cut -d' ' -f2-3 >&2
      exit 1
   fi
}

# $1: name, $2: how it is run (empty, or qemu)
check() {
   $2 "$WORK/dsp_test_$1" "$CASES" > "$WORK/$1.txt"
   $2 "$WORK/dsp_test_$1" "$CASES" all > "$WORK/$1-all.txt"
   same "$WORK/$1.txt" "$WORK/$1-all.txt" "the $1 compiler's steps in use and all 128"
   same "$WORK/$1.txt" "$WORK/interp.txt" "the $1 compiler and the interpreter"
   DONE="$DONE $1"
}

NATIVE=$(flags)
build interp "$NATIVE" "-DFEAT_DSPREC=DYNAREC_NONE $EXTRA"
"$WORK/dsp_test_interp" "$CASES" > "$WORK/interp.txt"
"$WORK/dsp_test_interp" "$CASES" all > "$WORK/interp-all.txt"
same "$WORK/interp.txt" "$WORK/interp-all.txt" "the interpreter's steps in use and all 128"

DONE=
LEFT=
case $(uname -m) in
   x86_64|amd64)
      build x64 "$NATIVE" "$EXTRA"
      check x64

      X86CC=
      if command -v i686-linux-gnu-gcc >/dev/null 2>&1; then
         X86CC=i686-linux-gnu-gcc
      elif echo 'int main(void){return 0;}' | gcc -m32 -x c - -o "$WORK/m32" >/dev/null 2>&1; then
         X86CC=gcc
      fi
      if [ -n "$X86CC" ]; then
         build x86 "$(flags platform=unix ARCH=i686 CC=$X86CC)" "$X86_CFLAGS" -m32
         check x86
      else
         LEFT="$LEFT x86"
      fi
      ;;
esac
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && command -v qemu-aarch64-static >/dev/null 2>&1; then
   build arm64 "$(flags platform=unix ARCH=aarch64 CC=aarch64-linux-gnu-gcc)" "" -static
   check arm64 qemu-aarch64-static
else
   LEFT="$LEFT arm64"
fi
echo "dsp: ok ($CASES programs: the interpreter and the compilers for${DONE:- none} agree, with and without the empty steps${LEFT:+; no compiler here for$LEFT})"
