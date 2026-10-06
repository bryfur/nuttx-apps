#!/bin/sh
# Self-hosting check: xcc's cpp/cc1/as/ld compiled by xcc (ARM32, Linux,
# run in qemu-arm) must produce byte-identical output to the host builds,
# on xcc's own sources and the tests.  A miscompile shows as a difference.
. "$(dirname "$0")/env.sh"
X=$XCCDIR/xcc/src
T=$XCCDIR/xcc/tests
S=/tmp/stage2
rm -rf $S; mkdir -p $S/o
F="-I$X/util -I$X/cc/frontend -I$X/cc/backend -I$X/cpp -I$X/as -DXCC_TARGET_ARCH=XCC_ARCH_ARM32 -DXCC_TARGET_PLATFORM=XCC_PLATFORM_POSIX"
UTIL="$X/util/util.c $X/util/table.c"
CC1SRC="$(ls $X/cc/frontend/*.c $X/cc/backend/*.c) $X/cc/builtin.c $X/cc/cc1.c $(ls $X/cc/arch/arm32/*.c) $UTIL"
CPPSRC="$(ls $X/cpp/*.c) $X/cc/frontend/lexer.c $X/cc/frontend/ast.c $UTIL"
ASSRC="$(ls $X/as/*.c $X/as/arch/arm32/*.c) $UTIL"
LDSRC="$X/ld/arm32/ld_arm32.c $UTIL $X/util/archive.c"
$TESTS_ARM/xcc-arm.sh $F -o $S/cc1 $CC1SRC 2>/dev/null || { echo "stage2: cc1 build failed"; exit 1; }
$TESTS_ARM/xcc-arm.sh $F -o $S/cpp $CPPSRC 2>/dev/null || { echo "stage2: cpp build failed"; exit 1; }
$TESTS_ARM/xcc-arm.sh $F -o $S/as $ASSRC 2>/dev/null || { echo "stage2: as build failed"; exit 1; }
$TESTS_ARM/xcc-arm.sh $F -o $S/ld $LDSRC 2>/dev/null || { echo "stage2: ld build failed"; exit 1; }
D="-D__XCC -D__ILP32__ -D__arm__ -D__thumb__ -D__thumb2__ -D__ARMEL__ -D__ARM_EABI__ -D__ARM_PCS_VFP -D__VFP_FP__ -D__CHAR_UNSIGNED__ -D__linux__ -D__STDC__ -D__STDC_VERSION__=199901L -D__SIZEOF_POINTER__=4 -D__SIZEOF_INT__=4 -D__SIZEOF_LONG__=4 -D__SIZEOF_LONG_LONG__=8 -D__SIZEOF_SIZE_T__=4"
I="-I $X/../include"
n=0; bad=0
for f in $CC1SRC $T/valtest.c $T/fvaltest.c $XCCDIR/bench/bench.c; do
  n=$((n+1)); b=$S/o/$n
  $A/cpp $D $F $I $f > $b.h.i 2>/dev/null || continue
  $Q $S/cpp $D $F $I $f > $b.q.i 2>/dev/null
  cmp -s $b.h.i $b.q.i || { echo "DIFF cpp $f"; bad=$((bad+1)); }
  $A/cc1 -o $b.h.s $b.h.i 2>/dev/null || continue
  $Q $S/cc1 -o $b.q.s $b.h.i 2>/dev/null
  cmp -s $b.h.s $b.q.s || { echo "DIFF cc1 $f"; bad=$((bad+1)); }
  $A/as -o $b.h.o $b.h.s 2>/dev/null || continue
  $Q $S/as -o $b.q.o $b.h.s 2>/dev/null
  cmp -s $b.h.o $b.q.o || { echo "DIFF as $f"; bad=$((bad+1)); }
done
K=$(echo $CC1SRC | wc -w)
OBJS=$(for i in $(seq 1 $K); do echo $S/o/$i.h.o; done)
$A/ld -e main -o $S/h.mod $OBJS || echo ld-failed
$Q $S/ld -e main -o $S/q.mod $OBJS || echo ld2-failed
cmp -s $S/h.mod $S/q.mod || { echo "DIFF ld"; bad=$((bad+1)); }
echo "stage2: $n files, $bad differences"
