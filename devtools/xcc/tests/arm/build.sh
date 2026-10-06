#!/bin/sh
# Host build of xcc for the ARM32 target (cpp, cc1, as, ld, xcc) from the vendored tree
. "$(dirname "$0")/env.sh"
set -e
X=$XCCDIR/xcc/src
O=$A
F="-O1 -g -std=gnu11 -D_DEFAULT_SOURCE -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -I$X/util -I$X/cc/frontend -I$X/cc/backend -I$X/cpp -I$X/as -DXCC_TARGET_ARCH=XCC_ARCH_ARM32 -DXCC_TARGET_PLATFORM=XCC_PLATFORM_POSIX"
U="$X/util/util.c $X/util/table.c"
gcc $F -o $O/cc1 $X/cc/frontend/*.c $X/cc/backend/*.c $X/cc/*.c $X/cc/arch/arm32/*.c $U
gcc $F -o $O/cpp $X/cpp/*.c $X/cc/frontend/lexer.c $X/cc/frontend/ast.c $U 2>/dev/null || gcc $F -o $O/cpp $X/cpp/*.c $(ls $X/cc/frontend/*.c) $U
if [ -d $X/as/arch/arm32 ]; then gcc $F -o $O/as $X/as/*.c $X/as/arch/arm32/*.c $U $X/util/bin_util.c; fi
if [ -d $X/ld/arm32 ]; then gcc $F -o $O/ld $X/ld/arm32/*.c $U $X/util/archive.c; fi
gcc $F -o $O/dump_ir $X/_debug/dump_ir.c $X/cc/frontend/*.c $X/cc/backend/*.c $X/cc/builtin.c $X/cc/arch/arm32/*.c $U
