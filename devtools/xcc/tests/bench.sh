#!/bin/sh
# apps/devtools/xcc/tests/bench.sh [-o OUT] [BUILD]
#
# The benchmarks of docs/xcc.md (bench/bench.c and CoreMark with
# bench/coremark's port) built for the board, on the build machine, every
# way they are compared:
#
#   bx_gcc, cm_gcc      native modules, gcc -O2 with the firmware's module
#                       flags (arm-none-eabi-gcc, CELFFLAGS, gnu-elf.ld)
#   bx_xcc, cm_xcc      native modules made by xcc (cpp, cc1, as, ld built
#                       for this machine from the same sources: the same
#                       output as xcc on the board)
#   bw_wcc.wasm, cw_wcc.wasm      WebAssembly made by wcc (this machine's)
#   bw_clang.wasm, cw_clang.wasm  and by clang -O2 (wasi-sdk), if found, stripped
#
# BUILD: the nuttx/apps pair of a built pc configuration (default
# ~/git/fpc), for its NuttX headers, the xcc sysroot and the module flags.
# OUT: where they go (default /tmp/xcc-bench); then a list of the commands
# that install and run them on the board.
#
#   CM_IT=8000     CoreMark iterations of the native builds (>= 10 s)
#   CM_IT_WASM=300 and of the WebAssembly ones

set -e
OUT=/tmp/xcc-bench
if [ "$1" = -o ]; then OUT=$2; shift 2; fi
BUILD=${1:-$HOME/git/fpc}
X=$(cd "$(dirname "$0")/.." && pwd)                  # apps/devtools/xcc
FPC=$(cd "$X/../../.." && pwd)
[ -f "$FPC/env.sh" ] && . "$FPC/env.sh" >/dev/null
TOP=$BUILD/nuttx
APPS=$BUILD/apps
S=$APPS/devtools/xcc/sysroot
CM=${XCC_COREMARK:-$FPC/apps/benchmarks/coremark/coremark}
B=$X/bench
CM_IT=${CM_IT:-8000}
CM_IT_WASM=${CM_IT_WASM:-300}
for f in $TOP/include/nuttx/config.h $S/arm/lib/crt0.o $S/arm/nuttx $APPS/devtools/xcc/host/wcc; do
  [ -e $f ] || { echo "bench.sh: $f missing (a pc build with CONFIG_DEVTOOLS_XCC first)"; exit 1; }
done
mkdir -p $OUT/obj $OUT/host
CMSRC="$CM/core_list_join.c $CM/core_main.c $CM/core_matrix.c $CM/core_state.c $CM/core_util.c $B/coremark/core_portme.c"

# gcc -O2: the firmware's module flags (Make.defs), -O2 for its -Os
cat > $OUT/obj/flags.mk <<EOF
include \$(APPDIR)/Make.defs
all:
	@echo "\$(CELFFLAGS)"
	@echo "\$(LDELFFLAGS)"
EOF
FLAGS=$(make -s -f $OUT/obj/flags.mk TOPDIR=$TOP APPDIR=$APPS)
CELF=$(echo "$FLAGS" | sed -n 1p | sed 's/-Os/-O2/; s/-g / /')
LDELF=$(echo "$FLAGS" | sed -n 2p)
gccmod() {  # gccmod OUT SOURCES... (-D/-I flags first)
  o=$1; shift; objs=
  for a in "$@"; do
    case $a in
      -*) fl="$fl $a";;
      *) ob=$OUT/obj/$(basename $o)_$(basename $a .c).o
         arm-none-eabi-gcc $fl $CELF -w -c -o $ob $a; objs="$objs $ob";;
    esac
  done
  arm-none-eabi-ld $LDELF -o $o $objs
  fl=
}
gccmod $OUT/bx_gcc $B/bench.c
gccmod $OUT/cm_gcc -I$CM -I$B/coremark -DITERATIONS=$CM_IT -DCOMPILER_VERSION=\"gcc-O2\" $CMSRC

