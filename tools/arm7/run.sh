#!/bin/sh
# The sound chip's ARM7, recompiled against interpreted.
#
#   sh tools/arm7/run.sh [programs [flags]]
#
# On x86-64, 32-bit x86 and ARM64 the ARM7 is run by a recompiler (core/hw/
# arm7/arm7_rec.c, with arm7_rec_x64.c, arm7_rec_x86.c or arm7_rec_arm64.c);
# everywhere the recompilers do not reach, and for the instructions this
# one hands back, by an interpreter (arm7.c with arm-new.h). arm7_test.c is
# built around each, with nothing else of the core, and all are given the
# same made-up programs, 10000 unless a number is given. The registers, the
# flags and all of memory have to be the same from each at the end of every
# program, and the cycles left over the same from every recompiler (the
# interpreter counts its own way). arm7_test.c says what the programs are made of and what is left
# out of them.
#
# A program that stores over its own instructions is not compared, and
# neither is one that stops on a check after doing so.
#
# The build with the recompiler has the interpreter in it as well, for the
# "Sound CPU Recompiler" core option to switch to. The programs are run
# twice more through that build: with the interpreter chosen, and changing
# between the two from one sample to the next. Both have to come out as
# the others.
#
# ARM7_TEST_MODES=1 has the programs change the processor's mode as well
# (MSR CPSR_c), between the modes with banked registers: other programs.
#
# x86 is built with i686-linux-gnu-gcc (or gcc -m32), ARM64 with
# aarch64-linux-gnu-gcc and run under qemu-aarch64-static; where one of
# them is not there, that recompiler is left out and said to be. Flags
# after the number of programs, such as -fsanitize=address,undefined, go
# to the interpreter's and the x86-64 recompiler's builds; $X86_CFLAGS
# goes to the x86 build (a header path a host's -m32 may want).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
PROGRAMS=${1:-10000}
[ $# -gt 0 ] && shift
EXTRA="$*"
WORK=${TMPDIR:-/tmp}/arm7_test.$$
mkdir -p "$WORK"
[ -n "$ARM7_KEEP" ] || trap 'rm -rf "$WORK"' EXIT

# the C flags the core compiles the ARM7 with, for the make arguments given
flags() {
   CMD=$(make -n -B "$@" core/hw/arm7/arm7_rec.o 2>/dev/null | grep 'arm7_rec\.c' | tail -n 1)
   [ -n "$CMD" ] || { echo "cannot get the core's C compile flags from make" >&2; exit 1; }
   CMD=${CMD% -o *}
   echo "$CMD" | sed 's| [^ ]*arm7_rec\.c||; s/ -MMD -MP//'
}

build() {
   # $1: name, $2: C flags (with the compiler), $3: extra flags, $4: link flags
   for SRC in core/hw/arm7/arm7_rec.c core/hw/arm7/arm7_rec_x64.c core/hw/arm7/arm7_rec_x86.c \
         core/hw/arm7/arm7_rec_arm64.c core/hw/arm7/arm_mem.c core/hw/arm7/arm7.c tools/arm7/arm7_test.c; do
      # the flags come out of make quoted for a shell
      eval "$2 $3 \"$SRC\" -o \"$WORK/$1-$(basename "$SRC" .c).o\""
   done
   CC=${2%% *}
   $CC $3 $4 -o "$WORK/arm7_test_$1" "$WORK/$1"-*.o
}

same() {
   # $1, $2: outputs to compare, $3 and $4: what each is, $5: both recompiled
   [ "$(wc -l < "$1")" -eq "$PROGRAMS" ] && [ "$(wc -l < "$2")" -eq "$PROGRAMS" ] || {
      echo "FAIL: not every program was run to its end ($3, $4)" >&2
      exit 1
   }
   # (the cycles left over only where both are recompiled)
   if [ -n "$5" ]; then
      paste -d'|' "$1" "$2" > "$WORK/both.txt"
   else
      sed 's/ cyc [0-9a-f]*$//' "$1" > "$WORK/a.txt"
      sed 's/ cyc [0-9a-f]*$//' "$2" > "$WORK/b.txt"
      paste -d'|' "$WORK/a.txt" "$WORK/b.txt" > "$WORK/both.txt"
   fi
   if ! awk -F'|' -v a="$3" -v b="$4" '
      $1 ~ /wrote over/ || $2 ~ /wrote over/ { skipped++; next }
      $1 != $2 { if (bad++ < 5) { print a ": " $1; print b ": " $2 } }
      END { printf "%d %d\n", skipped, bad > "'"$WORK"'/count.txt"; exit bad != 0 }' "$WORK/both.txt" >&2; then
      echo "FAIL: $3 and $4 differ on $(cut -d' ' -f2 "$WORK/count.txt") programs; the first are above" >&2
      exit 1
   fi
}

# $1: name, $2: how it is run (empty, or qemu): the program in ror0.txt
ror0() {
   CPSR=$(ARM7_TEST_PROGRAM="$WORK/ror0.txt" ARM7_TEST_CASE=0 $2 "$WORK/arm7_test_$1" | sed -n 's/.* cpsr \([0-9a-f]*\) .*/\1/p')
   [ -n "$CPSR" ] && [ $((0x$CPSR & 0x20000000)) -ne 0 ] \
      || { echo "FAIL: $1: ror by a register of 0 took the carry from bit 31 (cpsr ${CPSR:-none})" >&2; exit 1; }
}

# $1: name, $2: how it is run (empty, or qemu)
check() {
   ror0 "$1" "$2"
   $2 "$WORK/arm7_test_$1" "$PROGRAMS" > "$WORK/$1.txt"
   ARM7_TEST_INTERPRET=1 $2 "$WORK/arm7_test_$1" "$PROGRAMS" > "$WORK/$1-chosen.txt"
   ARM7_TEST_SWITCH=1 $2 "$WORK/arm7_test_$1" "$PROGRAMS" > "$WORK/$1-switched.txt"
   same "$WORK/$1-chosen.txt" "$WORK/interp.txt" "$1: the interpreter chosen by the option" "the interpreter"
   same "$WORK/$1-switched.txt" "$WORK/interp.txt" "$1: changing between the two" "the interpreter"
   same "$WORK/$1.txt" "$WORK/interp.txt" "$1: recompiled" "interpreted"
   # what the recompiled programs leave, for comparing one machine's with another's
   [ -z "$ARM7_TEST_OUT" ] || cp "$WORK/$1.txt" "$ARM7_TEST_OUT.$1"
   DONE="$DONE $1"
}

# which recompilers can be built here, and their builds, all at once
NATIVE=$(flags)
DONE=
LEFT=
X86FLAGS=
ARM64FLAGS=
ARM64RUN=
case $(uname -m) in
   x86_64|amd64)
      X86CC=
      if command -v i686-linux-gnu-gcc >/dev/null 2>&1; then
         X86CC=i686-linux-gnu-gcc
      elif echo 'int main(void){return 0;}' | gcc -m32 -x c - -o "$WORK/m32" >/dev/null 2>&1; then
         X86CC=gcc
      fi
      if [ -n "$X86CC" ]; then
         X86FLAGS=$(flags platform=unix ARCH=i686 CC=$X86CC)
      else
         LEFT="$LEFT x86"
      fi
      ;;
