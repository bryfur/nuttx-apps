#!/bin/sh
# All the ARM32 codegen tests under qemu-arm: xcc's own (valtest, dvaltest,
# fvaltest, test.sh, examples) and the ABI test against gcc.
# XCC=driver (default: the phase-1 wrapper)
. "$(dirname "$0")/env.sh"
XCC=${XCC:-$TESTS_ARM/xcc-arm.sh}
T=$XCCDIR/xcc/tests
W=$(mktemp -d)
strip() { sed "s/\x1b\[1G\x1b\[2K/\n/g; s/\x1b\[[0-9;]*m//g"; }
cd $T
$XCC -o $W/valtest -Wall -Werror valtest.c && $Q $W/valtest 2>&1 | strip | grep -E "FAILED|ERROR:|signal" ; echo "valtest: $?"
$XCC -o $W/dvaltest -Wall -Werror fvaltest.c && $Q $W/dvaltest 2>&1 | strip | grep -E "FAILED|ERROR:|signal" ; echo "dvaltest: $?"
$XCC -o $W/fvaltest -Wall -Werror -DUSE_SINGLE fvaltest.c && $Q $W/fvaltest 2>&1 | strip | grep -E "FAILED|ERROR:|signal" ; echo "fvaltest: $?"
XCC=$XCC RUN_AOUT="$Q $W/aout" AOUT=$W/aout RE_SKIP="//-ARM32" ./test.sh 2>&1 | strip | grep -E "FAILED|ERROR:|signal" ; echo "test.sh: $?"
XCC=$XCC RUN_EXE="$Q" AOUT=$W/aout2 RUN_AOUT="$Q $W/aout2" ./example_test.sh 2>&1 | strip | grep -E "FAILED|ERROR:|signal" ; echo "examples: $?"
cd $TESTS_ARM/abi && arm-none-eabi-gcc -O2 -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fno-short-enums -c -o $W/abi_g.o abi_g.c && $XCC -c -o $W/abi_x.o abi_x.c && $XCC -o $W/abi $W/abi_x.o $W/abi_g.o && $Q $W/abi 2>&1 | grep -v "0 failures"; echo "abi: $?"
rm -rf $W
echo "(each line: 1 = no failure lines)"
