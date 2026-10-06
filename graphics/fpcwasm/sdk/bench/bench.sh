#!/bin/bash
# apps/graphics/fpcwasm/sdk/bench/bench.sh [OUT]
#
# SPDX-License-Identifier: Apache-2.0
#
# The WebAssembly benchmarks, reproducibly: builds the inputs on the build
# host (fpga) into OUT (default ~/fpcwork/wasmbench) and prints the board
# session that measures them four ways (docs/wasm.md, "Performance"):
#
#   native       rocksn (the firmware, CONFIG_FPCWASM_ROCKSN), coremark
#   AOT          wasm run -a  (the .aot: wamrc, the board's flags)
#   JIT          wasm run -j  (the .wasm compiled on the board)
#   interpreted  wasm run -i
#
# Inputs: CoreMark (EEMBC's sources: apps/benchmarks/coremark/coremark when
# the NuttX app has downloaded them, else its archive; xcc's port,
# apps/devtools/xcc/bench/coremark), clang -O2 from wasi-sdk:
#   coremark.wasm / .aot     2000 iterations (AOT, JIT: ~6 s)
#   coremark-i.wasm          200 iterations (the interpreter: ~10 s)
#   kernels.wasm / .aot      small kernels, each timed (kernels.c)
# Rocks is the SDK's app (sdk/apps/rocks: app.wasm, app.aot by `make
# apps/rocks`), installed on the board as /mnt/sd0/apps/rocks.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$(dirname "$HERE")
APPS=$(cd "$SDK/../../.." && pwd)
OUT=${1:-$HOME/fpcwork/wasmbench}
WASI_SDK=${WASI_SDK:-$HOME/tools/wasm/wasi-sdk-34.0-x86_64-linux}
WAMRC=${WAMRC:-$HOME/tools/wasm/wamrc}
CC="$WASI_SDK/bin/clang --target=wasm32-wasip1 --sysroot=$WASI_SDK/share/wasi-sysroot -O2"
WAMRC_FLAGS="--target=thumbv8m.main --target-abi=gnueabihf --cpu=cortex-m33 \
  --disable-simd --bounds-checks=1 --stack-bounds-checks=1 --enable-multi-thread"

mkdir -p "$OUT"

# CoreMark's sources

CM=$APPS/benchmarks/coremark/coremark
if [ ! -f "$CM/core_main.c" ]; then
  CM=$OUT/coremark-main
  if [ ! -f "$CM/core_main.c" ]; then
    curl -sL -o "$OUT/coremark.zip" https://github.com/eembc/coremark/archive/main.zip
    (cd "$OUT" && unzip -q -o coremark.zip && rm -rf coremark-main.tmp &&
     mv coremark-main coremark-main.tmp && mv coremark-main.tmp coremark-main)
  fi
fi
PORT=$APPS/devtools/xcc/bench/coremark
CMSRC="$CM/core_list_join.c $CM/core_main.c $CM/core_matrix.c $CM/core_state.c \
  $CM/core_util.c $PORT/core_portme.c"

for it in 2000 200; do
  name=coremark; [ $it = 200 ] && name=coremark-i
  $CC -I"$PORT" -I"$CM" -DITERATIONS=$it -DPERFORMANCE_RUN=1 \
      -DFLAGS_STR="\"clang -O2\"" $CMSRC -Wl,--strip-all -o "$OUT/$name.wasm"
done
$CC -Wl,--strip-all -o "$OUT/kernels.wasm" "$HERE/kernels.c"
if [ -x "$WAMRC" ]; then
  $WAMRC $WAMRC_FLAGS -o "$OUT/coremark.aot" "$OUT/coremark.wasm" > /dev/null
  $WAMRC $WAMRC_FLAGS -o "$OUT/kernels.aot" "$OUT/kernels.wasm" > /dev/null
  make -s -C "$SDK" apps/rocks
fi
ls -l "$OUT"/*.wasm "$OUT"/*.aot "$SDK"/apps/rocks/app.* 2>/dev/null

cat <<EOF

The board (a pc image with the wasm module, rocksn and coremark; the
Rocks app installed): copy the inputs (from this machine, the board's
console free):

  tools/fpcput.py $OUT/coremark.wasm $OUT/coremark.aot $OUT/coremark-i.wasm \\
                  $OUT/kernels.wasm $OUT/kernels.aot -d /mnt/sd0/home
  tools/fpcput.py $SDK/apps/rocks/app.wasm $SDK/apps/rocks/app.aot -d /mnt/sd0/apps/rocks

then at NSH (each line one measurement; Rocks prints "us a frame",
CoreMark "Iterations/Sec", the kernels microseconds each):

  rocksn -b 100
  rocksn -b 400
  wasm run -a rocks -b 100
  wasm run -a rocks -b 400
  wasm run -j rocks -b 100
  wasm run -j rocks -b 400
  wasm run -j --sram rocks -b 400
  wasm run -i rocks -b 100
  wasm run -i rocks -b 400
  cd /mnt/sd0/home
  wasm run -a coremark.aot
  wasm run -j coremark.wasm
  wasm run -i coremark-i.wasm
  wasm run -a kernels.aot
  wasm run -j -v kernels.wasm
  wasm run -i kernels.wasm
  coremark                  (NuttX's native CoreMark: gcc -Os, 2 threads:
                             it/s on both cores together)
EOF