esac
case $(uname -m) in
   aarch64|arm64) ;;
   *)
      if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && command -v qemu-aarch64-static >/dev/null 2>&1; then
         ARM64FLAGS=$(flags platform=unix ARCH=aarch64 CC=aarch64-linux-gnu-gcc)
         ARM64RUN=qemu-aarch64-static
      else
         LEFT="$LEFT arm64"
      fi
      ;;
esac

PIDS=
build interp "$NATIVE" "-DFEAT_AREC=DYNAREC_NONE $EXTRA" & PIDS="$PIDS $!"
case $(uname -m) in
   x86_64|amd64|aarch64|arm64) build native "$NATIVE" "$EXTRA" & PIDS="$PIDS $!" ;;
esac
[ -z "$X86FLAGS" ] || { build x86 "$X86FLAGS" "$X86_CFLAGS" -m32 & PIDS="$PIDS $!"; }
[ -z "$ARM64FLAGS" ] || { build arm64 "$ARM64FLAGS" "" -static & PIDS="$PIDS $!"; }
for P in $PIDS; do
   wait "$P" || { echo "FAIL: a build did not finish" >&2; exit 1; }
done

"$WORK/arm7_test_interp" "$PROGRAMS" > "$WORK/interp.txt"
# A rotation by a register whose low byte is 0 leaves the carry as it is
# (it was taken from bit 31): mov r8, #0x100; mov r7, #0; movs r0,
# #0x80000000 (C set); bics r11, r3, r7, ror r8; and C has to be set still
printf 'e3a08c01 e3a07000 e3b00102 e1d3b877 eafffffe\n' > "$WORK/ror0.txt"
ror0 interp
case $(uname -m) in
   x86_64|amd64)
      mv "$WORK/arm7_test_native" "$WORK/arm7_test_x64"
      check x64
      ;;
   aarch64|arm64)
      mv "$WORK/arm7_test_native" "$WORK/arm7_test_arm64"
      check arm64
      ;;
esac
[ -z "$X86FLAGS" ] || check x86
[ -z "$ARM64FLAGS" ] || check arm64 "$ARM64RUN"
# the recompilers with each other, to the cycle
FIRST=
for A in $DONE; do
   if [ -z "$FIRST" ]; then
      FIRST=$A
   else
      same "$WORK/$A.txt" "$WORK/$FIRST.txt" "recompiled for $A" "recompiled for $FIRST" cycles
   fi
done
SKIPPED=0
if [ -n "$FIRST" ]; then
   same "$WORK/$FIRST.txt" "$WORK/interp.txt" "recompiled" "interpreted"
   SKIPPED=$(cut -d' ' -f1 "$WORK/count.txt")
fi
echo "arm7: ok ($((PROGRAMS - SKIPPED)) programs: the interpreter and the recompilers for${DONE:- none} agree, recompiled, chosen and changing between the two, and the recompilers to the cycle; $SKIPPED more wrote over themselves and were not compared${LEFT:+; no recompiler here for$LEFT})"
