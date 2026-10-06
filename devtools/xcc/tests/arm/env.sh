# apps/devtools/xcc/tests/arm/env.sh: what the ARM32 test scripts share
#   XCC_ARM_WORK  where the host tools, the ARM libc and the outputs go
#                 (default /tmp/xcc-arm)
#   QEMU_ARM      qemu-arm (Linux user mode; default: on the PATH)
#   WCC, WASM_RUN a wcc with its sysroot and a wasm runtime (iwasm) for
#                 difftest.sh to check wcc too (optional)
# arm-none-eabi-gcc/as/ld on the PATH (the fpc tree's env.sh puts them there)
TESTS_ARM=$(cd "$(dirname "$0")" && pwd)
XCCDIR=$(cd "$TESTS_ARM/../.." && pwd)            # apps/devtools/xcc
FPC=$(cd "$XCCDIR/../../.." && pwd)
[ -f "$FPC/env.sh" ] && . "$FPC/env.sh" >/dev/null
A=${XCC_ARM_WORK:-/tmp/xcc-arm}
mkdir -p "$A"
Q="${QEMU_ARM:-qemu-arm} -cpu max"
