#!/bin/bash
# apps/devtools/xcc/tests/wasm/check.sh: wcc's tests on a PC
#   XCC_WASM_WORK  where the host wcc, its sysroot and outputs go
#                  (default /tmp/xcc-wasm)
#   WASM_RUN       a WASI runtime (WAMR's iwasm; required)
#   XCC_FUZZ       how many fuzz programs (default 100)
# A host wcc and its C library from these sources, then xcc's test.sh,
# valtest, fvaltest and example_test.sh with wcc in WAMR, tests/opt.c
# against gcc and the differential fuzzer (tests/arm/fuzz.sh) on wcc.
[ -n "$WASM_RUN" ] || { echo "wasm check: set WASM_RUN (iwasm)"; exit 1; }
T=$(cd "$(dirname "$0")" && pwd)
X=$(cd "$T/../.." && pwd)                     # apps/devtools/xcc
W=${XCC_WASM_WORK:-/tmp/xcc-wasm}
mkdir -p $W/wlibc $W/sysroot/lib
S=$X/xcc/src
L=$X/xcc/libsrc
gcc -O2 -std=gnu11 -D_DEFAULT_SOURCE -w -I$S/util -I$S/cc/frontend -I$S/cpp \
    -DXCC_TARGET_ARCH=XCC_ARCH_WASM -o $W/wcc $S/wcc/*.c $S/cc/frontend/*.c \
    $S/cpp/preprocessor.c $S/cpp/pp_parser.c $S/cpp/macro.c $S/cpp/pch.c \
    $S/util/util.c $S/util/table.c $S/util/archive.c \
    $S/cc/backend/codegen.c $S/cc/backend/codegen_expr.c $S/cc/backend/ir.c \
    $S/cc/backend/optimize.c $S/cc/backend/regalloc.c $S/cc/backend/ssa.c || { echo "wasm check: build failed"; exit 1; }
rm -f $W/wlibc/*.o
for f in $L/math/*.c $L/misc/*.c $L/stdio/*.c $L/stdlib/*.c $L/string/*.c $L/_wasm/unistd/*.c $L/_wasm/crt0/*.c; do
  $W/wcc -nostdinc -isystem $X/xcc/include -c -o $W/wlibc/$(basename $f .c).o $f || echo "wasm check: libc $f failed"
done
crt0=$(for f in $L/_wasm/crt0/*.c; do echo $W/wlibc/$(basename $f .c).o; done)
libc=$(ls $W/wlibc/*.o | grep -v -F "$crt0")
python3 $X/mkar.py $W/sysroot/lib/wlibc.a $libc
python3 $X/mkar.py $W/sysroot/lib/wcrt0.a $crt0
cat > $W/wcc.sh <<EOS
#!/bin/sh
exec $W/wcc -nostdinc -isystem $X/xcc/include -L$W/sysroot/lib "\$@"
EOS
chmod +x $W/wcc.sh
echo "libc: $(echo $libc | wc -w) objects"

cd $X/xcc/tests
A=$W/a.wasm
XCC="$W/wcc.sh" RE_SKIP='\/\/-WCC' AOUT=$A RUN_AOUT="$WASM_RUN $A" ./test.sh > $W/test.log 2>&1
sum() { grep -ao "OK: [0-9]*" $1 | awk '{s += $2} END {print s + 0}'; }
err() { grep -ac "ERROR: " $1; }
echo "test.sh: $(sum $W/test.log) ok, $(err $W/test.log) failed suites"
for t in valtest:"valtest.c" fvaltest:"fvaltest.c"; do
  n=${t%%:*}
  srcs=$(sed -n "s/^${n}_WCCSRCS:=\(.*\)/\1/p" Makefile)
  case $srcs in '$(VAL_SRCS)') srcs=$(sed -n 's/^VAL_SRCS:=\(.*\)/\1/p' Makefile);; '$(FVAL_SRCS)') srcs=$(sed -n 's/^FVAL_SRCS:=\(.*\)/\1/p' Makefile);; esac
  $W/wcc.sh -o $W/$n.wasm -Wall -Werror $srcs > $W/$n.log 2>&1 && $WASM_RUN $W/$n.wasm >> $W/$n.log 2>&1
  echo "$n: exit $?, $(sum $W/$n.log) ok, $(err $W/$n.log) failed suites"
done
XCC="$W/wcc.sh" RE_SKIP='\/\/-WCC' AOUT=$A RUN_AOUT="$WASM_RUN --dir=. $A" ./example_test.sh > $W/examples.log 2>&1
echo "examples: $(sum $W/examples.log) ok, $(err $W/examples.log) failed"

# opt.c against gcc
gcc -O2 -w -o $W/opt.host $X/tests/opt.c && $W/opt.host > $W/opt.host.txt
$W/wcc.sh -o $W/opt.wasm $X/tests/opt.c && $WASM_RUN $W/opt.wasm > $W/opt.wasm.txt
if cmp -s $W/opt.host.txt $W/opt.wasm.txt; then echo "opt.c: same ($(wc -l < $W/opt.host.txt) lines)"
else echo "opt.c: DIFFERENT"; diff $W/opt.host.txt $W/opt.wasm.txt | head; fi

# the fuzzer (wcc only: XCC_FUZZ_ARM=0)
WCC="$W/wcc.sh" WASM_RUN="$WASM_RUN" XCC_FUZZ_NOARM=1 $X/tests/arm/fuzz.sh 1 ${XCC_FUZZ:-100} | tail -4
