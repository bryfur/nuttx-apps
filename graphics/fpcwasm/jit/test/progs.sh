#!/bin/bash
# apps/graphics/fpcwasm/jit/test/progs.sh OUT JITRUN...
#
# SPDX-License-Identifier: Apache-2.0
#
# Whole programs, compiled and interpreted, their output compared (make
# progs): progs/*.c and CoreMark by wasi-sdk's clang -O2; xcc's own tests
# (valtest, fvaltest, its bench and examples) by its wcc when there is
# one (code unlike clang's).  CC, WCC, WCC_SYSROOT, CM from the Makefile.

out=$1; shift
jitrun="$*"
here=$(cd "$(dirname "$0")" && pwd)
apps=$(cd "$here/../../../.." && pwd)
mkdir -p "$out"
bad=0
n=0

# Times differ: the lines of CoreMark's that say them, a time in "us"
filter="sed -E -e /Ticks|ticks|secs|Sec|valid/d -e s/[[:space:]]+[0-9]+[[:space:]]+us//"

run() {           # NAME WASM ARGS...: JIT against the interpreter
  local name=$1 w=$2; shift 2
  local a b
  a=$(cd "$out" && $jitrun run -i "$w" "$@" 2>/dev/null | $filter; \
      echo "status ${PIPESTATUS[0]}")
  b=$(cd "$out" && $jitrun run "$w" "$@" 2>/dev/null | $filter; \
      echo "status ${PIPESTATUS[0]}")
  n=$((n + 1))
  if [ "$a" != "$b" ]; then
    echo "progs: $name $*: DIFFERENT"
    diff <(echo "$a") <(echo "$b") | head -5
    bad=1
  fi
}

for c in "$here"/progs/*.c; do
  w=$out/$(basename "$c" .c).wasm
  $CC -o "$w" "$c" || { bad=1; continue; }
  run "$(basename "$c")" "$w"
done
for k in f s l d i c m t; do run kernels.c "$out/kernels.wasm" $k; done

if [ -n "$CM" ] && [ -f "$CM/core_main.c" ]; then
  port=$apps/devtools/xcc/bench/coremark
  $CC -I"$port" -I"$CM" -DITERATIONS=20 -DPERFORMANCE_RUN=1 \
      -DFLAGS_STR='"clang -O2"' "$CM"/core_list_join.c "$CM"/core_main.c \
      "$CM"/core_matrix.c "$CM"/core_state.c "$CM"/core_util.c \
      "$port/core_portme.c" -o "$out/coremark.wasm" && \
    run coremark "$out/coremark.wasm"
fi

if [ -n "$WCC" ] && [ -x "$WCC" ]; then
  t=$apps/devtools/xcc/xcc/tests
  $WCC -o "$out/valtest.wasm" "$t/valtest.c" && run wcc-valtest "$out/valtest.wasm"
  $WCC -o "$out/fvaltest.wasm" "$t/fvaltest.c" && run wcc-fvaltest "$out/fvaltest.wasm"
  $WCC -o "$out/xbench.wasm" "$apps/devtools/xcc/bench/bench.c" && \
    run wcc-bench "$out/xbench.wasm"
  for e in fib mandelbrot hello; do
    $WCC -o "$out/x$e.wasm" "$apps/devtools/xcc/xcc/examples/$e.c" && \
      run wcc-$e "$out/x$e.wasm"
  done
  $WCC -o "$out/wkernels.wasm" "$here/progs/kernels.c" && \
    for k in f s l d i c m t; do run wcc-kernels "$out/wkernels.wasm" $k; done
fi

echo "progs: $n runs, $([ $bad = 0 ] && echo "all the same" || echo "DIFFERENCES")"
exit $bad
