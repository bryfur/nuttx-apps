############################################################################
# apps/interpreters/micropython/mpy.mk
#
# SPDX-License-Identifier: Apache-2.0
#
# MicroPython's generated headers for this port, by its own rules
# (py/py.mk, py/mkrules.mk), run from the Makefile:
#
#   make -f mpy.mk MICROPYTHON_TOP=micropython BUILD=build CC=... CPP=... \
#        SRC_QSTR_PORT="the sources" NUTTX_CONFIG_H=.../config.h
#
# with the compiler's flags in $(BUILD)/cflags.  Makes $(BUILD)/genhdr/:
# mpversion.h, qstrdefs.generated.h, moduledefs.h, root_pointers.h.
#
############################################################################

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
