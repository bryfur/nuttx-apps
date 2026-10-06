#!/bin/sh
# Host test driver for the ARM32 target (phase 1: GNU as and ld, Linux EABI
# for qemu-arm): xcc-arm.sh [-c|-S|-E] [-o OUT] [-I DIR] [-D X] FILES...
. "$(dirname "$0")/env.sh"
if [ "$XAS" = gnu ]; then ASM="arm-none-eabi-as -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard"; else ASM=$A/as; fi
X=$XCCDIR/xcc
D="-D__XCC -D__ILP32__ -D__arm__ -D__thumb__ -D__thumb2__ -D__ARMEL__ -D__ARM_EABI__ -D__ARM_ARCH=8 -D__ARM_PCS_VFP -D__VFP_FP__ -D__CHAR_UNSIGNED__ -D__linux__ -D__unix__ -D__STDC__ -D__STDC_VERSION__=199901L -D__SIZEOF_POINTER__=4 -D__SIZEOF_INT__=4 -D__SIZEOF_LONG__=4 -D__SIZEOF_LONG_LONG__=8 -D__SIZEOF_SIZE_T__=4"
mode=link; out=; cppargs=; ccargs=; files=; objs=; tmp=$(mktemp -d)
# Precompiled headers (cpp/pch.c) in $A/pch unless XCC_PCH=0
PCH=; [ "${XCC_PCH:-1}" = 1 ] && { mkdir -p $A/pch; PCH="--pch $A/pch"; }
while [ $# -gt 0 ]; do
  case "$1" in
    -c) mode=obj;; -S) mode=asm;; -E) mode=pp;;
    -o) out=$2; shift;; -o*) out=${1#-o};;
    -I) cppargs="$cppargs -I $2"; shift;; -I*) cppargs="$cppargs -I ${1#-I}";;
    -D) cppargs="$cppargs -D$2"; shift;; -D*) cppargs="$cppargs $1";;
    -W*) ccargs="$ccargs $1";;
    -xc|-xC) ;;
    -x) shift;;
    -f*) ccargs="$ccargs $1";;
    -O*|-g*|-std=*|-ansi|-pedantic|-MMD) ;;
    -l*) ;;
    *.c|*.s|*.o|*.a|-) files="$files $1";;
    *) echo "xcc-arm: ? $1" >&2;;
  esac
  shift
done
rc=0
case " $files " in *" - "*) cat > $tmp/stdin.c; files=$(echo "$files" | sed "s| - | $tmp/stdin.c |; s| -\$| $tmp/stdin.c|; s|^- |$tmp/stdin.c |");; esac
for f in $files; do
  b=$(basename $f); b=${b%.*}; [ "$f" = - ] && b=stdin
  case $f in
    *.c|-)
      if [ $mode = pp ]; then $A/cpp $D $cppargs $PCH --no-prune -isystem $X/include $f > ${out:-/dev/stdout}; exit $?; fi
      $A/cpp $D $cppargs $PCH -isystem $X/include $f > $tmp/$b.i || { rc=1; break; }
      cc1out=$tmp/$b.s; [ $mode = asm ] && cc1out=${out:-$b.s}
      $A/cc1 $ccargs $tmp/$b.i > $cc1out; r=$?
      if [ $r = 3 ]; then  # an error with pruned headers (cpp/pch.c): again with all
        $A/cpp $D $cppargs $PCH --no-prune -isystem $X/include $f > $tmp/$b.i || { rc=1; break; }
        $A/cc1 $ccargs $tmp/$b.i > $cc1out; r=$?
        [ -n "$PCH_LOG" ] && { echo "retry $f: $r"; [ $r = 0 ] && cat $f; } >> $PCH_LOG
      fi
      if [ $mode = asm ]; then [ $r = 0 ] || rc=1; continue; fi
      [ $r = 0 ] || { rc=1; break; }
      o=$tmp/$b.o; [ $mode = obj ] && o=${out:-$b.o}
      $ASM -o $o $tmp/$b.s || { rc=1; break; }
      objs="$objs $o";;
    *.s)
      o=$tmp/$b.o; [ $mode = obj ] && o=${out:-$b.o}
      $ASM -o $o $f || { rc=1; break; }
      objs="$objs $o";;
    *) objs="$objs $f";;
  esac
done
if [ $rc = 0 ] && [ $mode = link ]; then
  G=$(arm-none-eabi-gcc -mcpu=cortex-m33 -mfloat-abi=hard -mfpu=fpv5-sp-d16 -mthumb -print-libgcc-file-name)
  arm-none-eabi-ld --no-warn-mismatch -o ${out:-a.out} $A/lib/crt0.o $objs $A/lib/libc.a $G 2>&1 | grep -v "enums\|warning: .* attributes" >&2
  [ -f ${out:-a.out} ] || rc=1
fi
rm -rf $tmp
exit $rc
