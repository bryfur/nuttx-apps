#!/bin/sh
# The xcc libc (libsrc) for arm32 Linux with the host-built cpp/cc1 + GNU as
. "$(dirname "$0")/env.sh"
if [ "$XAS" = gnu ]; then ASM="arm-none-eabi-as -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard"; else ASM=$A/as; fi
X=$XCCDIR/xcc
O=$A/lib
mkdir -p $O/obj
D="-D__XCC -D__ILP32__ -D__arm__ -D__thumb__ -D__thumb2__ -D__ARMEL__ -D__ARM_EABI__ -D__ARM_ARCH=8 -D__ARM_PCS_VFP -D__VFP_FP__ -D__CHAR_UNSIGNED__ -D__linux__ -D__unix__ -D__STDC__ -D__STDC_VERSION__=199901L -D__SIZEOF_POINTER__=4 -D__SIZEOF_INT__=4 -D__SIZEOF_LONG__=4 -D__SIZEOF_LONG_LONG__=8 -D__SIZEOF_SIZE_T__=4"
fail=0
for d in math misc stdio stdlib string unistd crt0; do
  for f in $X/libsrc/$d/*.c; do
    b=$(basename $f .c)
    if ! $A/cpp $D -I $X/include $f > $O/obj/$b.i 2>$O/obj/$b.err; then echo "CPP FAIL $f"; head -3 $O/obj/$b.err; fail=$((fail+1)); continue; fi
    if ! $A/cc1 $O/obj/$b.i > $O/obj/$b.s 2>$O/obj/$b.err; then echo "CC1 FAIL $f"; head -3 $O/obj/$b.err; fail=$((fail+1)); continue; fi
    if ! $ASM -o $O/obj/$b.o $O/obj/$b.s 2>$O/obj/$b.err; then echo "AS FAIL $f"; head -5 $O/obj/$b.err; fail=$((fail+1)); continue; fi
  done
done
rm -f $O/libc.a $O/crt0.o
cp $O/obj/_start.o $O/crt0.o
arm-none-eabi-ar rcs $O/libc.a $(ls $O/obj/*.o | grep -v _start.o)
echo "failures: $fail"
