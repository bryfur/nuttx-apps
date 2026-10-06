############################################################################
# apps/graphics/fpcwasm/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# wasm: WebAssembly apps (docs/wasm.md): WAMR (apps/interpreters/wamr,
# whose objects the module links from libapps), WASI, and the fpc API on
# the game SDK (wasm_fpc.c).  sdk/: the apps' header, examples, and how
# they are built (wasi-sdk, wamrc).

PROGNAME  = wasm
PRIORITY  = 100
STACKSIZE = $(CONFIG_FPCWASM_STACKSIZE)
MODULE    = $(CONFIG_GRAPHICS_FPCWASM)

MAINSRC   = wasm_main.c
CSRCS     = wasm_fpc.c wasm_image.c

CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/interpreters/wamr/wamr/core/iwasm/include
CFLAGS   += ${INCDIR_PREFIX}sdk
CFLAGS   += ${INCDIR_PREFIX}jit

# jit/ (the JIT) is built with WAMR (apps/interpreters/wamr/Makefile): it
# uses the runtime's own structures, so its configuration

# rocksn: the Rocks example built natively on fpc_native.c (the same C
# source, the fpc API straight onto the game SDK), to compare with it as
# a WebAssembly app

ifneq ($(CONFIG_FPCWASM_ROCKSN),)
PROGNAME += rocksn
PRIORITY += 100
STACKSIZE += 8192
MAINSRC  += rocks.c
CSRCS    += fpc_native.c
VPATH    += :sdk:sdk/examples/rocks
endif

include $(APPDIR)/Application.mk
