#!/bin/sh
# leak.sh: the board's xcc and wcc (the driver running cpp, cc1, as and ld
# in one process, the memory files, the precompiled headers; wcc) built for
# this machine as the board builds them (xcc_nuttx.c, the programs as
# blobs: xcc.ld) with LeakSanitizer, and run as the board runs a module: on
# the board a module's memory outlives it (a flat NuttX build), so after
# each compile nothing may still be allocated, not even what a global
# still points to (leakmain.c counts the blocks and bytes; a leak is
# listed with where it was allocated).
#   XCC_SYSROOT  the sysroot a firmware build makes (default
#                apps/devtools/xcc/sysroot: arm/ for xcc, include/ and
#                lib/ for wcc)
. "$(dirname "$0")/env.sh"
X=$XCCDIR/xcc/src
S=${XCC_SYSROOT:-$XCCDIR/sysroot}
O=$A/leak
[ -e $S/arm/lib/crt0.o ] && [ -e $S/lib/wlibc.a ] ||
  { echo "leak: no sysroot in $S (a firmware build with CONFIG_DEVTOOLS_XCC, or XCC_SYSROOT)"; exit 1; }
rm -rf $O
mkdir -p $O/obj $O/stub/nuttx $O/root $O/work
echo "#define CONFIG_DEVTOOLS_XCC_ROOT \"$O/root\"" > $O/stub/nuttx/config.h
# The sysroot, its cache directory empty
ln -s $S/arm $O/root/arm; ln -s $S/include $O/root/include; ln -s $S/lib $O/root/lib

