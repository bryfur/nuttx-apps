#!/bin/sh
# Precompiled prefixes (cpp/pch.c): each test of pch/ compiled without the
# cache, making it, and from it, must run the same; a local header changed
# (same size) must be seen; -E (no pruning) from the cache must be what it
# was when made.
. "$(dirname "$0")/env.sh"
P=$TESTS_ARM/pch
W=$(mktemp -d)
bad=0
cp $P/*.h $P/*.c $W/
cd $W
for f in p1 p2 p3 p4; do
  XCC_PCH=0 $TESTS_ARM/xcc-arm.sh -o $f.0 $f.c && $Q ./$f.0 > $f.0.out || { echo "pchtest: $f without the cache"; bad=$((bad+1)); continue; }
  rm -rf $A/pch
  $TESTS_ARM/xcc-arm.sh -E -o $f.1.i $f.c
  $TESTS_ARM/xcc-arm.sh -o $f.1 $f.c && $Q ./$f.1 > $f.1.out
  $TESTS_ARM/xcc-arm.sh -o $f.2 $f.c && $Q ./$f.2 > $f.2.out
  $TESTS_ARM/xcc-arm.sh -E -o $f.2.i $f.c
  cmp -s $f.0.out $f.1.out && cmp -s $f.0.out $f.2.out || { echo "pchtest: $f differs"; bad=$((bad+1)); }
  cmp -s $f.1.i $f.2.i || { echo "pchtest: $f -E differs"; bad=$((bad+1)); }
done
# p2 and p4: the same prefix but for comments, one cache file
rm -rf $A/pch
$TESTS_ARM/xcc-arm.sh -o p2.5 p2.c && $TESTS_ARM/xcc-arm.sh -o p4.5 p4.c && $Q ./p4.5 > p4.5.out
[ $(ls $A/pch | wc -l) = 1 ] && cmp -s p4.0.out p4.5.out || { echo "pchtest: p4 after p2"; bad=$((bad+1)); }
# local.h changed, the same size: seen
sed -i 's/LOCAL_VALUE 1/LOCAL_VALUE 7/' local.h
$TESTS_ARM/xcc-arm.sh -o p1.3 p1.c && $Q ./p1.3 > p1.3.out
grep -q "local 7" p1.3.out || { echo "pchtest: local.h's change not seen"; bad=$((bad+1)); }
# Pruned: hello's output is small
n=$($A/cpp -D__XCC --pch $A/pch -isystem $XCCDIR/xcc/include p2.c | wc -l)
[ $n -lt 200 ] || { echo "pchtest: p2 not pruned ($n lines)"; bad=$((bad+1)); }
# The system headers taken as they were when the key says so (--pch-key,
# what the board's driver passes): the same output, made and from the cache
rm -rf $A/pch
for k in 1 2; do
  $A/cpp -D__XCC --pch $A/pch --pch-key k1 -isystem $XCCDIR/xcc/include p2.c > p2.k$k.i
done
$A/cpp -D__XCC -isystem $XCCDIR/xcc/include p2.c > p2.nopch.i
cmp -s p2.k1.i p2.k2.i && $A/cc1 -o p2.k.s p2.k2.i && [ -s p2.k.s ] || { echo "pchtest: --pch-key"; bad=$((bad+1)); }
echo "pchtest: $bad failures"
rm -rf $W
