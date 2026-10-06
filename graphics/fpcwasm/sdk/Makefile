############################################################################
# apps/graphics/fpcwasm/sdk/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# WebAssembly apps for the FPC (docs/wasm.md), built on a PC: wasi-sdk's
# clang makes app.wasm (any wasm32 compiler will do: fpc_wasm.h is plain
# C99), WAMR's wamrc compiles it ahead of time for the board's CPU
# (app.aot: Cortex-M33, Thumb-2, hardware floating point, the firmware's
# ABI).  Each app goes to apps/NAME/, as it is installed on the board
# (/mnt/sd0/apps/NAME/: tools/fpcput.py, or the SD card):
#
#   make                    every app (app.wasm, app.aot, manifest, icon)
#   make WAMRC=             no AOT
#   make apps/rocks         one
############################################################################

WASI_SDK ?= $(HOME)/tools/wasm/wasi-sdk-34.0-x86_64-linux
WAMRC    ?= $(HOME)/tools/wasm/wamrc

CC       = $(WASI_SDK)/bin/clang
CFLAGS   = --target=wasm32-wasip1 --sysroot=$(WASI_SDK)/share/wasi-sysroot \
           -O2 -Wall -I.
LDFLAGS  = -Wl,-z,stack-size=32768 -Wl,--strip-all

# The board: what the firmware is built for (Toolchain.defs: cortex-m33,
# -mfpu=fpv5-sp-d16 -mfloat-abi=hard).  --enable-multi-thread: the code
# checks at every loop and call whether it is to stop (the watchdog,
# Ctrl-C); no SIMD (the CPU has none)

WAMRC_FLAGS = --target=thumbv8m.main --target-abi=gnueabihf \
              --cpu=cortex-m33 --disable-simd --enable-multi-thread

APPS = hello rocks fgdemo crash apitest

all: $(addprefix apps/,$(APPS))

apps/%: examples/%/*.c examples/%/manifest fpc_wasm.h
	@mkdir -p $@
	$(CC) $(CFLAGS) -o $@/app.wasm $(filter %.c,$^) $(LDFLAGS)
	cp examples/$*/manifest $@/manifest
	@if [ -f examples/$*/icon.bmp ]; then cp examples/$*/icon.bmp $@/; fi
	@if [ -n "$(WAMRC)" ]; then \
	  echo "$(WAMRC) $(WAMRC_FLAGS) -o $@/app.aot $@/app.wasm"; \
	  $(WAMRC) $(WAMRC_FLAGS) -o $@/app.aot $@/app.wasm >/dev/null; \
	fi
	@touch $@

clean:
	rm -rf apps

.PHONY: all clean
