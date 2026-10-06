#!/bin/sh
# asan.sh: xcc's preprocessor, compiler and assembler built with gcc's
# address and undefined-behavior sanitizers, run on the tests' sources,
# xcc's own and the benchmarks (a bad read or write of the compiler shows
# here; on the board it corrupts memory: a hang, a reset)
. "$(dirname "$0")/env.sh"
X=$XCCDIR/xcc/src
T=$XCCDIR/xcc/tests
O=$A/asan
mkdir -p $O
F="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -std=gnu11 -D_DEFAULT_SOURCE -w -I$X/util -I$X/cc/frontend -I$X/cc/backend -I$X/cpp -I$X/as -DXCC_TARGET_ARCH=XCC_ARCH_ARM32 -DXCC_TARGET_PLATFORM=XCC_PLATFORM_POSIX"
U="$X/util/util.c $X/util/table.c"
gcc $F -o $O/cpp $X/cpp/*.c $X/cc/frontend/lexer.c $X/cc/frontend/ast.c $U || exit 1
gcc $F -o $O/cc1 $X/cc/frontend/*.c $X/cc/backend/*.c $X/cc/builtin.c $X/cc/cc1.c $X/cc/arch/arm32/*.c $U || exit 1
gcc $F -o $O/as $X/as/*.c $X/as/arch/arm32/*.c $U $X/util/bin_util.c || exit 1
export ASAN_OPTIONS=detect_leaks=0
D="-D__XCC -D__ILP32__ -D__arm__ -D__thumb__ -D__thumb2__ -D__ARMEL__ -D__ARM_EABI__ -D__ARM_PCS_VFP -D__VFP_FP__ -D__CHAR_UNSIGNED__ -D__linux__ -D__STDC__ -D__STDC_VERSION__=199901L -D__SIZEOF_POINTER__=4 -D__SIZEOF_INT__=4 -D__SIZEOF_LONG__=4 -D__SIZEOF_LONG_LONG__=8 -D__SIZEOF_SIZE_T__=4"
I="-I$X/util -I$X/cc/frontend -I$X/cc/backend -I$X/cpp -I$X/as -DXCC_TARGET_ARCH=XCC_ARCH_ARM32 -DXCC_TARGET_PLATFORM=XCC_PLATFORM_POSIX -I$TESTS_ARM/pch"
CM=$FPC/apps/benchmarks/coremark/coremark
n=0; bad=0
for f in $T/*.c $(ls $X/*/*.c $X/*/*/*.c $X/*/*/*/*.c 2>/dev/null | grep -v _debug) $XCCDIR/bench/bench.c \
         $XCCDIR/tests/opt.c $TESTS_ARM/pch/*.c $CM/core_*.c; do
  n=$((n+1))
  $O/cpp $D $I -I$CM -I$XCCDIR/bench/coremark -isystem $XCCDIR/xcc/include --pch $O/pch $f > $O/t.i 2>$O/t.err
  rc=$?
  if grep -q "Sanitizer\|runtime error" $O/t.err; then echo "asan: cpp $f"; head -5 $O/t.err; bad=$((bad+1)); continue; fi
  [ $rc = 0 ] || continue
  $O/cc1 -o $O/t.s $O/t.i 2>$O/t.err
  if grep -q "Sanitizer\|runtime error" $O/t.err; then echo "asan: cc1 $f"; grep -m3 "ERROR\|runtime error\|#[0-3] " $O/t.err; bad=$((bad+1)); continue; fi
  [ -s $O/t.s ] || continue
  $O/as -o $O/t.o $O/t.s 2>$O/t.err
  if grep -q "Sanitizer\|runtime error" $O/t.err; then echo "asan: as $f"; head -5 $O/t.err; bad=$((bad+1)); fi
done
echo "asan: $n files, $bad failures"
