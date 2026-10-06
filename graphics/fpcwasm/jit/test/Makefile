############################################################################
# apps/graphics/fpcwasm/jit/test/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# The JIT's test harness: WAMR (the board's configuration: fast
# interpreter, AOT, WASI, the thread manager, bulk memory, reference
# types; the same patches) and the JIT built as a Linux ARM program, run
# under qemu-arm (-cpu max: the Cortex-M33's Thumb-2 and FPv5 are a subset
# of it).  The simulator is x86-64: the JIT cannot run there.
#
#   make                       build $(OUT)/jitrun
#   make spec                  the WebAssembly spec tests, JIT against the
#                              interpreter and the expected results
#   make fuzz N=200            random modules (wasm-tools smith), JIT
#                              against the interpreter
#
# Tools (on the build host): an arm-none-linux-gnueabihf GCC (ARM's
# toolchain), qemu-arm, wabt's wast2json, wasm-tools, WebAssembly's spec
# tests.  WAMR: its zip, unpacked and patched like the board's.
############################################################################

TOOLS   ?= $(HOME)/fpcwork/jit/tools
OUT     ?= $(HOME)/fpcwork/jit/hbuild
CROSS   ?= $(TOOLS)/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
QEMU    ?= $(HOME)/tools/qemu/usr/bin/qemu-arm -cpu max
WAMRZIP ?= $(HOME)/tools/wasm/wamr-2.4.5.zip
PATCHES  = $(sort $(wildcard $(CURDIR)/../../../../interpreters/wamr/*.patch))
WAST2JSON ?= $(TOOLS)/wabt-1.0.42/bin/wast2json
WASMTOOLS ?= $(TOOLS)/wasm-tools-1.261.0-x86_64-linux/wasm-tools
TESTSUITE ?= $(TOOLS)/spec/test/core

W  = $(OUT)/wamr
C  = $(W)/core
I  = $(C)/iwasm
S  = $(C)/shared
JIT = $(CURDIR)/..

CC = $(CROSS)gcc

DEFS = -DBH_MALLOC=wasm_runtime_malloc -DBH_FREE=wasm_runtime_free \
  -DBUILD_TARGET=\"THUMBV7\" -DBUILD_TARGET_THUMB_VFP -DBH_PLATFORM_LINUX \
  -D_GNU_SOURCE -DWASM_HAVE_MREMAP=1 \
  -DWASM_ENABLE_LOG=1 -DWASM_ENABLE_AOT=1 -DWASM_ENABLE_QUICK_AOT_ENTRY=0 \
  -DWASM_ENABLE_WORD_ALIGN_READ=0 -DWASM_ENABLE_DYNAMIC_AOT_DEBUG=0 \
  -DWASM_MEM_DUAL_BUS_MIRROR=0 -DWASM_ENABLE_FAST_INTERP=1 \
  -DWASM_ENABLE_INTERP=1 -DWASM_ENABLE_MINI_LOADER=0 \
  -DWASM_ENABLE_SHARED_MEMORY=0 -DWASM_ENABLE_BULK_MEMORY=1 \
  -DWASM_ENABLE_AOT_STACK_FRAME=0 -DWASM_ENABLE_PERF_PROFILING=0 \
  -DWASM_ENABLE_GC_PERF_PROFILING=0 -DWASM_ENABLE_MEMORY_PROFILING=0 \
  -DWASM_ENABLE_MEMORY_TRACING=0 -DWASM_ENABLE_DUMP_CALL_STACK=0 \
  -DWASM_ENABLE_LIBC_BUILTIN=1 -DWASM_CONFIGURABLE_BOUNDS_CHECKS=0 \
  -DWASM_ENABLE_LIBC_WASI=1 -DWASM_ENABLE_MODULE_INST_CONTEXT=1 \
  -DWASM_ENABLE_MULTI_MODULE=0 -DWASM_ENABLE_THREAD_MGR=1 \
  -DWASM_ENABLE_LIB_WASI_THREADS=0 -DWASM_ENABLE_GC=0 -DWASM_GC_MANUALLY=0 \
  -DWASM_ENABLE_LIB_PTHREAD=0 -DWASM_ENABLE_LIB_PTHREAD_SEMAPHORE=0 \
  -DWASM_DISABLE_HW_BOUND_CHECK=0 -DWASM_DISABLE_STACK_HW_BOUND_CHECK=0 \
  -DWASM_DISABLE_WAKEUP_BLOCKING_OP=0 -DWASM_ENABLE_LOAD_CUSTOM_SECTION=0 \
  -DWASM_ENABLE_CUSTOM_NAME_SECTION=0 -DWASM_ENABLE_GLOBAL_HEAP_POOL=0 \
  -DWASM_MEM_ALLOC_WITH_USAGE=0 -DWASM_ENABLE_SPEC_TEST=1 \
  -DWASM_ENABLE_REF_TYPES=1 -DWASM_ENABLE_TAIL_CALL=0 \
  -DWASM_ENABLE_EXCE_HANDLING=0 -DWASM_ENABLE_TAGS=0

INCS = -I$(C) -I$(I)/include -I$(I)/interpreter -I$(I)/common -I$(I)/aot \
  -I$(I)/libraries/thread-mgr \
  -I$(I)/libraries/libc-wasi/sandboxed-system-primitives/src \
  -I$(I)/libraries/libc-wasi/sandboxed-system-primitives/include \
  -I$(S)/include -I$(S)/platform/include -I$(S)/utils -I$(S)/utils/uncommon \
  -I$(S)/mem-alloc -I$(S)/platform/linux -I$(S)/platform/common/libc-util \
  -I$(JIT)

CFLAGS = -O2 -g -mthumb -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard \
  -Wno-int-conversion -Wno-implicit-function-declaration \
  -Wno-incompatible-pointer-types $(DEFS) $(INCS)

WSRCS = $(S)/platform/linux/platform_init.c \
  $(addprefix $(S)/platform/common/posix/,posix_blocking_op.c posix_clock.c \
    posix_file.c posix_malloc.c posix_memmap.c posix_sleep.c posix_socket.c \
    posix_thread.c posix_time.c) \
  $(S)/platform/common/libc-util/libc_errno.c \
  $(S)/mem-alloc/mem_alloc.c \
  $(addprefix $(S)/mem-alloc/ems/,ems_alloc.c ems_gc.c ems_hmu.c ems_kfc.c) \
  $(addprefix $(S)/utils/,bh_assert.c bh_bitmap.c bh_common.c bh_hashmap.c \
    bh_list.c bh_leb128.c bh_log.c bh_queue.c bh_vector.c runtime_timer.c) \
  $(S)/utils/uncommon/bh_read_file.c \
  $(addprefix $(I)/common/,wasm_application.c wasm_blocking_op.c \
    wasm_runtime_common.c wasm_native.c wasm_exec_env.c wasm_loader_common.c \
    wasm_memory.c wasm_c_api.c) \
  $(addprefix $(I)/interpreter/,wasm_interp_fast.c wasm_runtime.c \
    wasm_loader.c) \
  $(addprefix $(I)/aot/,aot_loader.c aot_runtime.c aot_intrinsic.c \
    arch/aot_reloc_thumb.c) \
  $(I)/libraries/thread-mgr/thread_manager.c \
  $(I)/libraries/libc-builtin/libc_builtin_wrapper.c \
  $(I)/libraries/libc-wasi/libc_wasi_wrapper.c \
  $(addprefix $(I)/libraries/libc-wasi/sandboxed-system-primitives/src/,\
    posix.c random.c str.c blocking_op.c)

WASRCS = $(I)/common/arch/invokeNative_thumb_vfp.s

JSRCS = $(JIT)/wasm_jit.c $(JIT)/wasm_jit_rt.c $(JIT)/test/jitrun.c

WOBJS = $(patsubst $(W)/%.c,$(OUT)/obj/%.o,$(WSRCS)) \
        $(patsubst $(W)/%.s,$(OUT)/obj/%.o,$(WASRCS))
JOBJS = $(patsubst $(CURDIR)/../%.c,$(OUT)/jobj/%.o,$(JSRCS))

all: $(OUT)/jitrun

$(W)/.patched: $(WAMRZIP) $(PATCHES)
	rm -rf $(W) $(OUT)/wasm-micro-runtime-WAMR-2.4.5
	mkdir -p $(OUT)
	cd $(OUT) && unzip -q $(WAMRZIP)
	for p in $(PATCHES); do patch -s -d $(OUT)/wasm-micro-runtime-WAMR-2.4.5 -p1 < $$p || exit 1; done
	mv $(OUT)/wasm-micro-runtime-WAMR-2.4.5 $(W)
	touch $@

$(WSRCS) $(WASRCS): $(W)/.patched

$(OUT)/obj/%.o: $(W)/%.c $(W)/.patched
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/obj/%.o: $(W)/%.s $(W)/.patched
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/jobj/%.o: $(CURDIR)/../%.c $(JIT)/*.h $(W)/.patched
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wall -Wno-unused-function -DWASM_JIT_FORCE -c -o $@ $<

$(OUT)/jitrun: $(WOBJS) $(JOBJS)
	$(CC) $(CFLAGS) -static -o $@ $^ -lm -lpthread

# The spec tests: each .wast to JSON and modules (wast2json), to commands
# (spec.py), run by jitrun

# The testsuite: WebAssembly/spec at the commit WAMR 2.4.5 tests against
# (f3a0e06235d2, with WAMR's tests/wamr-test-suites/spec-test-script/
# ignore_cases.patch); what WAMR's board configuration does not have is
# left out (linking needs multi-module)

SPEC_SKIP =

spec: $(OUT)/jitrun
	@mkdir -p $(OUT)/spec
	python3 $(CURDIR)/spec.py --wast2json $(WAST2JSON) \
	  --jitrun "$(QEMU) $(OUT)/jitrun" --out $(OUT)/spec \
	  $(filter-out $(patsubst %,$(TESTSUITE)/%.wast,$(SPEC_SKIP)),\
	    $(wildcard $(TESTSUITE)/*.wast)) $(wildcard $(CURDIR)/wast/*.wast)

N ?= 100
fuzz: $(OUT)/jitrun
	@mkdir -p $(OUT)/fuzz
	python3 $(CURDIR)/fuzz.py --wasm-tools $(WASMTOOLS) \
	  --jitrun "$(QEMU) $(OUT)/jitrun" --out $(OUT)/fuzz -n $(N)

clean:
	rm -rf $(OUT)/obj $(OUT)/jobj $(OUT)/jitrun

.PHONY: all spec fuzz clean