# xcc for this machine, from these sources (what runs on the board)
XS=$X/xcc/src
HF="-O2 -std=gnu11 -D_DEFAULT_SOURCE -w -I$XS/util -I$XS/cc/frontend -I$XS/cc/backend -I$XS/cpp -I$XS/as -DXCC_TARGET_ARCH=XCC_ARCH_ARM32 -DXCC_TARGET_PLATFORM=XCC_PLATFORM_POSIX"
U="$XS/util/util.c $XS/util/table.c"
gcc $HF -o $OUT/host/cpp $XS/cpp/*.c $XS/cc/frontend/lexer.c $XS/cc/frontend/ast.c $U
gcc $HF -o $OUT/host/cc1 $XS/cc/frontend/*.c $XS/cc/backend/*.c $XS/cc/builtin.c $XS/cc/cc1.c $XS/cc/arch/arm32/*.c $U
gcc $HF -o $OUT/host/as $XS/as/*.c $XS/as/arch/arm32/*.c $U $XS/util/bin_util.c
gcc $HF -o $OUT/host/ld $XS/ld/arm32/ld_arm32.c $U $XS/util/archive.c
# (xcc_main.c's predefined macros and include directories)
PRE="-D__XCC -D__ILP32__ -D__arm__ -D__thumb__ -D__thumb2__ -D__ARMEL__ -D__ARM_EABI__ -D__ARM_ARCH=8 -D__ARM_ARCH_8M_MAIN__ -D__ARM_PCS_VFP -D__VFP_FP__ -D__ARM_FP=4 -D__SOFTFP_DOUBLE__ -D__CHAR_UNSIGNED__ -D__STDC__ -D__STDC_VERSION__=199901L -D__SIZEOF_POINTER__=4 -D__SIZEOF_INT__=4 -D__SIZEOF_LONG__=4 -D__SIZEOF_LONG_LONG__=8 -D__SIZEOF_SIZE_T__=4 -D__NuttX__ -D__asm__=__asm -D__volatile__=volatile -D__inline__=inline -D__restrict=restrict -D__restrict__=restrict"
xccmod() {  # xccmod OUT SOURCES... (-D/-I flags first)
  o=$1; shift; objs=
  for a in "$@"; do
    case $a in
      -*) fl="$fl $a";;
      *) ob=$OUT/obj/$(basename $o)_$(basename $a .c)
         $OUT/host/cpp $PRE $fl -isystem $S/arm/include -isystem $S/arm/nuttx -o $ob.i $a
         $OUT/host/cc1 -o $ob.s $ob.i
         $OUT/host/as -o $ob.o $ob.s; objs="$objs $ob.o";;
    esac
  done
  $OUT/host/ld -o $o --stack=8192 $S/arm/lib/crt0.o $objs -L $S/arm/lib
  fl=
}
xccmod $OUT/bx_xcc $B/bench.c
xccmod $OUT/cm_xcc -I$CM -I$B/coremark -DITERATIONS=$CM_IT -DCOMPILER_VERSION=\"xcc\" $CMSRC

# WebAssembly
W=$APPS/devtools/xcc/host/wcc
WF="-nostdinc -isystem $S/include -L$S/lib"
CMF="-I$CM -I$B/coremark -DITERATIONS=$CM_IT_WASM"
$W $WF -o $OUT/bw_wcc.wasm $B/bench.c
$W $WF $CMF -DCOMPILER_VERSION=\"wcc\" -o $OUT/cw_wcc.wasm $CMSRC
WASI=$HOME/tools/wasm/wasi-sdk-34.0-x86_64-linux
if [ -x $WASI/bin/clang ]; then
  CL="$WASI/bin/clang --target=wasm32-wasip1 --sysroot=$WASI/share/wasi-sysroot -O2"
  $CL -o $OUT/bw_clang.wasm $B/bench.c -Wl,-z,stack-size=65536
  $CL $CMF -DCOMPILER_VERSION=\"clang-O2\" -D_WASI_EMULATED_PROCESS_CLOCKS -o $OUT/cw_clang.wasm $CMSRC -lwasi-emulated-process-clocks
  # clang keeps its names and debug sections (bench.wasm: 175 KB, 31 KB stripped)
  $WASI/bin/llvm-strip $OUT/bw_clang.wasm $OUT/cw_clang.wasm
fi

rm -rf $OUT/obj $OUT/host
cd $OUT && ls -l
cat <<EOF

Install (where the board is: lrzsz's sz on the PATH):
  tools/fpcput.py $(cd $OUT && ls | sed "s|^|$OUT/|" | tr '\n' ' ')-d /mnt/sd0/home/bench
  (nsh> mkdir /mnt/sd0/home/bench first)
Run (nsh; a module by its full path):
  set D /mnt/sd0/home/bench
  \$D/bx_gcc 10                  # bench kernels: ms each, a total (scale 10)
  \$D/bx_xcc 10
  wasm run \$D/bw_wcc.wasm 10    # (the runtime's own options pick interpreter, JIT, AOT)
  wasm run \$D/bw_clang.wasm 10
  \$D/cm_gcc                     # CoreMark: Iterations/Sec ($CM_IT iterations)
  \$D/cm_xcc
  wasm run \$D/cw_wcc.wasm       # ($CM_IT_WASM iterations)
  wasm run \$D/cw_clang.wasm
The checksums (bench) and CRCs (CoreMark) must be the same for every build.
EOF
