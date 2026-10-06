#!/bin/sh
# difftest.sh: apps/devtools/xcc/tests/opt.c built by xcc (ARM32, qemu-arm)
# and by the host's gcc: the same output
. "$(dirname "$0")/env.sh"
T=$XCCDIR/tests
W=$(mktemp -d)
gcc -O2 -o $W/ref $T/opt.c && $W/ref > $W/ref.txt
$TESTS_ARM/xcc-arm.sh -o $W/x $T/opt.c && $Q $W/x > $W/x.txt
if cmp -s $W/ref.txt $W/x.txt; then echo "difftest: same ($(wc -l < $W/x.txt) lines)"; else echo "difftest: DIFFERENT"; diff $W/ref.txt $W/x.txt | head; fi
# wcc (with its sysroot: a build's apps/devtools/xcc/host/wcc) and a runtime
if [ -n "$WCC" ] && [ -n "$WASM_RUN" ]; then
  $WCC -o $W/w.wasm $T/opt.c && $WASM_RUN $W/w.wasm > $W/w.txt
  if cmp -s $W/ref.txt $W/w.txt; then echo "difftest wcc: same"; else echo "difftest wcc: DIFFERENT"; diff $W/ref.txt $W/w.txt | head; fi
fi
rm -rf $W
