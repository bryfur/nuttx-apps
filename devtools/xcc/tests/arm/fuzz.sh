#!/bin/sh
# fuzz.sh [FIRST [COUNT]]: random programs (fuzz.py) built by gcc -O2 for
# this machine and by xcc for ARM32 (qemu-arm): their outputs the same
. "$(dirname "$0")/env.sh"
first=${1:-1}; count=${2:-100}
W=$A/fuzz; mkdir -p $W
bad=0; n=0
for s in $(seq $first $((first + count - 1))); do
  python3 $TESTS_ARM/fuzz.py $s > $W/f.c
  gcc -O2 -w -o $W/f.host $W/f.c || { echo "fuzz: gcc failed on $s"; continue; }
  timeout 10 $W/f.host > $W/f.host.txt || { echo "fuzz: $s runs too long (host)"; continue; }
  if ! $TESTS_ARM/xcc-arm.sh -o $W/f.arm $W/f.c 2> $W/f.err; then
    echo "fuzz: xcc failed on $s"; head -3 $W/f.err; bad=$((bad+1)); cp $W/f.c $W/fail-$s.c; continue
  fi
  timeout 20 $Q $W/f.arm > $W/f.arm.txt 2>&1
  n=$((n+1))
  if ! cmp -s $W/f.host.txt $W/f.arm.txt; then
    echo "fuzz: $s differs"; bad=$((bad+1)); cp $W/f.c $W/fail-$s.c
  fi
done
echo "fuzz: $n programs, $bad failures"