# As the Makefile builds them (XCCFLAGS, BLOB_RULES), for this machine
CF="-O1 -g -fsanitize=leak -fno-omit-frame-pointer -std=gnu11 -D_DEFAULT_SOURCE -D_GNU_SOURCE -D__NuttX__ -fno-common -w -I$O/stub"
XF="$CF -include $XCCDIR/xcc_nuttx.h"
ARM="-DXCC_TARGET_ARCH=XCC_ARCH_ARM32 -DXCC_TARGET_PLATFORM=XCC_PLATFORM_POSIX"
U="$X/util/util.c $X/util/table.c"
fail=0
blob() {  # blob NAME MAINFILE "FLAGS" SOURCES...
  n=$1 m=$2 f=$3; shift 3; objs=
  for s in "$@"; do
    o=$O/obj/$n.$(echo $s | sed "s|$XCCDIR/||; s|/|_|g").o
    d=; [ $s = $m ] && d=-Dmain=xcc_${n}_main
    gcc -c $XF $f $d -o $o $s || return 1
    objs="$objs $o"
  done
  gcc -c $CF -DXCC_MARK=$n -o $O/obj/$n.marks.o $XCCDIR/xcc_marks.c || return 1
  ld -r -T $XCCDIR/xcc.ld -o $O/obj/$n.tmp.o $objs $O/obj/$n.marks.o || return 1
  objcopy -G xcc_${n}_main -G ${n}_data_start -G ${n}_data_end -G ${n}_bss_start -G ${n}_bss_end \
    $O/obj/$n.tmp.o $O/$n.blob.o
}
PPF="-I$X/util -I$X/cc/frontend -I$X/cpp"
blob wcc $X/wcc/wcc.c "$PPF -DXCC_TARGET_ARCH=XCC_ARCH_WASM" $X/wcc/*.c $X/cc/frontend/*.c \
  $X/cpp/preprocessor.c $X/cpp/pp_parser.c $X/cpp/macro.c $X/cpp/pch.c $U $X/util/archive.c \
  $X/cc/backend/codegen.c $X/cc/backend/codegen_expr.c $X/cc/backend/ir.c $X/cc/backend/optimize.c \
  $X/cc/backend/regalloc.c $X/cc/backend/ssa.c &&
blob xccd $XCCDIR/xcc_main.c "" $XCCDIR/xcc_main.c &&
blob cpp $X/cpp/cpp.c "$PPF $ARM" $X/cpp/*.c $X/cc/frontend/lexer.c $X/cc/frontend/ast.c $U &&
blob cc1 $X/cc/cc1.c "-I$X/util -I$X/cc/frontend -I$X/cc/backend $ARM" $X/cc/frontend/*.c \
  $X/cc/backend/*.c $X/cc/builtin.c $X/cc/cc1.c $X/cc/arch/arm32/*.c $U &&
blob as $X/as/as.c "-I$X/util -I$X/as $ARM" $X/as/*.c $X/as/arch/arm32/*.c $U &&
blob ld $X/ld/arm32/ld_arm32.c "-I$X/util $ARM" $X/ld/arm32/ld_arm32.c $U $X/util/archive.c ||
  { echo "leak: build failed"; exit 1; }
gcc -c $CF -DXCC_MODULE -Wno-incompatible-pointer-types -o $O/obj/xcc_nuttx.o $XCCDIR/xcc_nuttx.c &&
gcc -c $CF -o $O/obj/leakmain.o $TESTS_ARM/leakmain.c &&
gcc -c $XF -Dmain=board_main -o $O/obj/xcc_entry.o $XCCDIR/xcc_entry.c &&
gcc -c $XF -Dmain=board_main -o $O/obj/wcc_main.o $XCCDIR/wcc_main.c &&
gcc -fsanitize=leak -o $O/xcc $O/obj/leakmain.o $O/obj/xcc_entry.o $O/obj/xcc_nuttx.o \
  $O/xccd.blob.o $O/cpp.blob.o $O/cc1.blob.o $O/as.blob.o $O/ld.blob.o &&
gcc -fsanitize=leak -o $O/wcc $O/obj/leakmain.o $O/obj/wcc_main.o $O/obj/xcc_nuttx.o $O/wcc.blob.o ||
  { echo "leak: build failed"; exit 1; }

# The compiles: each a start of the module, as on the board
B=$XCCDIR/bench
CM=${XCC_COREMARK:-$FPC/apps/benchmarks/coremark/coremark}  # (or a copy of its sources)
CMSRC="$CM/core_list_join.c $CM/core_main.c $CM/core_matrix.c $CM/core_state.c $CM/core_util.c $B/coremark/core_portme.c"
CMF="-I$CM -I$B/coremark -DITERATIONS=10"
W=$O/work
cat > $W/hello.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int main(int argc, char **argv) { printf("hello %d %g\n", argc, sqrt(2.0)); return 0; }
EOF
cat > $W/bad.c <<'EOF'
#include <stdio.h>
int main(void) { printf("%d\n", undeclared_name); return 0 }
EOF
printf '#include <stdio.h>\n#include "missing.h"\nint x;\n' > $W/badinc.c
printf '#if 1\n#error stop here\n#endif\n' > $W/error.c
n=0
run() {  # run EXPECT PROGRAM ARGS...: the program's exit code must be EXPECT (0 or "fail")
  e=$1; shift; n=$((n+1))
  (cd $W && "$@") > $O/out.txt 2>&1; rc=$?
  if [ $rc = 99 ] || [ $rc = 23 ] || grep -q "LEAK\|LeakSanitizer" $O/out.txt; then
    fail=$((fail+1)); echo "leak: $*"; grep "LEAK" $O/out.txt
    # Again for the list of what is held (globals, stacks not scanned)
    (cd $W && LSAN_OPTIONS=use_globals=0:use_stacks=0:use_registers=0:use_tls=0 "$@") 2>&1 |
      grep -A12 "^Direct\|^Indirect" | head -${XCC_LEAK_LINES:-60}
  elif { [ $e = 0 ] && [ $rc != 0 ]; } || { [ $e = fail ] && [ $rc = 0 ]; }; then
    fail=$((fail+1)); echo "leak: exit $rc (expected $e): $*"; head -5 $O/out.txt
  fi
}
XC=$O/xcc WC=$O/wcc
run 0 $XC --help
run 0 $XC -o hello hello.c                          # makes the precompiled headers
run 0 $XC -o hello hello.c                          # from them
run 0 $XC -fno-pch -o hello hello.c
run 0 $XC -S -o hello.s hello.c
run 0 $XC -E -o hello.i hello.c
run 0 $XC -c -o bench.o $B/bench.c
run 0 $XC -o bench bench.o
run 0 $XC -o bench $B/bench.c
run 0 $XC $CMF -o coremark $CMSRC
run 0 $XC $CMF -o coremark $CMSRC
run fail $XC -o bad bad.c                            # an error (pruned: cpp and cc1 again)
run fail $XC -fno-pch -o bad bad.c
run fail $XC -o bad badinc.c
run fail $XC -o bad error.c
run fail $XC -o bad nonexistent.c
run 0 $WC --version
run 0 $WC -o hello.wasm hello.c
run 0 $WC -o hello.wasm hello.c
run 0 $WC -c -o wbench.o $B/bench.c
run 0 $WC -o bench.wasm wbench.o
run 0 $WC -o bench.wasm $B/bench.c
run 0 $WC $CMF -o coremark.wasm $CMSRC
run 0 $WC -E -o hello.wi hello.c
run fail $WC -o bad.wasm bad.c
run fail $WC -o bad.wasm badinc.c
run fail $WC -o bad.wasm nonexistent.c
[ -s $W/hello ] && [ -s $W/coremark ] && [ -s $W/coremark.wasm ] ||
  { fail=$((fail+1)); echo "leak: outputs missing"; }
echo "leak: $n compiles, $fail failures"
[ $fail = 0 ]
