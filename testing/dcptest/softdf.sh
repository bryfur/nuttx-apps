#!/bin/sh
# softdf.sh "CC FLAGS" CROSSDEV OUT: libgcc's double-precision routines and
# newlib's __ieee754_sqrt in one object, their symbols renamed soft_*
# (__aeabi_dadd -> soft_dadd, __adddf3 -> soft_gnu_adddf3, __ieee754_sqrt
# -> soft___ieee754_sqrt): dcptest's reference.  (Not sqrt(): its bl to
# __errno in the firmware would not reach from a module in the PSRAM.)
set -e
CC="$1"
X="$2"
out="$3"
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
gcc=$($CC --print-libgcc-file-name)
m=$($CC --print-file-name=libm.a)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
cd "$t"
${X}ar x "$gcc" _arm_addsubdf3.o _arm_muldivdf3.o _arm_cmpdf2.o \
  _arm_unorddf2.o _arm_fixdfsi.o _arm_fixunsdfsi.o _arm_truncdfsf2.o
${X}ar x "$m" libm_a-e_sqrt.o
${X}ld -r -o all.o _arm_*.o libm_a-*.o
${X}nm all.o | awk '
  { n = $NF; t = $(NF - 1) }
  n ~ /^__aeabi_/ { print n, "soft_" substr(n, 9); next }
  n == "__ieee754_sqrt" { print n, "soft___ieee754_sqrt"; next }
  t == "T" && n ~ /^__/ { print n, "soft_gnu_" substr(n, 3) }
' | sort -u > map
${X}objcopy --redefine-syms=map all.o "$out"
