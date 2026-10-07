############################################################################
# apps/interpreters/micropython/mpy.mk
#
# SPDX-License-Identifier: Apache-2.0
#
# MicroPython's generated headers for this port, by its own rules
# (py/py.mk, py/mkrules.mk), run from the Makefile:
#
#   make -f mpy.mk MICROPYTHON_TOP=micropython BUILD=build CC=... CPP=... \
#        SRC_QSTR_PORT="the sources" NUTTX_CONFIG_H=.../config.h \
#        [FROZEN_MANIFEST_PY=manifest.py FROZEN_C=frozen.c FROZEN_DEPS=...]
#
# with the compiler's flags in $(BUILD)/cflags.  Makes $(BUILD)/genhdr/:
# mpversion.h, qstrdefs.generated.h, moduledefs.h, root_pointers.h.
#
# With FROZEN_MANIFEST_PY, also FROZEN_C: the manifest's modules frozen as
# bytecode.  mpy-cross, built for this computer from the same (patched)
# sources, compiles them; mpy-tool makes the C against this build's qstrs
# (so FROZEN_C is made again whenever those change).
#
############################################################################

# Our mpy-cross, built by our rule (mkenv.mk's would build it with this
# build's compiler and flags)

MICROPY_MPYCROSS = $(MICROPYTHON_TOP)/mpy-cross/build/mpy-cross

include $(MICROPYTHON_TOP)/py/mkenv.mk

CFLAGS := $(shell cat $(BUILD)/cflags)

QSTR_DEFS = qstrdefsport.h
QSTR_GLOBAL_DEPENDENCIES += $(BUILD)/cflags $(NUTTX_CONFIG_H)

include $(TOP)/py/py.mk

# The port's sources (py's own are already there)

SRC_QSTR += $(filter-out $(TOP)/py/%,$(SRC_QSTR_PORT))

GENHDR = $(addprefix $(HEADER_BUILD)/,mpversion.h qstrdefs.generated.h \
                                      moduledefs.h root_pointers.h)

all: $(GENHDR)

include $(TOP)/py/mkrules.mk

ifneq ($(FROZEN_MANIFEST_PY),)

all: $(FROZEN_C)

# For this computer, with nothing of the NuttX build's (its compiler and
# flags come in MAKEFLAGS and the environment)

$(MICROPY_MPYCROSS):
	$(Q)echo "Building mpy-cross"
	$(Q)env -i PATH="$$PATH" make -s -j8 -C $(TOP)/mpy-cross >/dev/null

$(FROZEN_C): $(HEADER_BUILD)/qstrdefs.generated.h $(MICROPY_MPYCROSS) \
             $(FROZEN_MANIFEST_PY) $(FROZEN_DEPS)
	$(Q)echo "Freezing $(FROZEN_MANIFEST_PY) as bytecode"
	$(Q)rm -f $@
	$(Q)MICROPY_MPYCROSS=$(MICROPY_MPYCROSS) $(PYTHON) \
		$(TOP)/tools/makemanifest.py -o $(abspath $@) -b $(BUILD) \
		-v MPY_DIR=$(TOP) -v PORT_DIR=$(CURDIR) \
		--mpy-tool-flags=-mmpz-dig-size=$(MPZ_DIG_SIZE) \
		$(FROZEN_MANIFEST_PY) >/dev/null

# The target's long integers' digits (32 bits on a 64-bit simulator)

MPZ_DIG_SIZE = $(shell echo MPZ_DIG_SIZE | $(CPP) $(CFLAGS) -include \
               py/mpconfig.h -include py/mpz.h - | tail -1 | tr -d '() ')

endif
