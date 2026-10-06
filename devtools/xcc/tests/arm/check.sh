#!/bin/sh
# Everything: host tools, the ARM libc, the tests under qemu-arm, the stage-2 self-compile
. "$(dirname "$0")/env.sh"

$TESTS_ARM/build.sh >/dev/null 2>&1 || { echo "build failed"; exit 1; }
$TESTS_ARM/libc.sh | tail -1
XAS=xcc $TESTS_ARM/armtests.sh 2>&1 | tail -7
$TESTS_ARM/stage2.sh 2>&1 | tail -4
$TESTS_ARM/difftest.sh
$TESTS_ARM/pchtest.sh
